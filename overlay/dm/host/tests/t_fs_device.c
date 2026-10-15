// 设备存档容量跨调用持久化、无效请求及损坏记录检查；不需要游戏文件。
// Device-save capacity persistence, invalid requests and corrupt-record checks; no game files required.
#include "../hle_fs_device.c"
#include <sys/stat.h>

struct dm_flags g_flag; uint64_t g_t0_ns;
uint64_t now_ns(void) { return 0; }
static int failures, checks;
#define CHECK(value) do { checks++; if (!(value)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); } } while (0)

int main(void) {
    char root[] = "/tmp/acnh-device-save-XXXXXX"; CHECK(mkdtemp(root) != NULL);
    char save[1200], path[1200]; snprintf(save, sizeof save, "%s/island", root); setenv("DM_SAVE", save, 1);
    int64_t data = -1, journal = -1;
    CHECK(h_fs_DevGetSize(&data, &journal) == RES_OK && data == 116391936 && journal == 116391936);
    CHECK(h_fs_DevGetSize(NULL, &journal) != RES_OK);
    CHECK(h_fs_DevExtend(116408320, 116424704) == RES_OK);
    data = journal = -1;
    CHECK(h_fs_DevGetSize(&data, &journal) == RES_OK && data == 116408320 && journal == 116424704);
    CHECK(h_fs_DevExtend(116408320, 116424704) == RES_OK);
    CHECK(h_fs_DevExtend(116391936, 116424704) != RES_OK);
    CHECK(h_fs_DevExtend(116408321, 116424704) != RES_OK);
    CHECK(h_fs_DevExtend(-1, 116424704) != RES_OK);
    CHECK(h_fs_DevExtend(603996160, 116424704) != RES_OK);
    CHECK(h_fs_DevGetSize(&data, &journal) == RES_OK && data == 116408320 && journal == 116424704);
    CHECK(quota_path(path));
    if (geteuid() != 0) {
        CHECK(chmod(root, 0500) == 0);
        CHECK(h_fs_DevExtend(116441088, 116441088) != RES_OK);
        CHECK(h_fs_DevGetSize(&data, &journal) == RES_OK && data == 116408320 && journal == 116424704);
        CHECK(chmod(root, 0700) == 0);
    }
    FILE *stream = fopen(path, "r+b"); CHECK(stream != NULL);
    if (stream) { uint64_t wrong_program = 42; fseek(stream, 8, SEEK_SET); fwrite(&wrong_program, 8, 1, stream); fclose(stream); }
    data = journal = -1;
    CHECK(h_fs_DevGetSize(&data, &journal) != RES_OK && data == -1 && journal == -1);
    CHECK(h_fs_DevExtend(116441088, 116441088) != RES_OK);
    CHECK(unlink(path) == 0);
    CHECK(mkdir(path, 0755) == 0);
    CHECK(h_fs_DevExtend(116408320, 116424704) != RES_OK);
    CHECK(rmdir(path) == 0);
    CHECK(h_fs_DevGetSize(&data, &journal) == RES_OK && data == 116391936 && journal == 116391936);
    CHECK(rmdir(root) == 0);
    printf("DEVICE_SAVE checks=%d failures=%d\n", checks, failures);
    return failures != 0;
}
