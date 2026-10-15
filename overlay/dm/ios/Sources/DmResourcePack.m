#import "DmResourcePack.h"
#import <CommonCrypto/CommonDigest.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_ZLIB_APIS
#include "../../third_party/miniz/miniz.h"
#include "game_profile.h"

static BOOL fail(NSError **error, NSString *message) {
    if (error) *error = [NSError errorWithDomain:@"ResourcePack" code:1 userInfo:@{NSLocalizedDescriptionKey:message}];
    return NO;
}
static NSString *hex_digest(const unsigned char *digest) {
    char text[65]; for (int i=0; i<32; ++i) snprintf(text+2*i, 3, "%02x", digest[i]); return @(text);
}
static BOOL logical_path(id value) {
    if (![value isKindOfClass:NSString.class] || ![value length] || [value length] > 900) return NO;
    NSString *path = value;
    if ([path hasPrefix:@"/"] || [path rangeOfString:@"\\"].location != NSNotFound ||
        [path rangeOfString:@":"].location != NSNotFound || [path rangeOfString:@"\0"].location != NSNotFound) return NO;
    for (NSString *part in [path componentsSeparatedByString:@"/"])
        if (!part.length || [part isEqual:@"."] || [part isEqual:@".."]) return NO;
    return YES;
}
static BOOL digest_string(id value) {
    if (![value isKindOfClass:NSString.class] || [value length] != 64) return NO;
    return [value rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet]].location == NSNotFound;
}
static BOOL number(id value, uint64_t maximum) {
    return [value isKindOfClass:NSNumber.class] && [value doubleValue] >= 0 &&
        [value doubleValue] <= maximum && [value doubleValue] == (double)[value unsignedLongLongValue];
}
// 使用同目录临时文件；CRC、SHA 和写入全部通过后才替换这一项。
// Write a sibling temporary file, replacing the entry only after CRC, SHA, and all writes succeed.
typedef struct { FILE *file; CC_SHA256_CTX sha; uint64_t bytes, limit; __unsafe_unretained DmImportProgress progress; } Sink;
static size_t sink_write(void *opaque, mz_uint64 offset, const void *buffer, size_t count) {
    Sink *sink = opaque;
    if (offset != sink->bytes || count > sink->limit - sink->bytes) return 0;
    size_t written = fwrite(buffer, 1, count, sink->file);
    CC_SHA256_Update(&sink->sha, buffer, (CC_LONG)written); sink->bytes += written;
    if (sink->progress) sink->progress(sink->bytes, sink->limit);
    return written;
}
static BOOL no_symlink(NSURL *root, NSString *relative, NSError **error) {
    NSString *path = root.path;
    for (NSString *part in [relative componentsSeparatedByString:@"/"]) {
        path = [path stringByAppendingPathComponent:part]; struct stat st;
        if (!lstat(path.fileSystemRepresentation, &st) && S_ISLNK(st.st_mode)) return fail(error, @"导入目录包含符号链接，请移走未完成的导入目录后重试。");
    }
    return YES;
}
static BOOL extract_file(mz_zip_archive *zip, unsigned index, NSDictionary *row, NSURL *root, DmImportProgress progress, NSError **error) {
    NSFileManager *fm = NSFileManager.defaultManager; NSString *relative = row[@"path"];
    if (!no_symlink(root, relative, error)) return NO;
    NSURL *destination = [root URLByAppendingPathComponent:relative];
    if (![fm createDirectoryAtURL:destination.URLByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:error]) return NO;
    NSURL *temporary = [destination.URLByDeletingLastPathComponent URLByAppendingPathComponent:[@".import-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    Sink sink = {.file=fopen(temporary.fileSystemRepresentation, "wb"), .limit=[row[@"size"] unsignedLongLongValue], .progress=progress};
    if (!sink.file) return fail(error, @"无法写入资源，请检查手机剩余存储空间。");
    CC_SHA256_Init(&sink.sha);
    BOOL ok = mz_zip_reader_extract_to_callback(zip, index, sink_write, &sink, 0);
    if (fclose(sink.file)) ok = NO;
    unsigned char digest[32]; CC_SHA256_Final(digest, &sink.sha);
    if (sink.bytes != sink.limit || ![hex_digest(digest) isEqual:row[@"sha256"]]) ok = NO;
    if (ok && rename(temporary.fileSystemRepresentation, destination.fileSystemRepresentation)) ok = NO;
    if (!ok) { [fm removeItemAtURL:temporary error:nil]; return fail(error, @"该卷解压或校验失败（也可能存储空间不足）。已成功导入的其他卷保留，可重新传入本卷。"); }
    return YES;
}
static BOOL check_file(NSURL *root, NSDictionary *row, NSError **error) {
    if (!no_symlink(root, row[@"path"], error)) return NO;
    NSURL *url = [root URLByAppendingPathComponent:row[@"path"]]; FILE *file = fopen(url.fileSystemRepresentation, "rb");
    if (!file) return fail(error, @"已导入资源缺失，请重新导入对应分卷。");
    CC_SHA256_CTX sha; CC_SHA256_Init(&sha); unsigned char buffer[65536], digest[32]; uint64_t size=0; size_t count;
    while ((count=fread(buffer, 1, sizeof buffer, file))) { CC_SHA256_Update(&sha, buffer, (CC_LONG)count); size+=count; }
    BOOL ok = !ferror(file); fclose(file); CC_SHA256_Final(digest, &sha);
    return ok && size == [row[@"size"] unsignedLongLongValue] && [hex_digest(digest) isEqual:row[@"sha256"]] ? YES : fail(error, @"已导入资源发生变化，请重新导入对应分卷。");
}

NSDictionary *dm_resource_import(NSURL *archive, NSURL *documents, DmImportProgress progress, NSError **error) {
    mz_zip_archive zip = {0}; NSDictionary *result = nil;
    NSFileManager *fm = NSFileManager.defaultManager;
    if (!mz_zip_reader_init_file(&zip, archive.fileSystemRepresentation, 0)) { fail(error, @"无法打开 ZIP。请先把完整分卷下载到本机。"); return nil; }
    @try {
        int manifestIndex = mz_zip_reader_locate_file(&zip, "acnh-volume.json", NULL, MZ_ZIP_FLAG_CASE_SENSITIVE);
        mz_zip_archive_file_stat info;
        if (manifestIndex < 0 || !mz_zip_reader_file_stat(&zip, manifestIndex, &info) || info.m_uncomp_size > (32u<<20)) { fail(error, @"不是本 App 支持的资源分卷。"); return nil; }
        size_t length=0; void *bytes=mz_zip_reader_extract_to_heap(&zip, manifestIndex, &length, 0);
        if (!bytes) { fail(error, @"分卷清单校验失败。"); return nil; }
        NSData *json=[NSData dataWithBytes:bytes length:length]; mz_free(bytes);
        NSDictionary *meta=[NSJSONSerialization JSONObjectWithData:json options:0 error:error];
        if (![meta isKindOfClass:NSDictionary.class]) return nil;
        NSString *pack=meta[@"pack_id"]; NSUUID *uuid=[pack isKindOfClass:NSString.class] ? [[NSUUID alloc] initWithUUIDString:pack] : nil;
        if (!uuid || ![uuid.UUIDString.lowercaseString isEqual:pack] || ![meta[@"schema"] isEqual:@1] ||
            ![meta[@"title_id"] isEqual:@DM_GAME_RESOURCE_TITLE] || ![meta[@"build"] isEqual:@DM_GAME_RESOURCE_BUILD] ||
            !number(meta[@"parts"], 128) || ![meta[@"parts"] unsignedIntValue] || !number(meta[@"part"], [meta[@"parts"] unsignedIntValue]) ||
            ![meta[@"part"] unsignedIntValue] || !number(meta[@"total_files"], 200000) || !number(meta[@"total_bytes"], 64ull<<30) ||
            ![meta[@"files"] isKindOfClass:NSArray.class]) { fail(error, @"资源分卷版本或编号不匹配。"); return nil; }
        NSArray *rows=meta[@"files"];
        if (!rows.count || rows.count > 200000 || mz_zip_reader_get_num_files(&zip) != rows.count+1) { fail(error, @"分卷文件数不匹配。"); return nil; }
        NSMutableDictionary *expected=[NSMutableDictionary dictionary]; uint64_t volumeBytes=0;
        for (NSDictionary *row in rows) {
            if (![row isKindOfClass:NSDictionary.class] || !logical_path(row[@"path"]) || !number(row[@"size"], 8ull<<30) || !digest_string(row[@"sha256"]) || expected[row[@"path"]]) { fail(error, @"分卷包含无效或重复路径。"); return nil; }
            expected[row[@"path"]]=row; volumeBytes += [row[@"size"] unsignedLongLongValue];
        }
        if (volumeBytes > [meta[@"total_bytes"] unsignedLongLongValue]) { fail(error, @"分卷大小无效。"); return nil; }
        NSMutableDictionary *indices=[NSMutableDictionary dictionary];
        for (unsigned i=0; i<mz_zip_reader_get_num_files(&zip); ++i) {
            if ((int)i == manifestIndex) continue;
            if (!mz_zip_reader_file_stat(&zip, i, &info) || !info.m_is_supported || info.m_is_directory ||
                ((info.m_external_attr>>16)&0170000) == 0120000 || mz_zip_reader_get_filename(&zip, i, NULL, 0) > sizeof info.m_filename) { fail(error, @"分卷包含不支持的文件。"); return nil; }
            NSString *name=[[NSString alloc] initWithUTF8String:info.m_filename];
            NSString *relative=[name hasPrefix:@"romfs/"] ? [name substringFromIndex:6] : nil;
            NSDictionary *row=relative ? expected[relative] : nil;
            if (!row || indices[relative] || info.m_uncomp_size != [row[@"size"] unsignedLongLongValue]) { fail(error, @"分卷文件与清单不一致。"); return nil; }
            indices[relative]=@(i);
        }
        NSURL *root=[documents URLByAppendingPathComponent:[@".acnh-import-" stringByAppendingString:pack]];
        NSURL *stateURL=[documents URLByAppendingPathComponent:[NSString stringWithFormat:@".acnh-import-%@.json",pack]];
        if (!no_symlink(documents, root.lastPathComponent, error) || !no_symlink(documents, stateURL.lastPathComponent, error)) return nil;
        NSMutableDictionary *identity=[meta mutableCopy]; [identity removeObjectForKey:@"files"]; [identity removeObjectForKey:@"part"];
        NSMutableDictionary *state=nil;
        if ([fm fileExistsAtPath:stateURL.path]) {
            NSData *data=[NSData dataWithContentsOfURL:stateURL options:0 error:error];
            state=data ? [[NSJSONSerialization JSONObjectWithData:data options:NSJSONReadingMutableContainers error:error] mutableCopy] : nil;
            if (![state isKindOfClass:NSDictionary.class] || ![state[@"identity"] isEqual:identity] || ![state[@"volumes"] isKindOfClass:NSDictionary.class]) { fail(error, @"未完成的导入记录与本卷不匹配。"); return nil; }
        } else state=[@{@"identity":identity,@"volumes":[NSMutableDictionary dictionary]} mutableCopy];
        NSMutableDictionary *volumes=state[@"volumes"]; NSString *part=[meta[@"part"] stringValue];
        NSMutableSet *claimed=[NSMutableSet set];
        for (NSString *key in volumes) {
            if ([key isEqual:part]) continue;
            for (NSDictionary *row in volumes[key]) [claimed addObject:row[@"path"]];
        }
        for (NSString *path in expected) if ([claimed containsObject:path]) { fail(error, @"不同分卷包含重复资源路径。"); return nil; }
        if (![fm createDirectoryAtURL:root withIntermediateDirectories:YES attributes:nil error:error]) return nil;
        __block uint64_t done=0;
        for (NSDictionary *row in rows) { @autoreleasepool {
            uint64_t base=done;
            if (!extract_file(&zip, [indices[row[@"path"]] unsignedIntValue], row, root, ^(uint64_t n,uint64_t size){ if (progress) progress(base+n,volumeBytes); }, error)) return nil;
            done += [row[@"size"] unsignedLongLongValue];
        } }
        volumes[part]=rows;
        NSData *newState=[NSJSONSerialization dataWithJSONObject:state options:0 error:error];
        if (!newState || ![newState writeToURL:stateURL options:NSDataWritingAtomic error:error]) return nil;
        BOOL complete=volumes.count == [meta[@"parts"] unsignedIntValue];
        if (complete) {
            uint64_t totalFiles=0,totalBytes=0;
            for (unsigned n=1; n<=[meta[@"parts"] unsignedIntValue]; ++n) {
                NSArray *files=volumes[[@(n) stringValue]]; if (!files) { fail(error,@"分卷编号不连续。"); return nil; }
                for (NSDictionary *row in files) {
                    if (!check_file(root,row,error)) return nil;
                    totalFiles++; totalBytes += [row[@"size"] unsignedLongLongValue];
                }
            }
            if (totalFiles != [meta[@"total_files"] unsignedLongLongValue] || totalBytes != [meta[@"total_bytes"] unsignedLongLongValue] ||
                ![fm fileExistsAtPath:[root.path stringByAppendingPathComponent:@DM_GAME_ROMFS_MARKER]]) { fail(error,@"资源未完整，请检查分卷。"); return nil; }
            NSURL *target=[documents URLByAppendingPathComponent:@"romfs"]; BOOL existed=[fm fileExistsAtPath:target.path];
            if (!no_symlink(documents,@"romfs",error)) return nil;
            if (existed ? renamex_np(root.fileSystemRepresentation,target.fileSystemRepresentation,RENAME_SWAP) : rename(root.fileSystemRepresentation,target.fileSystemRepresentation)) { fail(error,@"无法启用已解压的资源，原资源保持不变。"); return nil; }
            if (existed) [fm removeItemAtURL:root error:nil];
            [fm removeItemAtURL:stateURL error:nil];
        }
        result=@{@"part":meta[@"part"],@"parts":meta[@"parts"],@"received":@(volumes.count),@"complete":@(complete)};
    } @catch (NSException *exception) { fail(error,@"资源清单或导入记录无效，请重新传入分卷。"); }
    @finally { mz_zip_reader_end(&zip); }
    return result;
}
