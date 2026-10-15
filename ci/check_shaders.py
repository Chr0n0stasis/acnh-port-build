import json
import sys
from pathlib import Path

mac, ios = map(Path, sys.argv[1:3])
rows = json.loads((mac / '_b_build.json').read_text())
failed = [r for r in rows if r['st'] != 'ok']
if not rows or failed:
    raise SystemExit(f'Shader build incomplete: {len(failed)} failures / {len(rows)} shaders')
problems = []
for row in rows:
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
print(f'Compiled and checked {len(rows)} macOS/iOS shader libraries')
