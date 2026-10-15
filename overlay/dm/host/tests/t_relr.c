// RELR 编码、拒绝时不写入，以及真实表在不同装载基址下的逐槽回归；不执行游戏代码。
// RELR encoding, no writes on rejection, and per-slot regressions of real tables at different load bases; no guest code is executed.
#include "../dm_relr.h"
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); } } while (0)
typedef struct { uint64_t offsets[16]; size_t n; } capture;
static int collect(void *context, uint64_t offset) {
    capture *c = context; if (c->n == 16) return 1; c->offsets[c->n++] = offset; return 0;
}
static void unit_tests(void) {
    uint64_t table[] = {0x20, 1ull | 1ull << 1 | 1ull << 3 | 1ull << 63, 1ull | 1ull << 2, 0x10, 3};
    const uint64_t expected[] = {0x20, 0x28, 0x38, 0x218, 0x228, 0x10, 0x18};
    capture c = {{0},0}; size_t n = 99;
    CHECK(dm_relr_walk(table, sizeof table, 0x1000, collect, &c, &n) == 0);
    CHECK(n == 7 && c.n == 7 && !memcmp(c.offsets, expected, sizeof expected));
    uint64_t empty_bitmap[] = {0x20, 1, 3}; c.n = 0;
    CHECK(dm_relr_walk(empty_bitmap, sizeof empty_bitmap, 0x1000, collect, &c, &n) == 0);
    CHECK(n == 2 && c.offsets[1] == 0x220);
    CHECK(dm_relr_walk(NULL, 0, 0, NULL, NULL, &n) == 0 && n == 0);
    CHECK(dm_relr_walk(NULL, 8, 0x1000, NULL, NULL, &n) == DM_RELR_BAD_TABLE);
    CHECK(dm_relr_walk(table, 7, 0x1000, NULL, NULL, &n) == DM_RELR_BAD_TABLE);
    uint64_t bitmap_first[] = {3}, out_of_bounds[] = {0xff8, 3};
    CHECK(dm_relr_walk(bitmap_first, 8, 0x1000, NULL, NULL, &n) == DM_RELR_BAD_ENCODING);
    CHECK(dm_relr_walk(out_of_bounds, 16, 0x1000, NULL, NULL, &n) == DM_RELR_BAD_TARGET);

    uint8_t image[4096], original[4096]; uint64_t bad;
    uint64_t targets[][2] = {{0x100,0x20}, {0x100,0xff8}, {0x100,0x800}, {0x100,0x7fc}};
    for (unsigned i=0; i<sizeof targets/sizeof *targets; i++) {
        memset(image,0xa5,sizeof image); memcpy(image+0x800,targets[i],16); memcpy(original,image,sizeof image);
        CHECK(dm_relr_relocate(image,sizeof image,0x800,16,0x100,0x800,0x100000000ull,&n,&bad) != 0);
        CHECK(n == 0 && !memcmp(image,original,sizeof image));
    }
    uint64_t valid[] = {0x100,3}, value = 0x1234;
    memset(image,0,sizeof image); memcpy(image+0x800,valid,16); memcpy(image+0x100,&value,8);
    CHECK(dm_relr_relocate(image,sizeof image,0x800,16,0x100,0x800,0x11245c000ull,&n,&bad) == 0 && n == 2);
    memcpy(&value,image+0x100,8); CHECK(value == 0x11245d234ull);
    memcpy(&value,image+0x108,8); CHECK(value == 0x11245c000ull);
}

typedef struct { uint64_t offset, value; } slot;
static int fixture(const char *path) {
    FILE *f = fopen(path,"rb"); if (!f) { perror(path); return 1; }
    char magic[8]; uint64_t h[6];
    if (fread(magic,1,8,f)!=8 || memcmp(magic,"DRELR01",8) || fread(h,8,6,f)!=6 ||
        h[3]>h[0] || h[4]>h[0]-h[3] || h[5]>SIZE_MAX/sizeof(slot)) { fclose(f); return 1; }
    uint8_t *image = calloc(1,(size_t)h[0]);
    uint8_t *entries = malloc((size_t)h[4]); slot *slots = malloc((size_t)h[5]*sizeof *slots);
    if (!image || !entries || !slots || fread(entries,1,(size_t)h[4],f)!=h[4] ||
        fread(slots,sizeof *slots,(size_t)h[5],f)!=h[5]) { fclose(f); free(image); free(entries); free(slots); return 1; }
    fclose(f); const uint64_t bases[] = {0x11245c000ull,0x7000000000ull,0x200000000ull};
    for (unsigned b=0;b<sizeof bases/sizeof *bases;b++) {
        memcpy(image+h[3],entries,(size_t)h[4]);
        for (size_t i=0;i<h[5];i++) {
            if (slots[i].offset>h[0] || h[0]-slots[i].offset<8) { free(image); free(entries); free(slots); return 1; }
            memcpy(image+slots[i].offset,&slots[i].value,8);
        }
        size_t count=0; uint64_t bad=0;
        int rc=dm_relr_relocate(image,(size_t)h[0],h[3],(size_t)h[4],h[1],h[2],bases[b],&count,&bad);
        if (rc || count!=h[5]) { fprintf(stderr,"RELR fixture failed rc=%d count=%zu expected=%" PRIu64 " bad=%#" PRIx64 "\n",rc,count,h[5],bad); free(image); free(entries); free(slots); return 1; }
        for (size_t i=0;i<h[5];i++) {
            uint64_t value; memcpy(&value,image+slots[i].offset,8);
            if (value!=slots[i].value+bases[b]) { fprintf(stderr,"RELR slot %#" PRIx64 " differs\n",slots[i].offset); free(image); free(entries); free(slots); return 1; }
        }
    }
    printf("RELR %s: %" PRIu64 " pointers checked at 3 ASLR bases\n",path,h[5]);
    free(image); free(entries); free(slots); return 0;
}
int main(int argc,char **argv) {
    unit_tests();
    for (int i=1;i<argc;i++) if (fixture(argv[i])) failures++;
    printf("RELR checks=%d failures=%d\n",checks,failures); return failures!=0;
}
