// 验证 SDK 20 完整渲染入口、线程内命令捕获、PCM 裁剪和 Update 串行；不模拟整套游戏或声卡。
// Verify the SDK 20 render entry, thread-local command capture, PCM clamping and serialized Update; not the game or audio hardware.
#include "../hle_audren.c"
#include "../hle_sdkinit.c"
#include <errno.h>

static module_t sdk_module = {.build_id = "a226048605d3597f90cb1067d141cd36"};
static uint8_t commands[0x200];
static int32_t samples[8] = {40000, -40000, 100, -100, 200, -200, 32767, -32768};
static int setups, executions, updates, old_lookups, fail_render;
static void mock_setup(void *proc, const void *cl, size_t size) { (void)proc; if (cl != commands || size != sizeof commands) abort(); setups++; }
static void mock_dispatch(void *proc, const uint8_t *command) { (void)proc; (void)command; }
static Result mock_exec(void *sys) {
    (void)sys; executions++;
    if (pthread_mutex_trylock(&g_render_mu) != EBUSY) abort();
    if (fail_render) return 0x40299;
    h_ar_ProcessorSetup(NULL, commands, sizeof commands);
    h_ar_Dispatch(NULL, commands + 0x20);
    // 后续命令复用输入缓冲，已经取出的声音必须保持原值。
    // Later commands reuse the input buffers; the captured audio must retain the original values.
    memset(samples, 0, sizeof samples);
    return 0;
}
static Result mock_update(void *sys, void *o, size_t on, void *p, size_t pn, const void *in, size_t inn) {
    (void)sys; (void)o; (void)on; (void)p; (void)pn; (void)in; (void)inn;
    if (pthread_mutex_trylock(&g_render_mu) != EBUSY) abort();
    updates++; return 0;
}
void *guest_sym(const char *name) {
    if (strstr(name, "GenerateCommandImpl")) return &sdk_module;
    if (!strcmp(name, "_ZN2nn5audio6server17AudioRenderSystem29ExecuteAudioRendererRenderingEv")) return (void *)mock_exec;
    if (strstr(name, "CommandListProcessor5Setup")) return (void *)mock_setup;
    if (strstr(name, "CommandListProcessor15DispatchCommand")) return (void *)mock_dispatch;
    if (strstr(name, "AudioRenderSystem6Update")) return (void *)mock_update;
    old_lookups++; abort();
}
module_t *module_of(const void *p) { return p == &sdk_module ? &sdk_module : NULL; }
uint64_t now_ns(void) { return 0; }
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack_size) { (void)fn; (void)arg; (void)prio; (void)stack_size; abort(); }
void *dm_pcmout_open(int ch) { (void)ch; abort(); }
void dm_pcmout_start(void *k) { (void)k; abort(); }
int dm_pcmout_append(void *k, void *b) { (void)k; (void)b; abort(); }
void *dm_pcmout_released(void *k) { (void)k; abort(); }
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; abort(); }

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d\n", __LINE__); return 1; } checks++; } while (0)
int main(void) {
    int checks = 0, n = 0; int16_t out[8] = {0};
    *(uint32_t *)(commands + 4) = 1;
    *(const int32_t **)(commands + 8) = samples;
    *(uint32_t *)(commands + 0x10) = 2;
    *(uint32_t *)(commands + 0x14) = 4;
    *(uint32_t *)(commands + 0x18) = 48000;
    uint8_t *sink = commands + 0x20;
    *(uint32_t *)sink = 0xcafebabeu; sink[4] = 1; sink[5] = 0x14;
    *(int16_t *)(sink + 6) = 0x148;
    *(uint32_t *)(sink + 0x130) = 2;
    *(int16_t *)(sink + 0x136) = 1;
    CHECK(h_applet_ResourceId() == 1 && h_am_ResourceId() == h_applet_ResourceId());
    CHECK(render_once(NULL, out, &n) == 0 && n == 4 && setups == 1 && executions == 1);
    const int16_t expected[8] = {32767, 200, -32768, -200, 100, 32767, -100, -32768};
    CHECK(!memcmp(out, expected, sizeof expected));
    CHECK(!g_capture.active && !g_capture.commands && old_lookups == 0);
    memcpy(samples, (int32_t[]){40000, -40000, 100, -100, 200, -200, 32767, -32768}, sizeof samples);
    g_frame_n = 0;
    // 实际虚表尾调进入 SystemExec，必须同样持锁并捕获本轮 PCM。
    // The real vtable tail call enters SystemExec and must hold the lock and capture this frame's PCM too.
    CHECK(h_ar_SystemExec(NULL) == 0 && g_frame_n == 4 && executions == 2);
    CHECK(!memcmp(g_frame, expected, sizeof expected));
    h_ar_ProcessorSetup(NULL, commands, sizeof commands);
    CHECK(!g_capture.commands);
    CHECK(h_ars_Update(NULL, NULL, 0, NULL, 0, NULL, 0) == 0 && updates == 1);
    CHECK(h_ars_Update(NULL, NULL, 0, NULL, 0, NULL, 0) == 0 && updates == 2 && atomic_load(&g_catchups) == 1);
    fail_render = 1; n = 0;
    CHECK(render_once(NULL, out, &n) == 0x40299 && n == 0 && !g_capture.active);
    CHECK(pthread_mutex_trylock(&g_render_mu) == 0); pthread_mutex_unlock(&g_render_mu);
    strcpy(sdk_module.build_id, "unknown"); CHECK(!sdk20_audio());
    printf("audren SDK20: %d checks passed; SDK calls mocked, no device audio claim\n", checks);
    return 0;
}
