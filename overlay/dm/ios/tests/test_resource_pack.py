"""Exercise the real native importer, including archive corruption and persisted partial imports."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import uuid
import zipfile

EXE = str(Path(sys.argv[1]).resolve())
MARKER = 'Shader/Park_UBER_All.Nin_NX_NVN.szs'
checks = 0


def check(value, message):
    global checks
    checks += 1
    if not value:
        raise AssertionError(message)
    print('PASS', message, flush=True)


def call(volume, documents, ok=True):
    documents.mkdir(parents=True, exist_ok=True)
    run = subprocess.run([EXE, str(volume), str(documents)], text=True, capture_output=True)
    check(run.returncode == (0 if ok else 2), f'import {volume.name}: expected {"success" if ok else "failure"}; {run.stdout.strip()} {run.stderr.strip()}')
    return json.loads(run.stdout)


with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    counter = 0

    def volume(files, part=1, parts=1, total_files=None, total_bytes=None, pack=None, override=None, extra=None, symlink=False, method=zipfile.ZIP_STORED):
        global counter
        counter += 1
        pack = pack or str(uuid.uuid4())
        rows = [dict(path=p, size=len(v), sha256=hashlib.sha256(v).hexdigest()) for p, v in files.items()]
        meta = dict(schema=1, title_id='01006F8002326000', build='v2228224', pack_id=pack, part=part, parts=parts,
                    total_files=total_files if total_files is not None else len(files),
                    total_bytes=total_bytes if total_bytes is not None else sum(map(len, files.values())), files=rows)
        if override:
            override(meta)
        path = root / f'{counter}.zip'
        with zipfile.ZipFile(path, 'w', compression=method) as z:
            for name, value in files.items():
                info = zipfile.ZipInfo('romfs/' + name)
                info.compress_type = method
                if symlink:
                    info.create_system = 3
                    info.external_attr = 0o120777 << 16
                with z.open(info, 'w', force_zip64=True) as stream:
                    stream.write(value)
            z.writestr('acnh-volume.json', json.dumps(meta))
            if extra:
                z.writestr(*extra)
        return path

    files = {MARKER: b'marker', 'Model/test.bin': os.urandom(300000), 'Model/中文.bin': b'utf8', 'empty': b''}
    pack = str(uuid.uuid4())
    a = volume(dict(list(files.items())[:2]), part=1, parts=2, total_files=4, total_bytes=sum(map(len, files.values())), pack=pack)
    b = volume(dict(list(files.items())[2:]), part=2, parts=2, total_files=4, total_bytes=sum(map(len, files.values())), pack=pack, method=zipfile.ZIP_DEFLATED)
    docs = root / 'docs'
    first = call(a, docs)
    check(not first['complete'] and not (docs / 'romfs').exists(), 'partial volume never published as romfs')
    again = call(a, docs)
    check(again['received'] == 1, 'retry does not count the same volume twice')
    complete = call(b, docs)
    check(complete['complete'], 'later process completes earlier partial import')
    check(all((docs / 'romfs' / k).read_bytes() == v for k, v in files.items()), 'all bytes including UTF-8 names and zero-length file preserved')
    check(a.exists() and b.exists(), 'manually selected source ZIPs remain intact')
    reverse = root / 'reverse'
    call(b, reverse)
    check(call(a, reverse)['complete'], 'out-of-order volumes supported')

    old = root / 'old'; (old / 'romfs').mkdir(parents=True); (old / 'romfs/old').write_bytes(b'original')
    call(a, old)
    check((old / 'romfs/old').read_bytes() == b'original', 'previous resources preserved during partial import')
    call(b, old)
    check(not (old / 'romfs/old').exists() and (old / 'romfs/Model/test.bin').read_bytes() == files['Model/test.bin'], 'complete set atomically replaces previous resources')

    wrong = volume({MARKER: b'x'}, override=lambda m: m.update(build='other'))
    call(wrong, root / 'wrong', False)
    for name in ('../escape', '/absolute', 'C:/file', 'a\\b', 'a/./b'):
        bad = volume({MARKER: b'x', name: b'escape'})
        call(bad, root / ('bad' + str(counter)), False)
    check(not (root / 'escape').exists(), 'path traversal never writes outside import staging')
    call(volume({MARKER: b'x'}, symlink=True), root / 'symlink', False)
    call(volume({MARKER: b'x'}, extra=('not-listed', b'x')), root / 'extra', False)
    call(volume({MARKER: b'x'}, override=lambda m: m['files'][0].update(sha256='0' * 64)), root / 'sha', False)
    call(volume({'a': b'x'}), root / 'missing', False)
    call(volume({MARKER: b'x'}, override=lambda m: m.update(total_files=2)), root / 'count', False)

    corrupt = volume({MARKER: b'corruption-test'})
    with zipfile.ZipFile(corrupt) as z:
        entry = z.getinfo('romfs/' + MARKER)
        offset = entry.header_offset
    data = bytearray(corrupt.read_bytes())
    name_len, extra_len = struct.unpack_from('<HH', data, offset + 26)
    data[offset + 30 + name_len + extra_len] ^= 0x80
    corrupt.write_bytes(data)
    call(corrupt, root / 'crc', False)
    check(not (root / 'crc/romfs').exists(), 'corrupt CRC does not publish resources')

    conflict_pack = str(uuid.uuid4())
    c = volume({MARKER: b'one'}, part=1, parts=2, total_files=2, total_bytes=6, pack=conflict_pack)
    d = volume({MARKER: b'two'}, part=2, parts=2, total_files=2, total_bytes=6, pack=conflict_pack)
    call(c, root / 'conflict')
    call(d, root / 'conflict', False)

    tamper = root / 'tamper'; call(a, tamper)
    (tamper / f'.acnh-import-{pack}' / MARKER).write_bytes(b'changed')
    call(b, tamper, False)
    check(not (tamper / 'romfs').exists(), 'final pass detects changes in earlier imported volume')
    check(call(a, tamper)['complete'], 'reimport repairs an earlier volume after final validation failure')

    # 真正跨过 65535 个目录项；既验证 ZIP64 读取，也验证十万文件级清单不会被截断。
    # Really cross the 65535-entry boundary, testing ZIP64 reading and large manifests.
    many = {MARKER: b'x'} | {f'Model/{i:05d}': b'' for i in range(65535)}
    large = volume(many)
    check(call(large, root / 'zip64')['complete'], 'ZIP64 archive with over 65535 entries imports correctly')

print(f'{checks} checks passed', flush=True)
