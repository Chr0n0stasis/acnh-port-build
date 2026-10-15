"""Add byte-identified device captures to the normal offline shader build."""
from pathlib import Path
import hashlib
import json
import struct

captures = Path('work/captured-shaders')
profile = Path('source/dm/games/acnh')
packages = Path('work/shader-pkg')
mapping = profile / 'shader_map.tsv'
known = {line.split('\t')[3] for line in mapping.read_text(encoding='utf-8').splitlines()
         if line.strip() and not line.startswith('#')}
records = {'vs': bytearray(), 'ps': bytearray()}
keys = {'vs': [], 'ps': []}
rows = []
for path in sorted(captures.glob('*.ctl')):
    control, code = path.read_bytes(), path.with_suffix('.code').read_bytes()
    key = hashlib.sha256(control + code).hexdigest()[:16]
    assert len(control) == 0x850 and control[:4] == bytes.fromhex('34127698')
    assert key == path.stem and struct.unpack_from('<I', control, 0x704)[0] == len(code)
    assert code[:4] == bytes.fromhex('78563412')
    stage = {1: 'vs', 5: 'ps'}[struct.unpack_from('<I', code, 0x30)[0] >> 10 & 15]
    if key in known:
        continue
    ordinal = len(keys[stage]); keys[stage].append(key)
    header = bytearray(b'BTCDBTCD' + bytes(0x30))
    struct.pack_into('<II', header, 0x28, len(control), len(code))
    records[stage].extend(header + code + control)
    rows.append(f'{stage}_capture.bin\t{ordinal}\t' + ('顶点' if stage == 'vs' else '像素') +
                f'\t{key}\tdevice-capture-20261016/{key}\n')
assert rows, 'No new captured shaders found'
for stage, data in records.items():
    (packages / f'{stage}_capture.bin').write_bytes(data)
with mapping.open('a', encoding='utf-8') as stream:
    stream.write('\n' + ''.join(rows))
# These are conservative interface-padding relationships, not observed PSOs.
# Additional unused vertex outputs preserve the default-value rule used by vs_pad.
with (profile / 'shader_pairs.tsv').open('a', encoding='utf-8') as stream:
    stream.write('\n# Captured shaders: conservative interface padding; actual PSO pairs not yet captured.\n')
    for vertex in keys['vs']:
        for pixel in keys['ps']:
            stream.write(f'{vertex}\t{pixel}\n')
Path('output/captured-shaders.json').write_text(json.dumps(dict(
    count=len(rows), keys=keys, identity='sha256(control[0:0x850] + code)[:16]',
    pairing='conservative interface padding, not measured PSO relationships'), indent=2))
print(f'Imported {len(rows)} captured shaders: {len(keys["vs"])} vertex, {len(keys["ps"])} fragment')
