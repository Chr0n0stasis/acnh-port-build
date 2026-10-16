#!/usr/bin/env python3
# VS/GS 配对离线降为两段计算与透传 VS；描述真实 MSL 参数布局，不把无 Metal 库的转译当作成功。
# Lower paired VS/GS offline into two compute stages and a passthrough VS; describe the actual MSL argument layout, never treating translation without Metal libraries as success.
import hashlib
import json
import re
import struct
import subprocess
from pathlib import Path

ROLES = ('vertex_compute', 'geometry_compute', 'passthrough_vertex')
RESERVED = {
    'vb_info': ('info', 'buffer'), 'vertex_output': ('vertex_output', 'buffer'),
    'geometry_vb_output': ('geometry_output', 'buffer'),
    'geometry_ib_output': ('geometry_indices', 'buffer'), 'vb_input': ('geometry_output', 'buffer'),
    'ib_data': ('indices', 'texture'), 'trb_data': ('topology', 'texture'),
}


def pairs(path):
    path = Path(path)
    if not path.exists():
        return []
    result = []
    for line in path.read_text(encoding='utf8').splitlines():
        if not line.strip() or line.startswith('#'):
            continue
        vs, gs, ps, *source = line.split('\t')
        if not all(re.fullmatch('[0-9a-f]{16}', key) for key in (vs, gs, ps)):
            raise ValueError(f'{path}: invalid VS/GS/PS identity')
        result.append(dict(vs=vs, gs=gs, ps=ps, source='\t'.join(source)))
    if len({r['gs'] for r in result}) != len(result):
        raise ValueError(f'{path}: ambiguous geometry pair')
    return result


def argument_sets(source):
    """只接受当前宿主能绑定的明确资源；新资源类型必须先实现再放行。
    Accept only explicit resources the host can bind; implement new resource kinds before admitting them.
    """
    sets = []
    for number, body in re.findall(r'struct spvDescriptorSetBuffer(\d+)\s*\{(.*?)\};', source, re.S):
        bindings = []
        for declaration, name, aid in re.findall(r'([^;]+?)\s+(\w+)\s*\[\[id\((\d+)\)\]\];', body):
            declaration = declaration.strip()
            resource = 'texture' if declaration.startswith('texture_buffer<') else 'buffer' if '*' in declaration else None
            b = dict(id=int(aid), name=name, resource=resource)
            if name in RESERVED:
                b['kind'], expected = RESERVED[name]
                if resource != expected:
                    raise ValueError(f'{name}: unexpected resource type {declaration}')
            elif m := re.fullmatch('vb_data(\d+)', name):
                if resource != 'texture' or declaration != 'texture_buffer<float>':
                    raise ValueError(f'{name}: unsupported attribute component type')
                b.update(kind='attribute', location=int(m[1]))
            elif m := re.fullmatch('[vg]p_c(\d+)', name):
                slot = int(m[1])
                if resource != 'buffer' or slot not in (1, *range(3, 19)):
                    raise ValueError(f'{name}: unsupported geometry constant bank')
                b.update(kind='c1' if slot == 1 else 'uniform', slot=slot, reg=slot - 3)
            elif name == 'support_buffer' and resource == 'buffer':
                b['kind'] = 'support'
            else:
                raise ValueError(f'unsupported geometry resource {name}: {declaration}')
            bindings.append(b)
        if not bindings or len({b['id'] for b in bindings}) != len(bindings):
            raise ValueError(f'empty or duplicate argument set {number}')
        sets.append(dict(index=int(number), words=1 + max(b['id'] for b in bindings), bindings=bindings))
    if not sets or len({s['index'] for s in sets}) != len(sets) or any(s['index'] > 2 for s in sets):
        raise ValueError('unsupported geometry argument set layout')
    return sorted(sets, key=lambda s: s['index'])


def execution_model(code):
    if len(code) % 4:
        raise ValueError('unaligned SPIR-V')
    words = struct.unpack('<%dI' % (len(code) // 4), code)
    if len(words) < 5 or words[0] != 0x07230203:
        raise ValueError('invalid SPIR-V')
    pos, models = 5, []
    while pos < len(words):
        count, opcode = words[pos] >> 16, words[pos] & 65535
        if not count or pos + count > len(words):
            raise ValueError('invalid SPIR-V instruction')
        if opcode in (218, 219):
            raise ValueError('native geometry emission remains after lowering')
        if opcode == 15:
            models.append(words[pos + 1])
        pos += count
    if len(models) != 1:
        raise ValueError('expected exactly one SPIR-V entry point')
    return models[0]


def build(jobs, output, chain, translator, spirv_cross='spirv-cross', ios_output=None, compile_libraries=True, compile_mac=True):
    if compile_libraries and not compile_mac and not ios_output:
        raise ValueError('At least one Metal target is required')
    output = Path(output); output.mkdir(parents=True, exist_ok=True)
    if ios_output:
        ios_output = Path(ios_output); ios_output.mkdir(parents=True, exist_ok=True)
    job_path = output / '_geometry_jobs.json'
    job_path.write_text(json.dumps(jobs), encoding='utf8')
    subprocess.run([str(translator), '--geometry', str(job_path), str(output)], check=True)
    manifests = []
    for job in jobs:
        key = job['key']
        metadata = json.loads((output / (key + '.geometry.json')).read_text(encoding='utf8'))
        if metadata['notes']['vertex'] or metadata['notes']['geometry']:
            raise ValueError(f'{key}: translator reported unresolved queries: {metadata["notes"]}')
        if metadata['vertex_info_bytes'] != 1056 or metadata['vertices_per_primitive'] != 3 or metadata['invocations'] != 1:
            raise ValueError(f'{key}: unsupported geometry layout')
        metadata.update(format=1, chain=chain, compiled=False, programs={})
        metadata['index_stride'] = metadata['max_output_vertices'] + metadata['max_output_vertices'] // 3
        for role in ROLES:
            name = key + '_' + role
            spv = output / (name + '.spv'); msl = output / (name + '.metal')
            expected_model = 0 if role == 'passthrough_vertex' else 5
            if execution_model(spv.read_bytes()) != expected_model:
                raise ValueError(f'{name}: wrong execution model')
            subprocess.run([str(spirv_cross), str(spv), '--msl', '--msl-version', '20300',
                            '--msl-argument-buffers', '--msl-texture-buffer-native', '--output', str(msl)], check=True)
            source = msl.read_text(encoding='utf8')
            arguments = argument_sets(source)
            info = json.loads((output / (name + '.res.json')).read_text(encoding='utf8'))
            if any(info[k] for k in ('textures', 'images', 'sbufs')):
                raise ValueError(f'{name}: guest textures/images/storage are not supported in the geometry path')
            p = dict(library=name + '.metallib', entry='main0', sets=arguments,
                     component='vertex' if expected_model == 0 else 'compute', source_sha256=hashlib.sha256(msl.read_bytes()).hexdigest())
            if role != 'passthrough_vertex':
                p['c1'] = job['vertex' if role == 'vertex_compute' else 'geometry']['c1']
                if len(bytes.fromhex(p['c1'])) > 65536:
                    raise ValueError(f'{name}: constant bank exceeds 64 KiB')
            metadata['programs'][role] = p
            if compile_libraries:
                if compile_mac:
                    library = output / p['library']; air = output / (name + '.air')
                    subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', str(msl), '-o', str(air)], check=True)
                    subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(air), '-o', str(library)], check=True)
                    if library.read_bytes()[:4] != b'MTLB':
                        raise ValueError(f'{library}: not a compiled library')
                if ios_output:
                    import ios_metal
                    ios_library = ios_output / p['library']
                    ios_metal.compile(str(msl), str(ios_output / (name + '.air')), str(ios_library))
                    (ios_output / (name + '.air')).unlink(missing_ok=True)
                    if ios_library.read_bytes()[:4] != b'MTLB':
                        raise ValueError(f'{ios_library}: not a compiled library')
        metadata['compiled'] = bool(compile_libraries)
        metadata['compiled_targets'] = (['macos'] if compile_mac else []) + (['ios'] if ios_output else []) if compile_libraries else []
        destination = output / (key + '.geometry.json')
        destination.write_text(json.dumps(metadata, indent=2), encoding='utf8')
        if compile_libraries and ios_output:
            (ios_output / destination.name).write_bytes(destination.read_bytes())
        manifests.append(dict(vs=metadata['vs'], gs=metadata['gs'], ps=metadata['ps'], metadata=destination.name))
    manifest = dict(format=1, chain=chain, compiled=bool(compile_libraries), pairs=manifests)
    (output / '_geometry_manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf8')
    if compile_libraries and ios_output:
        (ios_output / '_geometry_manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf8')
    job_path.unlink()
    return manifest
