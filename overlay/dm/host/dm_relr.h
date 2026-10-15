// ELF64 RELR：偶数项是地址，奇数项是随后 63 个机器字的位图。调用者先完整校验，再进行写入。
// ELF64 RELR: even entries are addresses, odd entries bitmap the following 63 words. Callers validate the entire table before writing.
// 格式来源 | Format: https://gabi.xinuos.com/elf/06-reloc.html
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

enum { DM_RELR_OK, DM_RELR_BAD_TABLE, DM_RELR_BAD_ENCODING, DM_RELR_BAD_TARGET, DM_RELR_CALLBACK };
typedef int (*dm_relr_visit)(void *context, uint64_t offset);

static int dm_relr_walk(const void *table, size_t bytes, size_t image_size,
                        dm_relr_visit visit, void *context, size_t *count) {
    if (count) *count = 0;
    if (bytes % 8 || (bytes && !table)) return DM_RELR_BAD_TABLE;
    uint64_t cursor = 0; int have_address = 0; size_t n = 0;
    for (size_t i = 0; i < bytes; i += 8) {
        uint64_t entry; memcpy(&entry, (const uint8_t *)table + i, 8);
        if (!(entry & 1)) {
            if (entry > image_size || image_size - entry < 8) return DM_RELR_BAD_TARGET;
            if (visit && visit(context, entry)) return DM_RELR_CALLBACK;
            n++; cursor = entry + 8; have_address = 1;
        } else {
            if (!have_address || cursor > UINT64_MAX - 63 * 8) return DM_RELR_BAD_ENCODING;
            uint64_t bits = entry >> 1;
            for (unsigned bit = 0; bits; bit++, bits >>= 1) if (bits & 1) {
                uint64_t offset = cursor + (uint64_t)bit * 8;
                if (offset > image_size || image_size - offset < 8) return DM_RELR_BAD_TARGET;
                if (visit && visit(context, offset)) return DM_RELR_CALLBACK;
                n++;
            }
            cursor += 63 * 8;
        }
    }
    if (count) *count = n;
    return DM_RELR_OK;
}

typedef struct {
    uint8_t *image;
    uint64_t table, bytes, writable_start, writable_end, bias, bad;
} dm_relr_context;

static int dm_relr_validate_target(void *context, uint64_t offset) {
    dm_relr_context *r = context;
    if (offset < r->writable_start || offset > r->writable_end || r->writable_end - offset < 8 ||
        (offset < r->table + r->bytes && r->table < offset + 8)) { r->bad = offset; return 1; }
    return 0;
}
static int dm_relr_apply_target(void *context, uint64_t offset) {
    dm_relr_context *r = context; uint64_t value;
    memcpy(&value, r->image + offset, 8); value += r->bias;
    memcpy(r->image + offset, &value, 8); return 0;
}

// 所有目标通过边界/权限校验后才写；表损坏时不留下半重定位映像。
// Write only after every target passes bounds/permission checks; a malformed table leaves no partially relocated image.
static int dm_relr_relocate(void *image, size_t size, uint64_t table, size_t bytes,
                             uint64_t writable_start, uint64_t writable_end, uint64_t bias,
                             size_t *count, uint64_t *bad_target) {
    if (count) *count = 0;
    if (bad_target) *bad_target = 0;
    if (!image || table > size || bytes > size - table || writable_start > writable_end || writable_end > size)
        return DM_RELR_BAD_TABLE;
    dm_relr_context context = {(uint8_t *)image, table, bytes, writable_start, writable_end, bias, 0};
    const void *entries = (const uint8_t *)image + table;
    int error = dm_relr_walk(entries, bytes, size, dm_relr_validate_target, &context, count);
    if (error) { if (count) *count = 0; if (bad_target) *bad_target = context.bad; return error; }
    return dm_relr_walk(entries, bytes, size, dm_relr_apply_target, &context, count);
}
