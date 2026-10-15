// BCAT 离线空缓存：没有已分发内容时，挂载成功、打开目录/文件返回不存在，同步返回网络请求未获准。
// Offline empty BCAT cache: mounting succeeds, opening missing directories/files returns NotFound, and sync returns InternetRequestNotAccepted.
// 保留 SDK 的对象构造/析构与 Progress::Update；打开从未成功，服务指针保持为空，不会进入 IPC。
// Keep SDK constructors/destructors and Progress::Update; no open succeeds, so service pointers stay null and never enter IPC.
#include "hle.h"
#include <stdatomic.h>

// 模块 122 的标准结果；0x107a 也来自本作 SDK 的 RequestSyncDeliveryCache 离线分支。
// Standard module-122 results; 0x107a also appears in this game's SDK offline RequestSyncDeliveryCache branch.
// https://switchbrew.org/wiki/Error_codes
enum { BCAT_NOT_FOUND=0x47a, BCAT_ALREADY_MOUNTED=0x87a, BCAT_NOT_MOUNTED=0xa7a, BCAT_NO_INTERNET=0x107a };
static _Atomic int g_bcat_mounted;

HLE_FORCE("_ZN2nn4bcat10InitializeEv", "BCAT 初始化：宿主提供离线空缓存，不连接 Switch 分发服务", void, h_bcat_Init, void) {
    LOG("bcat：离线空缓存已初始化");
}
HLE_FORCE("_ZN2nn4bcat25MountDeliveryCacheStorageEv", "挂载离线分发缓存：有效空缓存，保留已挂载状态", Result, h_bcat_Mount, void) {
    return atomic_exchange(&g_bcat_mounted,1) ? BCAT_ALREADY_MOUNTED : RES_OK;
}
HLE_FORCE("_ZN2nn4bcat27UnmountDeliveryCacheStorageEv", "卸载离线分发缓存：释放挂载状态", Result, h_bcat_Unmount, void) {
    return atomic_exchange(&g_bcat_mounted,0) ? RES_OK : BCAT_NOT_MOUNTED;
}
HLE_FORCE("_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE", "打开分发目录：空缓存中不存在，不制造服务对象", Result, h_bcat_DirOpen, void *self, const void *name) {
    (void)self; (void)name;
    return atomic_load(&g_bcat_mounted) ? BCAT_NOT_FOUND : BCAT_NOT_MOUNTED;
}
HLE_FORCE("_ZN2nn4bcat17DeliveryCacheFile4OpenERKNS0_13DirectoryNameERKNS0_8FileNameE", "打开分发文件：空缓存中不存在，不制造服务对象", Result, h_bcat_FileOpen, void *self, const void *directory, const void *file) {
    (void)self; (void)directory; (void)file;
    return atomic_load(&g_bcat_mounted) ? BCAT_NOT_FOUND : BCAT_NOT_MOUNTED;
}
HLE_FORCE("_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressE", "分发同步：返回 SDK 自身的网络请求未获准结果，不附加异步服务", Result, h_bcat_Sync, void *progress) {
    (void)progress; return BCAT_NO_INTERNET;
}
