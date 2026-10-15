#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

// 在 CI 的 Metal 设备上执行真实核；合成输入覆盖索引、实例、深度变换和写入边界。
// Execute real kernels on the CI Metal device using synthetic indices, instances, depth, and write-boundary checks.
static void require(BOOL condition, NSString *message) { if (!condition) { fprintf(stderr,"FAIL %s\n",message.UTF8String); exit(2); } }
static id<MTLBuffer> buffer(id<MTLDevice> device, NSUInteger bytes) {
    id<MTLBuffer> b=[device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    require(b!=nil,@"buffer allocation"); memset(b.contents,0,bytes); return b;
}
static id<MTLTexture> texture(id<MTLDevice> device,id<MTLBuffer> b,NSUInteger words) {
    MTLTextureDescriptor *d=[MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatR32Float width:words resourceOptions:MTLResourceStorageModeShared usage:MTLTextureUsageShaderRead];
    id<MTLTexture> t=[b newTextureWithDescriptor:d offset:0 bytesPerRow:words*4]; require(t!=nil,@"buffer texture");return t;
}
static NSData *unhex(NSString *s) {
    NSMutableData *d=[NSMutableData dataWithLength:s.length/2];unsigned char *p=d.mutableBytes;
    for(NSUInteger i=0;i<d.length;i++){ unsigned value=0;sscanf([[s substringWithRange:NSMakeRange(i*2,2)] UTF8String],"%2x",&value);p[i]=value; }return d;
}
static id<MTLFunction> function(id<MTLDevice> device,NSString *root,NSString *stem) {
    NSError *error=nil;NSURL *url=[NSURL fileURLWithPath:[[root stringByAppendingPathComponent:stem] stringByAppendingPathExtension:@"metallib"]];
    id<MTLLibrary> lib=[device newLibraryWithURL:url error:&error];require(lib!=nil,error.description ?: stem);
    id<MTLFunction> fn=[lib newFunctionWithName:@"main0"];require(fn!=nil,stem);return fn;
}
static id<MTLBuffer> args(id<MTLDevice> device,id<MTLFunction> fn,NSUInteger index,NSArray *buffers,NSArray *textures) {
    id<MTLArgumentEncoder> encoder=[fn newArgumentEncoderWithBufferIndex:index];require(encoder!=nil,@"argument encoder");
    id<MTLBuffer> a=buffer(device,encoder.encodedLength);[encoder setArgumentBuffer:a offset:0];
    for(NSUInteger i=0;i<buffers.count;i++)[encoder setBuffer:buffers[i] offset:0 atIndex:i];
    for(NSUInteger i=0;i<textures.count;i++)[encoder setTexture:textures[i] atIndex:i];return a;
}
static void dispatch(id<MTLCommandBuffer> command,id<MTLDevice> device,id<MTLFunction> fn,NSArray *arguments,NSArray *resources,NSUInteger nx,NSUInteger ny) {
    NSError *error=nil;id<MTLComputePipelineState> pipeline=[device newComputePipelineStateWithFunction:fn error:&error];require(pipeline!=nil,error.description);
    require(pipeline.maxTotalThreadsPerThreadgroup>=32,@"32-thread test group");
    id<MTLComputeCommandEncoder> e=[command computeCommandEncoder];[e setComputePipelineState:pipeline];
    for(NSUInteger i=0;i<arguments.count;i++)[e setBuffer:arguments[i] offset:0 atIndex:i];
    for(id<MTLResource> r in resources)[e useResource:r usage:MTLResourceUsageRead|MTLResourceUsageWrite];
    [e dispatchThreadgroups:MTLSizeMake((nx+7)/8,(ny+3)/4,1) threadsPerThreadgroup:MTLSizeMake(8,4,1)];[e endEncoding];
}
static BOOL close_to(float a,float b){return isfinite(a)&&fabsf(a-b)<0.0001f;}
int main(int argc,char **argv) {
 @autoreleasepool {
    require(argc==3,@"usage: test_geometry <probe inputs> <compiled outputs>"); 
    NSString *inputs=@(argv[1]),*outputs=@(argv[2]);id<MTLDevice> device=MTLCreateSystemDefaultDevice();require(device!=nil,@"Metal device unavailable");
    printf("Metal device: %s\n",device.name.UTF8String);
    NSArray *jobs=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:[inputs stringByAppendingPathComponent:@"jobs.json"]] options:0 error:nil];
    NSArray *pairs=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:[inputs stringByAppendingPathComponent:@"pairs.json"]] options:0 error:nil];
    id<MTLCommandQueue> queue=[device newCommandQueue];NSUInteger cases=0;
    for(NSDictionary *job in jobs){ @autoreleasepool {
        NSString *key=job[@"key"];
        NSDictionary *meta=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:[[inputs stringByAppendingPathComponent:@"spv"] stringByAppendingPathComponent:[key stringByAppendingString:@".probe.json"]]] options:0 error:nil];
        NSUInteger vi=[meta[@"geometry"][@"InputSizePerInvocation"] unsignedIntegerValue],vo=[meta[@"geometry"][@"OutputSizePerInvocation"] unsignedIntegerValue];
        require([meta[@"max_output_vertices"] intValue]==6 && [meta[@"invocations"] intValue]==1,@"known ocean GS output contract");
        const NSUInteger nv=6,np=2,ni=2,guard=64;NSUInteger vwords=nv*ni*vi,gwords=nv*ni*6*vo,iwords=nv*ni*8;
        id<MTLBuffer> info=buffer(device,1056);uint32_t *u=info.contents;
        u[0]=nv;u[1]=ni;u[2]=1;u[3]=3;u[4]=np;u[7]=2;u[8]=4;u[9]=u[10]=u[11]=1;
        id<MTLBuffer> vc3=buffer(device,65536),gc3=buffer(device,65536),vc1=buffer(device,65536),gc1=buffer(device,65536);
        float *v=vc3.contents;v[0]=v[5]=v[10]=v[15]=1;v[16]=v[21]=v[26]=1;v[33*4]=v[33*4+1]=1;
        float *g=gc3.contents;g[0]=1280;g[1]=720;g[2]=0.15;g[3]=0.9;g[4]=1;
        NSData *vconstants=unhex(job[@"vertex"][@"c1"]),*gconstants=unhex(job[@"geometry"][@"c1"]);
        require(vconstants.length<=65536 && gconstants.length<=65536,@"constant sizes");memcpy(vc1.contents,vconstants.bytes,vconstants.length);memcpy(gc1.contents,gconstants.bytes,gconstants.length);
        id<MTLBuffer> attributes=buffer(device,8*4*4),indices=buffer(device,8*4),topology=buffer(device,6*4);
        float *a=attributes.contents;for(unsigned n=0;n<8;n++){a[4*n]=-0.7+0.19*n;a[4*n+1]=0.3-0.11*n;a[4*n+2]=0;a[4*n+3]=1;}
        uint32_t *index=indices.contents;index[0]=index[1]=0;unsigned order[6]={2,1,0,5,4,3};for(unsigned n=0;n<6;n++){index[n+2]=order[n];((uint32_t*)topology.contents)[n]=n;}
        id<MTLTexture> at=texture(device,attributes,32),it=texture(device,indices,8),tt=texture(device,topology,6);
        id<MTLBuffer> vsout=buffer(device,(vwords+guard)*4),gsout=buffer(device,(gwords+guard)*4),gsindices=buffer(device,(iwords+guard)*4);
        memset(vsout.contents,0xcd,vsout.length);memset(gsout.contents,0xcd,gsout.length);memset(gsindices.contents,0xcd,gsindices.length);
        id<MTLFunction> vs=function(device,outputs,[key stringByAppendingString:@"_vertex_compute"]),gs=function(device,outputs,[key stringByAppendingString:@"_geometry_compute"]);
        NSArray *va=@[args(device,vs,0,@[info,vc3,vc1],@[]),args(device,vs,1,@[vsout],@[]),args(device,vs,2,@[],@[it,at])];
        NSArray *ga=@[args(device,gs,0,@[info,gc3,gc1],@[]),args(device,gs,1,@[vsout,gsout,gsindices],@[]),args(device,gs,2,@[],@[tt])];
        id<MTLCommandBuffer> command=[queue commandBuffer];
        dispatch(command,device,vs,va,@[info,vc3,vc1,vsout,attributes,indices,at,it],nv,ni);
        dispatch(command,device,gs,ga,@[info,gc3,gc1,vsout,gsout,gsindices,topology,tt],np,ni);
        [command commit];[command waitUntilCompleted];require(command.status==MTLCommandBufferStatusCompleted,command.error.description ?: @"GPU command failed");
        float *vertex=vsout.contents,*geometry=gsout.contents;uint32_t *gi=gsindices.contents;
        for(NSUInteger inst=0;inst<ni;inst++)for(NSUInteger n=0;n<nv;n++){
            float *p=vertex+(inst*nv+n)*vi;unsigned src=order[n]+1;
            require(close_to(p[0],a[src*4])&&close_to(p[1],-a[src*4+1])&&close_to(p[2],0)&&close_to(p[3],1),[key stringByAppendingString:@" VS indexed position / depth (must not convert twice)"]);
        }
        NSMutableIndexSet *writtenVertices=[NSMutableIndexSet indexSet],*writtenIndices=[NSMutableIndexSet indexSet];
        for(NSUInteger inst=0;inst<ni;inst++)for(NSUInteger prim=0;prim<np;prim++){
            NSUInteger base=(inst*nv+prim)*6,ib=(inst*nv+prim)*8;unsigned expected[8]={0,1,2,UINT32_MAX,3,4,5,UINT32_MAX};
            for(unsigned n=0;n<8;n++)require(gi[ib+n]==(expected[n]==UINT32_MAX?UINT32_MAX:(uint32_t)base+expected[n]),[key stringByAppendingString:@" emitted index order"]);
            [writtenVertices addIndexesInRange:NSMakeRange(base*vo,6*vo)];[writtenIndices addIndexesInRange:NSMakeRange(ib,8)];
            for(unsigned n=0;n<6;n++)for(unsigned c=0;c<4;c++)require(isfinite(geometry[(base+n)*vo+c]),[key stringByAppendingString:@" finite clip position"]);
            for(unsigned n=0;n<3;n++){
                float *actual=geometry+(base+3+n)*vo,*original=vertex+(inst*nv+prim*3+n)*vi;
                require(close_to(actual[0],original[0])&&close_to(actual[1],original[1])&&close_to(actual[2],0.5f*(original[2]+original[3]))&&close_to(actual[3],original[3]),[key stringByAppendingString:@" GS final triangle and single GL-to-Metal depth conversion"]);
            }
        }
        for(NSUInteger n=vwords;n<vwords+guard;n++)require(((uint32_t*)vsout.contents)[n]==0xcdcdcdcd,@"VS guard overwritten");
        for(NSUInteger n=0;n<gwords+guard;n++)if(![writtenVertices containsIndex:n])require(((uint32_t*)gsout.contents)[n]==0xcdcdcdcd,@"GS touched unwritten/guard vertex region");
        for(NSUInteger n=0;n<iwords+guard;n++)if(![writtenIndices containsIndex:n])require(gi[n]==0xcdcdcdcd,@"GS touched unwritten/guard index region");
        id<MTLFunction> passthrough=function(device,outputs,[key stringByAppendingString:@"_passthrough_vertex"]);NSString *ps=nil;
        for(NSDictionary *pair in pairs)if([pair[@"gs"] isEqual:key])ps=pair[@"ps"];
        require(ps!=nil,@"paired fragment");id<MTLFunction> fragment=function(device,outputs,[ps stringByAppendingString:@"_b"]);
        MTLRenderPipelineDescriptor *render=[MTLRenderPipelineDescriptor new];render.vertexFunction=passthrough;render.fragmentFunction=fragment;render.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
        NSError *error=nil;id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:render error:&error];require(pipeline!=nil,error.description ?: @"VS/PS interface link");
        printf("PASS %s: GPU indexed VS, two instances, GS indices/positions/guards, paired PS pipeline\n",key.UTF8String);fflush(stdout);cases++;
    }}
    printf("GPU geometry cases passed: %lu\n",(unsigned long)cases);require(cases==8,@"all eight geometry pairs");
 }
 return 0;
}
