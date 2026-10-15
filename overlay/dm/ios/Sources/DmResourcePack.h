#import <Foundation/Foundation.h>
// 每卷独立 ZIP，校验并流式落盘；未齐卷时不会发布 romfs。
// Each volume is an independent ZIP; validate and stream to disk, publishing romfs only once complete.
typedef void (^DmImportProgress)(uint64_t done, uint64_t total);
NSDictionary *dm_resource_import(NSURL *archive, NSURL *documents, DmImportProgress progress, NSError **error);
