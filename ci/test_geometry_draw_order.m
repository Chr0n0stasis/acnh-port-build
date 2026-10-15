#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "../geometry-test-source/nvn/nvn_geometry_fetch.h"

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
static void prep(id<MTLCommandBuffer> command,id<MTLDevice> device,id<MTLLibrary> library,NSString *name,nvn_geo_params p,NSArray *buffers,NSArray *offsets,NSUInteger count) {
    NSError *error=nil;id<MTLFunction> fn=[library newFunctionWithName:name];require(fn!=nil,name);
    id<MTLComputePipelineState> ps=[device newComputePipelineStateWithFunction:fn error:&error];require(ps!=nil,error.description);
    id<MTLBuffer> parameter=buffer(device,sizeof p);memcpy(parameter.contents,&p,sizeof p);
    id<MTLComputeCommandEncoder> e=[command computeCommandEncoder];[e setComputePipelineState:ps];[e setBuffer:parameter offset:0 atIndex:0];
    for(NSUInteger i=0;i<buffers.count;i++){id<MTLBuffer> b=buffers[i];[e setBuffer:b offset:[offsets[i] unsignedIntegerValue] atIndex:i+1];[e useResource:b usage:MTLResourceUsageRead|MTLResourceUsageWrite];}
    [e dispatchThreads:MTLSizeMake(count,1,1) threadsPerThreadgroup:MTLSizeMake(MIN(32,ps.maxTotalThreadsPerThreadgroup),1,1)];[e endEncoding];
}
static BOOL close_to(float a,float b){return isfinite(a)&&fabsf(a-b)<0.0001f;}
static void test_preparation(id<MTLDevice> device,id<MTLCommandQueue> queue,id<MTLLibrary> library) {
    // 期望值独立手写；源缓冲先填毒值，再由同队列 blit 写入，避免 CPU 预读替代 GPU 取数。
    // Expectations are handwritten; poison sources before a same-queue blit, so CPU reads cannot replace GPU fetches.
    struct FormatCase { uint32_t format, bytes, words[4]; float expected[4]; } formats[]={
        {0x25,4,{0xff00807f},{127.0f/255,128.0f/255,0,1}},
        {0x26,4,{0x7f008081},{-1,-1,0,1}},
        {0x13,4,{0x7fff8000},{-1,1,0,1}},
        {0x11,4,{0xc0003c00},{1,-2,0,1}},
        {0x2e,16,{0x3f800000,0xc0000000,0x3f000000,0x3f800000},{1,-2,0.5,1}},
        {0x0b,4,{42},{42,0,0,1}},
        {0x3d,4,{0xc00003ff},{1,0,0,1}},
        {0x69,4,{512u|(511u<<10)|(1023u<<20)|(2u<<30)},{-1,1,-1.0f/511,-1}},
        {0x3f,4,{960u|(960u<<11)|(480u<<22)},{1,1,1,1}},
        {0x9a,4,{0x80402010},{64.0f/255,32.0f/255,16.0f/255,128.0f/255}}
    };
    unsigned cases=0;
    for(unsigned n=0;n<sizeof formats/sizeof *formats;n++) {
        struct FormatCase f=formats[n];require(nvn_geo_format_bytes(f.format)==f.bytes,@"format byte size");
        id<MTLBuffer> staged=buffer(device,32),source=buffer(device,32),out=buffer(device,32),indices=buffer(device,4);
        memcpy(staged.contents,f.words,16);memset(source.contents,0xcd,32);memset(out.contents,0xcd,32);
        id<MTLCommandBuffer> cb=[queue commandBuffer];id<MTLBlitCommandEncoder> blit=[cb blitCommandEncoder];
        [blit copyFromBuffer:staged sourceOffset:0 toBuffer:source destinationOffset:0 size:32];[blit endEncoding];
        nvn_geo_params p={0};p.count=1;p.vertex_count=1;p.format=f.format;p.stride=f.bytes;p.source_bytes=f.bytes;
        prep(cb,device,library,@"geo_fetch",p,@[source,out,indices],@[@0,@0,@0],1);
        [cb commit];[cb waitUntilCompleted];require(cb.status==MTLCommandBufferStatusCompleted,cb.error.description ?: @"format command");
        for(unsigned c=0;c<4;c++)require(close_to(((float*)out.contents)[c],f.expected[c]),[NSString stringWithFormat:@"format %#x component %u",f.format,c]);
        for(unsigned c=4;c<8;c++)require(((uint32_t*)out.contents)[c]==0xcdcdcdcd,@"format output guard");cases++;
    }
    for(unsigned width=1;width<=4;width*=2){
        id<MTLBuffer> source=buffer(device,16),out=buffer(device,32);memset(out.contents,0xcd,32);
        unsigned values[4]={3,1,0,7};for(unsigned i=0;i<4;i++)memcpy((char*)source.contents+i*width,&values[i],width);
        nvn_geo_params p={0};p.count=5;p.index_bytes=width;p.source_bytes=4*width;
        id<MTLCommandBuffer> cb=[queue commandBuffer];prep(cb,device,library,@"geo_indices",p,@[source,out],@[@0,@0],5);
        [cb commit];[cb waitUntilCompleted];require(cb.status==MTLCommandBufferStatusCompleted,@"index command");
        for(unsigned i=0;i<4;i++)require(((uint32_t*)out.contents)[i]==values[i],@"expanded index");
        require(((uint32_t*)out.contents)[4]==UINT32_MAX&&((uint32_t*)out.contents)[5]==0xcdcdcdcd,@"index bounds and guard");cases++;
    }
    {
        id<MTLBuffer> source=buffer(device,6*16),out=buffer(device,8*16),indices=buffer(device,8);
        for(unsigned i=0;i<6;i++)for(unsigned c=0;c<4;c++)((float*)source.contents)[4*i+c]=10*i+c;
        ((uint32_t*)indices.contents)[0]=5;((uint32_t*)indices.contents)[1]=4;
        nvn_geo_params p={0};p.count=8;p.vertex_count=2;p.format=0x2e;p.stride=16;p.source_bytes=6*16;p.divisor=2;p.base_instance=2;
        id<MTLCommandBuffer> cb=[queue commandBuffer];prep(cb,device,library,@"geo_fetch",p,@[source,out,indices],@[@0,@0,@0],8);
        [cb commit];[cb waitUntilCompleted];require(cb.status==MTLCommandBufferStatusCompleted,@"instancing command");
        for(unsigned i=0;i<8;i++)for(unsigned c=0;c<4;c++)require(close_to(((float*)out.contents)[4*i+c],10*(2+(i/2)/2)+c),@"baseInstance plus divided instance, independent of vertex indices");cases++;
        for(unsigned kind=0;kind<3;kind++){
            p.count=2;p.divisor=0;p.reserved=kind==0?(uint32_t)-6:0;p.source_bytes=kind==1?4:6*16;p.stride=kind==2?0:16;
            cb=[queue commandBuffer];prep(cb,device,library,@"geo_fetch",p,@[source,out,indices],@[@0,@0,@0],2);
            [cb commit];[cb waitUntilCompleted];require(cb.status==MTLCommandBufferStatusCompleted,@"bounds/constant command");
            for(unsigned i=0;i<2;i++)for(unsigned c=0;c<4;c++)require(close_to(((float*)out.contents)[4*i+c],kind==2?(float)c:(c==3?1:0)),@"negative base / short source / constant attribute");cases++;
        }
    }
    {
        id<MTLBuffer> indices=buffer(device,8*4),topology=buffer(device,6*3*4),valid=buffer(device,6*4);
        uint32_t input[8]={0,1,2,255,3,4,5,6};memcpy(indices.contents,input,sizeof input);
        nvn_geo_params p={0};p.vertex_count=8;p.index_bytes=1;p.restart_enable=1;p.restart_index=UINT32_MAX;p.primitive=5;p.primitive_count=6;
        id<MTLCommandBuffer> cb=[queue commandBuffer];prep(cb,device,library,@"geo_topology",p,@[indices,topology,valid],@[@0,@0,@0],1);
        [cb commit];[cb waitUntilCompleted];require(cb.status==MTLCommandBufferStatusCompleted,@"restart command");
        uint32_t expected[9]={0,1,2,4,5,6,6,5,7};require(!memcmp(topology.contents,expected,sizeof expected),@"strip winding and restart reset");
        for(unsigned i=0;i<6;i++)require(((uint32_t*)valid.contents)[i]==(i<3),@"restart compact primitive mask");cases++;
    }
    printf("PASS GPU preparation cases: %u (formats, GPU uploads, indices, instancing, bounds, restart)\n",cases);
}
int main(int argc,char **argv) {
 @autoreleasepool {
    require(argc==3,@"usage: test_geometry <probe inputs> <compiled outputs>"); 
    NSString *inputs=@(argv[1]),*outputs=@(argv[2]);id<MTLDevice> device=MTLCreateSystemDefaultDevice();require(device!=nil,@"Metal device unavailable");
    printf("Metal device: %s\n",device.name.UTF8String);
    NSError *prepError=nil;id<MTLLibrary> preparation=[device newLibraryWithSource:@(k_nvn_geometry_fetch_msl) options:nil error:&prepError];require(preparation!=nil,prepError.description ?: @"preparation kernels");
    NSArray *jobs=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:[inputs stringByAppendingPathComponent:@"jobs.json"]] options:0 error:nil];
    NSArray *pairs=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:[inputs stringByAppendingPathComponent:@"pairs.json"]] options:0 error:nil];
    id<MTLCommandQueue> queue=[device newCommandQueue];test_preparation(device,queue,preparation);NSUInteger cases=0;
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
        id<MTLBuffer> attributes=buffer(device,8*4*4),indices=buffer(device,8*4),topology=buffer(device,6*4),valid=buffer(device,np*4),gathered=buffer(device,nv*ni*16),expanded=buffer(device,8*4);
        id<MTLBuffer> vertexSource=buffer(device,attributes.length),indexSource=buffer(device,indices.length);
        memset(attributes.contents,0xcd,attributes.length);memset(indices.contents,0xcd,indices.length);
        float *a=vertexSource.contents;for(unsigned n=0;n<8;n++){a[4*n]=-0.7+0.19*n;a[4*n+1]=0.3-0.11*n;a[4*n+2]=0;a[4*n+3]=1;}
        uint32_t *index=indexSource.contents;index[0]=index[1]=0;unsigned order[6]={2,1,0,5,4,3};for(unsigned n=0;n<6;n++)index[n+2]=order[n];
        memset(topology.contents,0xcd,topology.length);memset(valid.contents,0xcd,valid.length);
        id<MTLTexture> at=texture(device,gathered,nv*ni*4),it=texture(device,expanded,8),tt=texture(device,topology,6);
        id<MTLBuffer> vsout=buffer(device,(vwords+guard)*4),gsout=buffer(device,(gwords+guard)*4),gsindices=buffer(device,(iwords+guard)*4);
        memset(vsout.contents,0xcd,vsout.length);memset(gsout.contents,0xcd,gsout.length);memset(gsindices.contents,0xcd,gsindices.length);
        id<MTLFunction> vs=function(device,outputs,[key stringByAppendingString:@"_vertex_compute"]),gs=function(device,outputs,[key stringByAppendingString:@"_geometry_compute"]);
        NSArray *va=@[args(device,vs,0,@[info,vc3,vc1],@[]),args(device,vs,1,@[vsout],@[]),args(device,vs,2,@[],@[it,at])];
        NSArray *ga=@[args(device,gs,0,@[info,gc3,gc1],@[]),args(device,gs,1,@[vsout,gsout,gsindices],@[]),args(device,gs,2,@[],@[tt])];
        id<MTLCommandBuffer> command=[queue commandBuffer];
        id<MTLBlitCommandEncoder> upload=[command blitCommandEncoder];
        [upload copyFromBuffer:vertexSource sourceOffset:0 toBuffer:attributes destinationOffset:0 size:attributes.length];
        [upload copyFromBuffer:indexSource sourceOffset:0 toBuffer:indices destinationOffset:0 size:indices.length];[upload endEncoding];
        nvn_geo_params p={0};p.count=8;p.index_bytes=4;p.source_bytes=(uint32_t)indices.length;
        prep(command,device,preparation,@"geo_indices",p,@[indices,expanded],@[@0,@0],8);
        p.count=nv*ni;p.vertex_count=nv;p.instances=ni;p.stride=16;p.source_bytes=(uint32_t)attributes.length;p.format=0x2e;p.reserved=1;p.base_instance=3;
        prep(command,device,preparation,@"geo_fetch",p,@[attributes,gathered,expanded],@[@0,@0,@8],nv*ni);
        p.primitive=4;p.primitive_count=np;p.index_stride=8;
        prep(command,device,preparation,@"geo_topology",p,@[expanded,topology,valid],@[@8,@0,@0],1);
        prep(command,device,preparation,@"geo_initialize_indices",p,@[gsindices],@[@0],iwords);

        dispatch(command,device,vs,va,@[info,vc3,vc1,vsout,gathered,expanded,at,it],nv,ni);
        dispatch(command,device,gs,ga,@[info,gc3,gc1,vsout,gsout,gsindices,topology,tt],np,ni);
        prep(command,device,preparation,@"geo_mask_indices",p,@[valid,gsindices],@[@0,@0],iwords);
        [command commit];[command waitUntilCompleted];require(command.status==MTLCommandBufferStatusCompleted,command.error.description ?: @"GPU command failed");
        for(unsigned n=0;n<6;n++)require(((uint32_t*)topology.contents)[n]==n,@"GPU-produced primitive ordinals");
        for(unsigned n=0;n<np;n++)require(((uint32_t*)valid.contents)[n]==1,@"GPU-produced valid primitives");
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
        for(NSUInteger n=0;n<iwords+guard;n++)if(![writtenIndices containsIndex:n])require(gi[n]==(n<iwords?UINT32_MAX:0xcdcdcdcd),@"GS touched unwritten/guard index region");
        id<MTLFunction> passthrough=function(device,outputs,[key stringByAppendingString:@"_passthrough_vertex"]);NSString *ps=nil;
        for(NSDictionary *pair in pairs)if([pair[@"gs"] isEqual:key])ps=pair[@"ps"];
        require(ps!=nil,@"paired fragment");id<MTLFunction> fragment=function(device,outputs,[ps stringByAppendingString:@"_b"]);
        MTLRenderPipelineDescriptor *render=[MTLRenderPipelineDescriptor new];render.vertexFunction=passthrough;render.fragmentFunction=fragment;render.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
        NSError *error=nil;id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:render error:&error];require(pipeline!=nil,error.description ?: @"VS/PS interface link");
        printf("PASS %s: GPU-gathered indexed VS after queued GPU writes, two instances, GS indices/positions/guards, paired PS pipeline\n",key.UTF8String);fflush(stdout);cases++;
    }}
    printf("GPU geometry cases passed: %lu\n",(unsigned long)cases);require(cases==8,@"all eight geometry pairs");
 }
 return 0;
}
