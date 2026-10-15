"""NVN 驱动段直接读取及函数参数别名读取的保守统计。
Conservative census of direct and function-parameter NVN driver-buffer reads.
"""
import re

# SPIRV-Cross 在辅助函数中去掉描述集包装，但保留各阶段的 c0/c2 名字。
# SPIRV-Cross drops the descriptor wrapper in helper functions but preserves stage-specific c0/c2 names.
READ = re.compile(r'\b(?:vp|fp|cp|gp|tcp|tep)_c([02])\s*\)*\s*\.\s*_m0\s*\[([^\]]+)\]')


def reads(source):
    result = {}
    for match in READ.finditer(source):
        bank, index = match.groups()
        entry = result.setdefault(bank, {'vec4_offsets': set(), 'dynamic': False})
        index = index.strip().rstrip('uU')
        if re.fullmatch(r'(?:0[xX][0-9a-fA-F]+|[0-9]+)', index):
            entry['vec4_offsets'].add(int(index, 16 if index.lower().startswith('0x') else 10) * 16)
        else:
            entry['dynamic'] = True
    return {bank: {'vec4_offsets': sorted(entry['vec4_offsets']), 'dynamic': entry['dynamic']}
            for bank, entry in result.items()}


def unfilled(reads, ranges):
    result = []
    for bank, entry in reads.items():
        if entry['dynamic']:
            result.append(f'c{bank} 动态下标')
        for offset in entry['vec4_offsets']:
            # 每行完整 16 字节都要有定义；表间的未初始化空洞不能借相交检查蒙混通过。
            # Require the full 16-byte row to be defined; overlap cannot hide uninitialized gaps.
            if not all(any(start <= byte < end for start, end in ranges.get(int(bank), ()))
                       for byte in range(offset, offset + 16)):
                result.append(f'c{bank}+{offset:#x}')
    return result
