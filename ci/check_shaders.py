import json
import os
import sys
from pathlib import Path

mac, ios = map(Path, sys.argv[1:3])
bringup = '--bringup' in sys.argv[3:]
integrated_geometry = os.environ.get('DM_GEOMETRY_RUNTIME') == '1'
geometry = {c[3] for line in Path('source/dm/games/acnh/shader_map.tsv').read_text().splitlines()
            if len(c := line.split('\t')) > 4 and c[2] == '几何'}
rows = json.loads((mac / '_b_build.json').read_text())
deferred = [r for r in rows if bringup and r['key'] in geometry]
checked = [r for r in rows if r not in deferred]
failed = [r for r in checked if r['st'] != 'ok']
if not rows or failed:
    raise SystemExit(f'Shader build incomplete: {len(failed)} failures / {len(rows)} shaders')
problems = []
for row in checked:
    key = row['key'] + '_b'
    for root in (mac, ios):
        lib = root / (key + '.metallib')
        if not lib.exists() or lib.read_bytes()[:4] != b'MTLB':
            problems.append(f'{root.name}/{key}: missing compiled Metal library')
    meta = json.loads((mac / (key + '.json')).read_text())
    if meta.get('msl_entry_kind') not in {'vertex', 'fragment', 'kernel'}:
        problems.append(f'{key}: no supported Metal entry point for stage {meta["stage"]}')
    if meta.get('drv_unfillable'):
        problems.append(f'{key}: unfillable driver reads {meta["drv_unfillable"]}')
    for binding in meta['bindings']:
        if binding['kind'] == '?' or (binding['kind'] in {'cbuf', 'samp', 'samp_tex', 'tex', 'img', 'tbuf', 'sbuf', 'ubo_ptr', 'tex_ubo', 'samp_ubo'} and binding.get('reg') is None):
            problems.append(f'{key}: unresolved binding {binding}')
if problems:
    Path('output/shader-problems.txt').write_text('\n'.join(problems))
    raise SystemExit(f'Shader interface validation failed: {len(problems)} problems')
geometry_programs = 0
manifest_path = mac / '_geometry_manifest.json'
if manifest_path.exists():
    manifest = json.loads(manifest_path.read_text())
    assert manifest['compiled'] and manifest['chain'] == mac.name.removeprefix('b-')
    assert {p['gs'] for p in manifest['pairs']} == geometry
    for pair in manifest['pairs']:
        metadata = json.loads((mac / pair['metadata']).read_text())
        assert metadata['compiled'] and metadata['chain'] == manifest['chain']
        if integrated_geometry:
            assert metadata['attribute_layout'] == 'draw-order-float4'
        assert metadata['vs'] == pair['vs'] and metadata['gs'] == pair['gs'] and metadata['ps'] == pair['ps']
        assert len(metadata['programs']) == 3
        for program in metadata['programs'].values():
            for directory in (mac, ios):
                assert (directory / program['library']).read_bytes()[:4] == b'MTLB'
            geometry_programs += 1
if deferred:
    Path('output/BRINGUP-LIMITATIONS.json').write_text(json.dumps({
        'status': 'experimental bring-up only, not a complete or verified playable port',
        'deferred_geometry_shaders': deferred,
        'lowered_geometry_programs_compiled': geometry_programs,
        'geometry_renderer_integrated': integrated_geometry,
        'requires_device_test': True}, indent=2))
print(f'Compiled and checked {len(checked)} macOS/iOS shader libraries; deferred geometry: {len(deferred)}')
if geometry_programs:
    print(f'Geometry lowering: {geometry_programs} compiled programs; renderer {"included; target-device verification pending" if integrated_geometry else "integration pending"}')
