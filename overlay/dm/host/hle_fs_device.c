// 设备存档容量：main 的 SDK 20 导入 GetDeviceSaveDataSize / ExtendDeviceSaveData。
// Device-save capacity: the SDK 20 main imports GetDeviceSaveDataSize / ExtendDeviceSaveData.
// 保存的是逻辑容量；实际提交仍由 hle_fs.c 的宿主文件系统事务负责，写满时拒绝提交。
// Capacities are logical; hle_fs.c implements actual host transactions and refuses commits after write failures.
#include "hle.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

typedef struct { uint64_t magic, program; int64_t data, journal; } device_quota;
_Static_assert(sizeof(device_quota) == 32, "device quota record layout");
static pthread_mutex_t g_quota_lock = PTHREAD_MUTEX_INITIALIZER;
#define QUOTA_MAGIC UINT64_C(0x3141544f55514d44)

static int quota_path(char path[1200]) {
    const char *save = getenv("DM_SAVE");
    if (!save || !*save) return 0;
    size_t n = strlen(save); while (n > 1 && save[n - 1] == '/') n--;
    if (n > 1100) return 0;
    snprintf(path, 1200, "%.*s.dmquota", (int)n, save); return 1;
}

static int quota_valid(const device_quota *q) {
    return DM_GAME_DEVICE_SAVE_SIZE > 0 && DM_GAME_DEVICE_SAVE_JOURNAL > 0
        && q->magic == QUOTA_MAGIC && q->program == DM_GAME_PROGRAM_ID
        && q->data >= DM_GAME_DEVICE_SAVE_SIZE && q->data <= DM_GAME_DEVICE_SAVE_MAX
        && q->journal >= DM_GAME_DEVICE_SAVE_JOURNAL && q->journal <= DM_GAME_DEVICE_SAVE_JOURNAL_MAX
        && !(q->data & 0x3fff) && !(q->journal & 0x3fff);
}

static Result quota_load(device_quota *out) {
    device_quota q = {QUOTA_MAGIC, DM_GAME_PROGRAM_ID, DM_GAME_DEVICE_SAVE_SIZE, DM_GAME_DEVICE_SAVE_JOURNAL};
    char path[1200]; if (!quota_valid(&q) || !quota_path(path)) return RES_FAIL;
    FILE *stream = fopen(path, "rb");
    if (!stream) { if (errno == ENOENT) { *out = q; return RES_OK; } return RES_FAIL; }
    int ok = fread(&q, sizeof q, 1, stream) == 1 && fgetc(stream) == EOF && !ferror(stream);
    if (fclose(stream)) ok = 0;
    if (!ok || !quota_valid(&q)) { LOG("【红】设备存档容量记录损坏或属于其他程序：%s", path); return RES_FAIL; }
    *out = q; return RES_OK;
}

static Result quota_store(const device_quota *q) {
    char path[1200], temp[1250]; if (!quota_path(path)) return RES_FAIL;
    snprintf(temp, sizeof temp, "%s.tmp.XXXXXX", path);
    int fd = mkstemp(temp); if (fd < 0) return RES_FAIL;
    size_t done = 0;
    while (done < sizeof *q) {
        ssize_t wrote = write(fd, (const uint8_t *)q + done, sizeof *q - done);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) break;
        done += (size_t)wrote;
    }
    int ok = done == sizeof *q && fsync(fd) == 0;
    if (close(fd)) ok = 0;
    if (ok && rename(temp, path)) ok = 0;
    if (!ok) unlink(temp);
    return ok ? RES_OK : RES_FAIL;
}

HLE_FORCE("_ZN2nn2fs21GetDeviceSaveDataSizeEPlS1_", "fsp-srv；设备存档逻辑容量由 NACP 起始值及持久记录给出", Result, h_fs_DevGetSize, int64_t *data, int64_t *journal) {
    if (!data || !journal) return RES_FAIL;
    pthread_mutex_lock(&g_quota_lock); device_quota q; Result r = quota_load(&q);
    if (r == RES_OK) { *data = q.data; *journal = q.journal; }
    pthread_mutex_unlock(&g_quota_lock); return r;
}

HLE_FORCE("_ZN2nn2fs20ExtendDeviceSaveDataEll", "fsp-srv；单调扩展逻辑容量，持久化失败不发布新值", Result, h_fs_DevExtend, int64_t data, int64_t journal) {
    pthread_mutex_lock(&g_quota_lock); device_quota old; Result r = quota_load(&old);
    if (r == RES_OK) {
        device_quota next = {QUOTA_MAGIC, DM_GAME_PROGRAM_ID, data, journal};
        if (!quota_valid(&next) || data < old.data || journal < old.journal) r = RES_FAIL;
        else if (data != old.data || journal != old.journal) r = quota_store(&next);
    }
    pthread_mutex_unlock(&g_quota_lock); return r;
}
