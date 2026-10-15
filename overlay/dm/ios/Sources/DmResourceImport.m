#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <CommonCrypto/CommonDigest.h>
#include <stdio.h>
#import "DmResourcePack.h"
int front_ios_retry_boot(void);
UIViewController *front_ios_root(void);

// 首次导入与错误反馈；解压在后台线程，资源始终落在 App 自己的文稿目录。
// First-run import and errors; unpack on a worker and always store resources in the app's Documents.
@interface DmResourceVC : UIViewController <UIDocumentPickerDelegate>
@property(nonatomic,strong) UILabel *status;
@property(nonatomic,strong) UIProgressView *progress;
@property(nonatomic,strong) UIButton *importButton;
@property(nonatomic,strong) UIButton *downloadButton;
@property(nonatomic,strong) NSURLSession *session;
@property(nonatomic,strong) NSURL *serverBase;
@property(nonatomic,strong) NSDictionary *serverManifest;
@property(nonatomic,strong) NSSet *received;
@property(nonatomic,strong) NSTimer *downloadTimer;
@property(nonatomic) BOOL busy;
@property(nonatomic) BOOL checkedIncoming;
@end
@implementation DmResourceVC
- (NSURL *)documents { return [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject; }
- (void)viewDidLoad {
    [super viewDidLoad]; self.view.backgroundColor=UIColor.systemBackgroundColor;
    UILabel *title=[UILabel new]; title.text=@"导入游戏资源 / Import resources"; title.font=[UIFont preferredFontForTextStyle:UIFontTextStyleTitle2]; title.textAlignment=NSTextAlignmentCenter;
    self.status=[UILabel new]; self.status.numberOfLines=0; self.status.textAlignment=NSTextAlignmentCenter;
    self.status.text=@"选择资源 ZIP 分卷，逐卷导入。\n每卷成功后可删除原 ZIP，再传下一卷。全部完成后自动启动。\n\n也支持把完整 RomFS 放到本 App 文稿的 romfs 文件夹。";
    self.progress=[UIProgressView new];
    self.importButton=[UIButton buttonWithType:UIButtonTypeSystem]; [self.importButton setTitle:@"选择资源 ZIP 分卷" forState:UIControlStateNormal];
    [self.importButton addTarget:self action:@selector(pickVolume) forControlEvents:UIControlEventTouchUpInside];
    self.downloadButton=[UIButton buttonWithType:UIButtonTypeSystem]; [self.downloadButton setTitle:@"从电脑下载资源" forState:UIControlStateNormal];
    [self.downloadButton addTarget:self action:@selector(askServer) forControlEvents:UIControlEventTouchUpInside];
    UIButton *log=[UIButton buttonWithType:UIButtonTypeSystem]; [log setTitle:@"导出日志 / Export log" forState:UIControlStateNormal];
    [log addTarget:self action:@selector(exportLog) forControlEvents:UIControlEventTouchUpInside];
    UIStackView *stack=[[UIStackView alloc] initWithArrangedSubviews:@[title,self.status,self.progress,self.downloadButton,self.importButton,log]];
    stack.axis=UILayoutConstraintAxisVertical; stack.spacing=12; stack.translatesAutoresizingMaskIntoConstraints=NO;
    [self.view addSubview:stack]; UILayoutGuide *safe=self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[[stack.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor], [stack.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
        [stack.widthAnchor constraintLessThanOrEqualToConstant:700], [stack.leadingAnchor constraintGreaterThanOrEqualToAnchor:safe.leadingAnchor constant:24],
        [stack.trailingAnchor constraintLessThanOrEqualToAnchor:safe.trailingAnchor constant:-24], [stack.topAnchor constraintGreaterThanOrEqualToAnchor:safe.topAnchor constant:8],
        [stack.bottomAnchor constraintLessThanOrEqualToAnchor:safe.bottomAnchor constant:-8]]];
    UIApplication.sharedApplication.idleTimerDisabled=NO;
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated]; if (self.checkedIncoming) return; self.checkedIncoming=YES;
    // 文稿中有指定名称的 ZIP 时自动导入，选择器路径用于文稿尚未显示的设备。
    // Auto-import named ZIPs in Documents; the picker also works before Files exposes the app's folder.
    NSArray *files=[NSFileManager.defaultManager contentsOfDirectoryAtURL:self.documents includingPropertiesForKeys:nil options:0 error:nil];
    for (NSURL *url in [files sortedArrayUsingComparator:^NSComparisonResult(NSURL *a,NSURL *b){return [a.lastPathComponent compare:b.lastPathComponent];}])
        if ([url.lastPathComponent hasPrefix:@"ACNH-resources.part"] && [url.pathExtension.lowercaseString isEqual:@"zip"]) { [self importVolume:url]; break; }
}
- (void)pickVolume {
    if (self.busy) return;
    UIDocumentPickerViewController *picker=[[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeZIP] asCopy:NO];
    picker.delegate=self; picker.allowsMultipleSelection=NO; [self presentViewController:picker animated:YES completion:nil];
}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSURL *url=urls.firstObject; if (url) [self importVolume:url];
}
- (void)importVolume:(NSURL *)url {
    if (self.busy) return; self.busy=YES; self.importButton.enabled=NO; self.downloadButton.enabled=NO; self.progress.progress=0;
    self.status.text=@"正在解压并校验本卷，请保持 App 在前台。\nUnpacking and checking this volume…";
    UIApplication.sharedApplication.idleTimerDisabled=YES;
    NSURL *documents=self.documents;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
        BOOL scoped=[url startAccessingSecurityScopedResource];
        __block NSError *error=nil; __block NSDictionary *result=nil; __block CFTimeInterval last=0;
        NSFileCoordinator *coordinator=[[NSFileCoordinator alloc] initWithFilePresenter:nil];
        [coordinator coordinateReadingItemAtURL:url options:0 error:&error byAccessor:^(NSURL *local){
            result=dm_resource_import(local,documents,^(uint64_t done,uint64_t total){
                CFTimeInterval now=CFAbsoluteTimeGetCurrent(); if (now-last<0.15 && done!=total) return; last=now;
                dispatch_async(dispatch_get_main_queue(),^{self.progress.progress=total ? (float)((double)done/total) : 1;});
            },&error);
        }];
        if (scoped) [url stopAccessingSecurityScopedResource];
        dispatch_async(dispatch_get_main_queue(),^{
            self.busy=NO; self.importButton.enabled=YES; self.downloadButton.enabled=YES; UIApplication.sharedApplication.idleTimerDisabled=NO;
            if (!result) { self.status.text=error.localizedDescription ?: @"导入失败，请重新选择完整分卷。"; return; }
            self.progress.progress=1;
            if ([result[@"complete"] boolValue]) {
                if (front_ios_retry_boot()) { self.view.window.rootViewController=front_ios_root(); return; }
                self.status.text=@"已导入，但游戏资源检查未通过。请导出日志。"; return;
            }
            self.status.text=[NSString stringWithFormat:@"第 %@ / %@ 卷已导入，已完成 %@ 卷。\n可以删除刚才的 ZIP，再传入并选择下一卷。",result[@"part"],result[@"parts"],result[@"received"]];
            [self.importButton setTitle:@"选择下一卷" forState:UIControlStateNormal];
        });
    });
}
- (void)finishNetwork:(NSString *)message {
    fprintf(stderr,"resources: %s\n",message.UTF8String);
    dispatch_async(dispatch_get_main_queue(),^{
        [self.downloadTimer invalidate]; self.downloadTimer=nil;
        self.busy=NO; self.importButton.enabled=YES; self.downloadButton.enabled=YES;
        UIApplication.sharedApplication.idleTimerDisabled=NO; self.status.text=message;
        [self.session finishTasksAndInvalidate]; self.session=nil;
    });
}
- (void)askServer {
    if (self.busy) return;
    UIAlertController *alert=[UIAlertController alertControllerWithTitle:@"电脑资源服务器" message:@"手机与电脑连接同一局域网，填入电脑显示的地址。" preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *field){
        field.placeholder=@"http://192.168.x.x:8080"; field.keyboardType=UIKeyboardTypeURL;
        field.autocapitalizationType=UITextAutocapitalizationTypeNone; field.autocorrectionType=UITextAutocorrectionTypeNo;
        field.text=[NSUserDefaults.standardUserDefaults stringForKey:@"resourceServer"];
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"开始导入" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action){ [self startServer:alert.textFields.firstObject.text]; }]];
    [self presentViewController:alert animated:YES completion:nil];
}
- (void)startServer:(NSString *)address {
    NSURL *url=[NSURL URLWithString:[address stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]];
    if (!url.host.length || ![@[@"http",@"https"] containsObject:url.scheme.lowercaseString]) { self.status.text=@"地址格式不正确，请填写 http://电脑IP:端口。"; return; }
    if (![url.lastPathComponent isEqual:@"resource-pack.json"]) url=[[url URLByAppendingPathComponent:@"resource-pack.json"] absoluteURL];
    self.serverBase=url.URLByDeletingLastPathComponent;
    [NSUserDefaults.standardUserDefaults setObject:address forKey:@"resourceServer"];
    self.busy=YES; self.importButton.enabled=NO; self.downloadButton.enabled=NO; self.progress.progress=0;
    self.status.text=@"正在连接电脑并读取资源清单…"; UIApplication.sharedApplication.idleTimerDisabled=YES;
    NSURLSessionConfiguration *config=NSURLSessionConfiguration.ephemeralSessionConfiguration;
    config.timeoutIntervalForRequest=60; config.timeoutIntervalForResource=7200;
    self.session=[NSURLSession sessionWithConfiguration:config];
    [[self.session dataTaskWithURL:url completionHandler:^(NSData *data,NSURLResponse *response,NSError *error){
        if (error || ((NSHTTPURLResponse *)response).statusCode!=200 || data.length>(1<<20)) { [self finishNetwork:error.localizedDescription ?: @"读取清单失败，请检查地址、电脑防火墙和局域网权限。"]; return; }
        NSDictionary *manifest=[NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        if (![manifest isKindOfClass:NSDictionary.class] || ![manifest[@"volumes"] isKindOfClass:NSArray.class] ||
            ![manifest[@"pack_id"] isKindOfClass:NSString.class] || ![[NSUUID alloc] initWithUUIDString:manifest[@"pack_id"]] ||
            ![manifest[@"parts"] isKindOfClass:NSNumber.class] || [manifest[@"volumes"] count]!=[manifest[@"parts"] unsignedIntValue] ||
            ![manifest[@"volumes"] count] || [manifest[@"volumes"] count]>128) { [self finishNetwork:@"电脑提供的资源清单格式不正确。"]; return; }
        unsigned expectedPart=1;
        for (NSDictionary *volume in manifest[@"volumes"]) {
            if (![volume isKindOfClass:NSDictionary.class] || ![volume[@"file"] isKindOfClass:NSString.class] ||
                ![volume[@"file"] isEqual:[NSString stringWithFormat:@"ACNH-resources.part%02u.zip",expectedPart]] ||
                ![volume[@"part"] isEqual:@(expectedPart)] || ![volume[@"bytes"] isKindOfClass:NSNumber.class] ||
                [volume[@"bytes"] unsignedLongLongValue]>(8ull<<30) || ![volume[@"sha256"] isKindOfClass:NSString.class] || [volume[@"sha256"] length]!=64) {
                [self finishNetwork:@"电脑提供的分卷清单无效。"]; return;
            }
            expectedPart++;
        }
        self.serverManifest=manifest;
        NSURL *state=[self.documents URLByAppendingPathComponent:[NSString stringWithFormat:@".acnh-import-%@.json",manifest[@"pack_id"]]];
        NSDictionary *saved=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfURL:state] ?: [NSData data] options:0 error:nil];
        NSMutableDictionary *identity=[manifest mutableCopy]; [identity removeObjectForKey:@"volumes"];
        self.received=[saved isKindOfClass:NSDictionary.class] && [saved[@"identity"] isEqual:identity] && [saved[@"volumes"] isKindOfClass:NSDictionary.class] ? [NSSet setWithArray:[saved[@"volumes"] allKeys]] : [NSSet set];
        [self downloadVolume:0];
    }] resume];
}
- (void)downloadVolume:(NSUInteger)index {
    NSArray *volumes=self.serverManifest[@"volumes"];
    while (index+1<volumes.count && [self.received containsObject:[volumes[index][@"part"] stringValue]]) index++;
    NSDictionary *volume=volumes[index]; NSUInteger next=index+1;
    dispatch_async(dispatch_get_main_queue(),^{self.status.text=[NSString stringWithFormat:@"正在下载第 %@ / %@ 卷…\n请保持 App 在前台。",volume[@"part"],self.serverManifest[@"parts"]];self.progress.progress=0;});
    NSURL *url=[self.serverBase URLByAppendingPathComponent:volume[@"file"]];
    // NSURLSession 下载任务把响应写入临时文件，禁止用 NSData 缓存数 GiB 分卷。
    // Download tasks stream responses to temporary files; never buffer multi-GiB volumes in NSData.
    NSURLSessionDownloadTask *task=[self.session downloadTaskWithURL:url completionHandler:^(NSURL *temporary,NSURLResponse *response,NSError *error){
        dispatch_async(dispatch_get_main_queue(),^{
            [self.downloadTimer invalidate]; self.downloadTimer=nil;
            self.status.text=[NSString stringWithFormat:@"正在校验第 %@ / %@ 卷…",volume[@"part"],self.serverManifest[@"parts"]];
        });
        if (error || ((NSHTTPURLResponse *)response).statusCode!=200 || !temporary) { [self finishNetwork:error.localizedDescription ?: @"分卷下载失败，已导入的卷会保留。"]; return; }
        FILE *file=fopen(temporary.fileSystemRepresentation,"rb");
        if (!file) { [self finishNetwork:@"无法读取下载缓存。"]; return; }
        CC_SHA256_CTX sha; CC_SHA256_Init(&sha); unsigned char buffer[65536],digest[32]; uint64_t size=0; size_t n;
        while ((n=fread(buffer,1,sizeof buffer,file))) { CC_SHA256_Update(&sha,buffer,(CC_LONG)n);size+=n; }
        BOOL good=!ferror(file); fclose(file); CC_SHA256_Final(digest,&sha); char text[65];for(int i=0;i<32;i++)snprintf(text+2*i,3,"%02x",digest[i]);
        if (!good || size!=[volume[@"bytes"] unsignedLongLongValue] || ![@(text) isEqual:volume[@"sha256"]]) { [NSFileManager.defaultManager removeItemAtURL:temporary error:nil]; [self finishNetwork:@"下载的分卷校验失败，请重新连接电脑重试。"]; return; }
        dispatch_async(dispatch_get_main_queue(),^{self.status.text=[NSString stringWithFormat:@"正在解压第 %@ / %@ 卷…",volume[@"part"],self.serverManifest[@"parts"]];});
        __block CFTimeInterval last=0;
        NSDictionary *result=dm_resource_import(temporary,self.documents,^(uint64_t done,uint64_t total){
            CFTimeInterval now=CFAbsoluteTimeGetCurrent();if(now-last<0.15 && done!=total)return;last=now;
            dispatch_async(dispatch_get_main_queue(),^{self.progress.progress=total?(float)((double)done/total):1;});
        },&error);
        [NSFileManager.defaultManager removeItemAtURL:temporary error:nil];
        if (!result) { [self finishNetwork:error.localizedDescription ?: @"资源解压失败。"]; return; }
        if ([result[@"complete"] boolValue]) {
            [self finishNetwork:@"资源导入完成，正在启动…"];
            dispatch_async(dispatch_get_main_queue(),^{if(front_ios_retry_boot())self.view.window.rootViewController=front_ios_root();});
        } else if (next<volumes.count) [self downloadVolume:next];
        else [self finishNetwork:@"尚缺资源分卷，请重新连接电脑继续导入。"]; 
    }];
    dispatch_async(dispatch_get_main_queue(),^{
        [self.downloadTimer invalidate];
        __weak DmResourceVC *weakSelf=self;
        self.downloadTimer=[NSTimer scheduledTimerWithTimeInterval:0.3 repeats:YES block:^(NSTimer *timer){
            DmResourceVC *owner=weakSelf; if (!owner) { [timer invalidate]; return; }
            double done=(double)task.countOfBytesReceived, total=[volume[@"bytes"] doubleValue];
            owner.progress.progress=total>0 ? (float)fmin(1.0,done/total) : 0;
            owner.status.text=[NSString stringWithFormat:@"正在下载第 %@ / %@ 卷：%.2f / %.2f GiB\n请保持 App 在前台。",volume[@"part"],owner.serverManifest[@"parts"],done/(1ull<<30),total/(1ull<<30)];
        }];
    });
    fprintf(stderr,"resources: downloading volume %s/%s\n",[volume[@"part"] description].UTF8String,[self.serverManifest[@"parts"] description].UTF8String);
    [task resume];
}
- (void)exportLog {
    NSURL *log=[self.documents URLByAppendingPathComponent:@"log.txt"];
    if (![NSFileManager.defaultManager fileExistsAtPath:log.path]) { self.status.text=@"还没有日志。"; return; }
    UIActivityViewController *share=[[UIActivityViewController alloc] initWithActivityItems:@[log] applicationActivities:nil];
    share.popoverPresentationController.sourceView=self.view; share.popoverPresentationController.sourceRect=self.view.bounds;
    [self presentViewController:share animated:YES completion:nil];
}
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
@end
UIViewController *dm_resources_controller(void) { return [DmResourceVC new]; }
