import json
import sys
from pathlib import Path

mac, ios = map(Path, sys.argv[1:3])
bringup = '--bringup' in sys.argv[3:]
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
if deferred:
    Path('output/BRINGUP-LIMITATIONS.json').write_text(json.dumps({
        'status': 'experimental bring-up only, not a complete or verified playable port',
        'deferred_geometry_shaders': deferred,
        'requires_device_test': True}, indent=2))
print(f'Compiled and checked {len(checked)} macOS/iOS shader libraries; deferred geometry: {len(deferred)}')
