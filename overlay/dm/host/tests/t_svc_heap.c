// 重现动森 QueryMemoryInfo → SetHeapSize 启动链，区分地址容量与应用池记账；只触碰堆首尾页。
// Reproduce ACNH's QueryMemoryInfo -> SetHeapSize boot path, distinguishing address capacity from pool accounting; touch only the boundary pages.
#include "../hle_svc.c"

module_t g_mod[MAX_MOD];
int g_nmod;
struct dm_flags g_flag;
void hlecount_svc(int n) { (void)n; }
int svc_thread(int n, uint64_t *r) { (void)n; (void)r; return 0; }
uint64_t svc_host_call(int n, uint64_t *r) { (void)n; (void)r; return 0; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void dm_backtrace(uint64_t fp) { (void)fp; }
uint64_t guest_ns(void) { return 0; }
uint64_t dm_entropy64(void) { return 0; }

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); } } while (0)

static uint64_t info(uint64_t id) {
    uint64_t r[32] = {0}; r[1] = id;
    svc_dispatch(0x29, r, 0); CHECK(r[0] == 0); return r[1];
}
static uint64_t heap(size_t size, uint64_t *base) {
    uint64_t r[32] = {0}; r[1] = size;
    svc_dispatch(1, r, 0); if (base) *base = r[1]; return r[0];
}

int main(void) {
    // 来自用户启动日志的三个已改写模块大小，不需要原始游戏文件。
    // Three rewritten module sizes from the user's boot log; no original game files required.
    g_nmod = 3;
    g_mod[0].size = 0x8264000; g_mod[1].size = 0x6d4000; g_mod[2].size = 0xdc0000;
    CHECK(image_bytes() == 0x9800000);
    uint64_t total = info(I_TotalNonSys), used = info(I_UsedNonSys);
    CHECK(total == APP_POOL && used == image_bytes());
    CHECK(info(I_UsedMem) == used);
    CHECK(info(I_HeapSize) >= 0xc9400000ull);
    CHECK(g_heap >= g_stack + g_stack_cap && g_heap + g_heap_cap == g_as + RESERVE);
    uint64_t base = 0, wanted = (total - used) & ~0x1fffffull;
    CHECK(wanted == 0xc3c00000ull);
    CHECK(heap(wanted, &base) == 0);
    CHECK(base == (uint64_t)g_heap && g_heap_cur == wanted);
    CHECK(dm_guest_heap_readable(base, wanted));
    CHECK(!dm_guest_heap_readable(base, wanted + 1));
    g_heap[0] = 0x12; g_heap[wanted - 1] = 0x34;
    CHECK(g_heap[0] == 0x12 && g_heap[wanted - 1] == 0x34);
    CHECK(info(I_UsedNonSys) == wanted + image_bytes());
    CHECK(info(I_TotalNonSys) - info(I_UsedNonSys) < 0x200000);
    CHECK(heap(wanted + 0x200000, NULL) == 0x10801);
    CHECK(g_heap_cur == wanted);
    CHECK(heap(g_heap_cap + 0x200000, NULL) != 0 && g_heap_cur == wanted);
    CHECK(heap(0, NULL) == 0 && g_heap_cur == 0);
    CHECK(info(I_UsedNonSys) == image_bytes());
    CHECK(!dm_guest_heap_readable(base, 1));
    CHECK(munmap(g_as, RESERVE) == 0); free(g_alias_bits);
    printf("SVC_HEAP checks=%d failures=%d requested=%#llx cap=%#zx\n", checks, failures, wanted, g_heap_cap);
    return failures != 0;
}
