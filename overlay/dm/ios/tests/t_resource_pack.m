#import "../Sources/DmResourcePack.h"
// 每次命令单独进程，验证导入状态确实能跨进程续用。
// Each invocation is a separate process, verifying persisted import state across launches.
int main(int argc,char **argv) {
    @autoreleasepool {
        if (argc!=3) return 3;
        NSError *error=nil;
        NSDictionary *result=dm_resource_import([NSURL fileURLWithPath:@(argv[1])], [NSURL fileURLWithPath:@(argv[2])], nil, &error);
        NSDictionary *output=result ?: @{@"error":error.localizedDescription ?: @"unknown"};
        NSData *json=[NSJSONSerialization dataWithJSONObject:output options:0 error:nil];
        puts([[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding].UTF8String);
        return result ? 0 : 2;
    }
}
