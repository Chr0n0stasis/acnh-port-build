#!/usr/bin/env python3
# 以独立 Python 解码器生成真实 NSO 的 RELR 期望值，再交给生产 C 实现逐项核对。
# Independently decode real NSO RELR expectations in Python, then check every slot with the production C implementation.
import struct
import subprocess
import sys
from pathlib import Path


def fixtures(exefs, patched, verify, output):
    sys.path.insert(0, str(verify))
    import nsoimg
    output.mkdir(parents=True, exist_ok=True)
    result = []
    for module in ('main', 'subsdk0', 'sdk'):
        image = nsoimg.load(exefs / module)
        mod = nsoimg.mod0(image)
        at = mod + struct.unpack_from('<i', image, mod + 4)[0]
        tags = {}
        while True:
            tag, value = struct.unpack_from('<QQ', image, at); at += 16
            if not tag: break
            tags[tag] = value
        assert tags.get(37) == 8
        table = image[tags[36]:tags[36] + tags[35]]
        offsets, cursor = [], None
        for entry, in struct.iter_unpack('<Q', table):
            if entry & 1:
                assert cursor is not None
                offsets.extend(cursor + 8 * (bit - 1) for bit in range(1, 64) if entry & (1 << bit))
                cursor += 504
            else:
                offsets.append(entry); cursor = entry + 8
        assert len(set(offsets)) == len(offsets)
        assert not set(offsets).intersection(r[0] for r in nsoimg.relocs(image))
        layout = nsoimg.header(patched, module)
        size = ((patched / (module + '.patched.bin')).stat().st_size + 0x3fff) & ~0x3fff
        path = output / (module + '.relr')
        with path.open('wb') as stream:
            stream.write(b'DRELR01\0')
            stream.write(struct.pack('<6Q', size, (layout['TEXT_END'] + 0x3fff) & ~0x3fff,
                                     layout['PATCH_OFF'], tags[36], len(table), len(offsets)))
            stream.write(table)
            for offset in offsets:
                stream.write(struct.pack('<QQ', offset, struct.unpack_from('<Q', image, offset)[0]))
        result.append(path)
    return result


if __name__ == '__main__':
    executable, exefs, patched, verify, output = map(Path, sys.argv[1:])
    paths = fixtures(exefs, patched, verify, output)
    subprocess.run([str(executable.resolve()), *map(str, paths)], check=True)
