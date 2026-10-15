"""动森资源独立 ZIP 分卷；原资源只读，清单供 iOS 导入器逐项校验。
Independent ACNH ZIP volumes; originals are read-only, with manifests for the iOS importer.
"""
import argparse
import hashlib
import json
import shutil
import uuid
import zipfile
from pathlib import Path


def package(romfs, output, part_bytes=int(3.76 * 1024**3), title='01006F8002326000', build='v2228224'):
    romfs, output = Path(romfs).resolve(), Path(output).resolve()
    if output == romfs or romfs in output.parents:
        raise ValueError('Output must be outside the original RomFS')
    if not (romfs / 'Shader/Park_UBER_All.Nin_NX_NVN.szs').is_file():
        raise ValueError('ACNH RomFS marker missing')
    files = sorted(p for p in romfs.rglob('*') if p.is_file())
    groups, current, cost = [], [], 65536
    total_bytes = 0
    for path in files:
        if path.is_symlink():
            raise ValueError(f'Symlink is not a resource file: {path}')
        relative = path.relative_to(romfs).as_posix()
        if any(p in ('', '.', '..') for p in relative.split('/')) or '\\' in relative or ':' in relative:
            raise ValueError(f'Unsupported resource name: {relative}')
        size = path.stat().st_size
        estimate = size + 2048 + 4 * len(relative.encode('utf8'))
        if estimate + 65536 > part_bytes:
            raise ValueError(f'File exceeds volume limit: {relative}')
        if current and cost + estimate > part_bytes:
            groups.append(current); current, cost = [], 65536
        current.append((path, relative, size)); cost += estimate; total_bytes += size
    if current:
        groups.append(current)
    output.mkdir(parents=True, exist_ok=True)
    if any(output.glob('ACNH-resources.part*.zip')) or (output / 'resource-pack.json').exists():
        raise ValueError('Output already contains a resource pack; choose a new directory')
    pack_id = str(uuid.uuid4())
    identity = dict(schema=1, title_id=title, build=build, pack_id=pack_id,
                    parts=len(groups), total_files=len(files), total_bytes=total_bytes)
    result = dict(identity, volumes=[])
    for index, group in enumerate(groups, 1):
        name = f'ACNH-resources.part{index:02d}.zip'
        target = output / name
        temporary = output / (name + '.partial')
        rows = []
        with zipfile.ZipFile(temporary, 'w', compression=zipfile.ZIP_STORED, allowZip64=True) as archive:
            for path, relative, size in group:
                before = path.stat()
                digest = hashlib.sha256()
                with path.open('rb') as source, archive.open('romfs/' + relative, 'w', force_zip64=True) as destination:
                    while chunk := source.read(1 << 20):
                        digest.update(chunk); destination.write(chunk)
                after = path.stat()
                if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                    raise RuntimeError(f'Source changed while packing: {relative}')
                rows.append(dict(path=relative, size=size, sha256=digest.hexdigest()))
            archive.writestr('acnh-volume.json', json.dumps(dict(identity, part=index, files=rows), separators=(',', ':')).encode())
        if temporary.stat().st_size > part_bytes:
            raise RuntimeError('Volume overhead estimate was insufficient')
        with zipfile.ZipFile(temporary) as archive:
            if bad := archive.testzip():
                raise RuntimeError(f'CRC validation failed: {bad}')
        with temporary.open('rb') as stream:
            sha256 = hashlib.file_digest(stream, 'sha256').hexdigest()
        temporary.replace(target)
        result['volumes'].append(dict(file=name, part=index, bytes=target.stat().st_size, sha256=sha256))
        print(f'{index}/{len(groups)} {name}: {target.stat().st_size / 1024**3:.3f} GiB; {len(rows)} files; CRC checked', flush=True)
    manifest = output / 'resource-pack.json'
    manifest.write_text(json.dumps(result, indent=2), encoding='utf8')
    print(f'Completed {len(groups)} volumes, {len(files)} files, {total_bytes} resource bytes', flush=True)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--romfs', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--part-gib', type=float, default=3.76)
    args = parser.parse_args()
    package(args.romfs, args.output, int(args.part_gib * 1024**3))
