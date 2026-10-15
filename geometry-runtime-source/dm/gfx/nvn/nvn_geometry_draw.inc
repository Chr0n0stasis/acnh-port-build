// 大输出独立分配，小参数使用现有竞技场；所有资源跟随本次 GPU 提交延后回收。
// Allocate large outputs separately and use the existing arena for small parameters; retire resources after this GPU submission.
static id<MTLBuffer> geometry_buffer(uint64_t bytes) {
    if(!bytes || bytes>g_dev.maxBufferLength) MDIE("【红】几何缓冲长度无效：%llu",(unsigned long long)bytes);
    id<MTLBuffer> b=[g_dev newBufferWithLength:(NSUInteger)bytes options:MTLResourceStorageModeShared];
    if(!b) MDIE("【红】几何缓冲分配失败：%llu",(unsigned long long)bytes);
    res_add(b);defer_obj_release(b);return b;
}
static uint64_t geometry_data(const void *data,size_t size) {
    id<MTLBuffer> b;size_t offset=arena_alloc(size,&b);memcpy((uint8_t*)b.contents+offset,data,size);return b.gpuAddress+offset;
}
static id<MTLTexture> geometry_texture(id<MTLBuffer> buffer) {
    MTLTextureDescriptor *d=[MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatR32Float width:buffer.length/4 resourceOptions:MTLResourceStorageModeShared usage:MTLTextureUsageShaderRead];
    id<MTLTexture> t=[buffer newTextureWithDescriptor:d offset:0 bytesPerRow:buffer.length];
    if(!t) MDIE("【红】几何缓冲贴图创建失败");defer_obj_unref(t);return t;
}
static void geometry_prepare(NSString *name,nvn_geo_params p,const uint64_t *buffers,unsigned n,uint32_t count) {
    static NSMutableDictionary<NSString *,id<MTLComputePipelineState>> *pipelines;
    static id<MTLLibrary> library;
    if(!pipelines) pipelines=[NSMutableDictionary dictionary];
    NSError *error=nil;
    if(!library) library=[g_dev newLibraryWithSource:@(k_nvn_geometry_fetch_msl) options:nil error:&error];
    if(!library) MDIE("【红】几何准备内核编译失败：%s",error.localizedDescription.UTF8String);
    id<MTLComputePipelineState> ps=pipelines[name];
    if(!ps) {
        id<MTLFunction> fn=[library newFunctionWithName:name];ps=[g_dev newComputePipelineStateWithFunction:fn error:&error];
        if(!ps) MDIE("【红】几何准备管线失败：%s",name.UTF8String);pipelines[name]=ps;
    }
    id e=enc_c_begin(NULL);enc_c_pso(e,ps);enc_c_args(e,geometry_data(&p,sizeof p),0);
    for(unsigned i=0;i<n;i++) enc_c_args(e,buffers[i],i+1);
    unsigned width=(unsigned)MIN(ps.maxTotalThreadsPerThreadgroup,32);
    enc_c_dispatch(e,MTLSizeMake((count+width-1)/width,1,1),MTLSizeMake(width,1,1));enc_c_end(e);
}
static void geometry_bind(NvGeometry *g,unsigned role,NSDictionary<NSString *,NSNumber *> *resources,id encoder) {
    for(NSDictionary *set in g->sets[role]) {
        uint64_t slots[32]={0};unsigned words=[set[@"words"] unsignedIntValue];
        for(NSDictionary *binding in set[@"bindings"]) {
            NSString *kind=binding[@"kind"];unsigned aid=[binding[@"id"] unsignedIntValue];uint64_t value=0;
            if([kind isEqual:@"c1"] && role<2) value=g->constants[role].gpuAddress;
            else if([kind isEqual:@"uniform"] && role<2) {
                unsigned reg=[binding[@"reg"] unsignedIntValue];
                if(reg>=16) MDIE("【红】几何 uniform 编号越界");
                const p_bind_buf *u=&S.ubo[role==0?0:2][reg];void *mb;uint64_t offset;
                if(!u->addr || !mtl_gpu_addr(u->addr,1,&mb,&offset,&value)) MDIE("【红】几何 uniform 未绑定：阶段 %u 槽 %u",role,reg);
                use_rec("buf",u->addr,u->size,0,NULL);
            } else {
                NSString *key=[kind isEqual:@"attribute"]?[NSString stringWithFormat:@"attribute%u",[binding[@"location"] unsignedIntValue]]:kind;
                NSNumber *number=resources[key];if(!number) MDIE("【红】几何参数资源未实现：%s",key.UTF8String);value=number.unsignedLongLongValue;
            }
            slots[aid]=value;
        }
        uint64_t address=geometry_data(slots,words*8);unsigned index=[set[@"index"] unsignedIntValue];
        if(role==2) enc_r_args(encoder,MTLRenderStageVertex,address,index);else enc_c_args(encoder,address,index);
    }
}
static uint64_t g_geometry_draws;
static void geometry_draw(NvProg *program,const p_draw *dw,const p_draw_idx *di) {
    int32_t count=dw?dw->count:di->count,instances=dw?dw->instances:di->instances;
    if(count<=0 || S.poly.cull==NVN_FACE_FRONT_AND_BACK) return;
    if(instances<0) MDIE("【红】几何实例数无效");
    uint32_t nv=count,ni=instances?instances:1,primitive=dw?dw->prim:di->prim;
    NvGeometry *g=program->geometry;nvn_geo_sizes sizes;
    uint32_t vs_words=[g->metadata[@"vertex_stride"] unsignedIntValue],gs_words=[g->metadata[@"geometry_stride"] unsignedIntValue];
    uint32_t maximum=[g->metadata[@"max_output_vertices"] unsignedIntValue];
    if(!nvn_geo_plan(nv,ni,primitive,vs_words,gs_words,maximum,&sizes)) MDIE("【红】几何绘制类型或容量不支持");
    if(!sizes.primitives) return;
    // 关闭普通绘制的覆盖/合并假设，准备与渲染之间使用 Metal 4 的生产者/消费者屏障。
    // Bypass normal-draw coverage/merge assumptions and use Metal 4 producer/consumer barriers between preparation and rendering.
    zskip_clear();rp_end();if(g_blit){enc_b_end(g_blit);g_blit=nil;}
    id<MTLBuffer> indices=geometry_buffer((uint64_t)nv*4),topology=geometry_buffer((uint64_t)sizes.primitives*12),valid=geometry_buffer((uint64_t)sizes.primitives*4);
    id<MTLBuffer> vertex=geometry_buffer((uint64_t)sizes.vertex_words*4),output=geometry_buffer((uint64_t)sizes.geometry_words*4),emitted=geometry_buffer((uint64_t)sizes.index_words*4);
    nvn_geo_params p={0};p.count=nv;p.vertex_count=nv;p.instances=ni;p.primitive=primitive;p.primitive_count=sizes.primitives;p.index_stride=sizes.index_stride;
    p.base_instance=(uint32_t)(dw?dw->base_instance:di->base_instance);p.reserved=(uint32_t)(dw?dw->first:di->base_vertex);
    p.restart_enable=S.restart.enabled;p.restart_index=S.restart.index;
    uint32_t default_attribute[4]={0,0,0,0x3f800000};uint64_t default_gpu=geometry_data(default_attribute,sizeof default_attribute),source_gpu=default_gpu;
    if(di) {
        if(di->itype<0 || di->itype>2) MDIE("【红】几何索引类型无效");
        p.index_bytes=1u<<di->itype;uint64_t bytes=(uint64_t)nv*p.index_bytes;void *mb;uint64_t offset;
        if(bytes>UINT32_MAX || !mtl_gpu_addr(di->ibuf,bytes,&mb,&offset,&source_gpu)) MDIE("【红】几何索引范围不在内存池内");
        p.source_bytes=(uint32_t)bytes;use_rec("buf",di->ibuf,bytes,0,NULL);
    }
    uint64_t index_args[]={source_gpu,indices.gpuAddress};geometry_prepare(@"geo_indices",p,index_args,2,nv);
    uint32_t info[264]={nv,ni,p.reserved,p.base_instance,sizes.primitives,0,0,0};
    NSMutableDictionary<NSString *,NSNumber *> *resources=[NSMutableDictionary dictionary];
    resources[@"vertex_output"]=@(vertex.gpuAddress);resources[@"geometry_output"]=@(output.gpuAddress);resources[@"geometry_indices"]=@(emitted.gpuAddress);
    resources[@"indices"]=@(geometry_texture(indices).gpuResourceID._impl);resources[@"topology"]=@(geometry_texture(topology).gpuResourceID._impl);
    NSMutableSet<NSNumber *> *locations=[NSMutableSet set];
    for(NSDictionary *set in g->sets[0])for(NSDictionary *binding in set[@"bindings"])if([binding[@"kind"] isEqual:@"attribute"]) [locations addObject:binding[@"location"]];
    for(NSNumber *location in locations) {
        unsigned a=location.unsignedIntValue;if(a>=16) MDIE("【红】几何顶点属性编号越界");
        id<MTLBuffer> expanded=geometry_buffer((uint64_t)nv*ni*16);nvn_geo_params fetch=p;fetch.count=nv*ni;
        uint64_t raw=default_gpu;fetch.format=0x2e;fetch.stride=0;fetch.source_bytes=16;fetch.attribute_offset=0;
        if(a<(unsigned)S.nattr && S.attr[a].fmt) {
            va_st at=S.attr[a];if(at.stream>=16 || !nvn_geo_format_bytes(at.fmt)) MDIE("【红】几何顶点格式或流不支持");
            const NVNbufferRange *range=&S.vbuf[at.stream];void *mb;uint64_t offset;
            if(!range->address || !mtl_gpu_addr(range->address,1,&mb,&offset,&raw)) MDIE("【红】几何顶点缓冲未绑定");
            uint64_t bytes=MIN(range->size,((__bridge id<MTLBuffer>)mb).length-offset);
            fetch.source_bytes=(uint32_t)MIN(bytes,UINT32_MAX);fetch.format=at.fmt;fetch.attribute_offset=at.offset;
            fetch.stride=S.stream[at.stream].stride;fetch.divisor=S.stream[at.stream].divisor;use_rec("buf",range->address,range->size,0,NULL);
        }
        uint64_t fetch_args[]={raw,expanded.gpuAddress,indices.gpuAddress};geometry_prepare(@"geo_fetch",fetch,fetch_args,3,nv*ni);
        info[8+a*4]=4;info[9+a*4]=info[10+a*4]=info[11+a*4]=1;
        resources[[NSString stringWithFormat:@"attribute%u",a]]=@(geometry_texture(expanded).gpuResourceID._impl);
    }
    resources[@"info"]=@(geometry_data(info,sizeof info));
    uint64_t topology_args[]={indices.gpuAddress,topology.gpuAddress,valid.gpuAddress};geometry_prepare(@"geo_topology",p,topology_args,3,1);
    uint64_t init_args[]={emitted.gpuAddress};geometry_prepare(@"geo_initialize_indices",p,init_args,1,sizes.index_words);
    for(unsigned role=0;role<2;role++) {
        id e=enc_c_begin(NULL);enc_c_pso(e,g->compute[role]);geometry_bind(g,role,resources,e);
        if(g->compute[role].maxTotalThreadsPerThreadgroup<32) MDIE("【红】几何计算线程组不支持");
        unsigned x=role?sizes.primitives:nv;enc_c_dispatch(e,MTLSizeMake((x+7)/8,(ni+3)/4,1),MTLSizeMake(8,4,1));enc_c_end(e);
    }
    uint64_t mask_args[]={valid.gpuAddress,emitted.gpuAddress};geometry_prepare(@"geo_mask_indices",p,mask_args,2,sizes.index_words);
    id en=rp_begin_ex(0);if(!en) return;
    id<MTLRenderPipelineState> pipeline=pso_get(program->geometry_render,nil);if(!pipeline) return;
    enc_r_pso(en,pipeline);g_enc_pso=pipeline;
    id<MTLDepthStencilState> depth=dss_get(g_pass.hd,g_pass.hs);enc_r_dss(en,depth);g_enc_dss=depth;
    enc_r_stencil_ref(en,S.st_ref[0],S.st_ref[1]);
    NSUInteger width=g_pass.tw,height=g_pass.th;
    enc_r_viewport(en,S.has_vp?(MTLViewport){S.vp[0][0],S.vp[0][1],S.vp[0][2],S.vp[0][3],S.dr[0],S.dr[1]}:(MTLViewport){0,0,width,height,0,1});
    MTLScissorRect sc={0,0,width,height};
    if(R.has_sc) {
        int64_t x0=MAX((int64_t)R.sc.x,0),y0=MAX((int64_t)R.sc.y,0),x1=MIN((int64_t)R.sc.x+R.sc.w,(int64_t)width),y1=MIN((int64_t)R.sc.y+R.sc.h,(int64_t)height);
        if(x1<=x0 || y1<=y0) return;sc=(MTLScissorRect){x0,y0,x1-x0,y1-y0};
    }
    enc_r_scissor(en,sc);enc_r_cull(en,S.poly.cull==NVN_FACE_FRONT?MTLCullModeFront:S.poly.cull==NVN_FACE_BACK?MTLCullModeBack:MTLCullModeNone);
    enc_r_winding(en,S.poly.front==NVN_FRONT_FACE_CCW?MTLWindingCounterClockwise:MTLWindingClockwise);enc_r_fill(en,S.poly.mode==NVN_POLYGON_MODE_LINE?MTLTriangleFillModeLines:MTLTriangleFillModeFill);
    if(S.poly.offset_en&4)enc_r_depth_bias(en,S.po.units,S.po.factor,S.po.clamp);else enc_r_depth_bias(en,0,0,0);
    enc_r_blend_color(en,S.blend_color[0],S.blend_color[1],S.blend_color[2],S.blend_color[3]);
    geometry_bind(g,2,resources,en);if(program->sh[1])encode_args(en,program->sh[1],1,0);
    if(g_vis_counting && g_enc_has_vis) {
        if(g_vis_cur<0 || g_vis_cur_pass!=g_pass.serial) {
            if(g_vis_n>=VIS_SLOTS) MDIE("【红】几何可见性槽用尽");g_vis_cur=(int)g_vis_n++;g_vis_cur_pass=g_pass.serial;
            if(g_nvs==g_vscap){g_vscap=g_vscap?g_vscap*2:64;g_vs=realloc(g_vs,g_vscap*sizeof *g_vs);}
            g_vs[g_nvs++]=(vis_slot){(void*)CFBridgingRetain(g_vis),(uint32_t)g_vis_cur};
        }
        enc_r_visibility(en,MTLVisibilityResultModeCounting,(NSUInteger)g_vis_cur*8);g_enc_vis=g_vis_cur+1;
    }
    g_pass.range_external=1;rt_range_note_depth();udc_note_draw(program);
    enc_r_draw_indexed(en,MTLPrimitiveTypeTriangleStrip,sizes.index_words,MTLIndexTypeUInt32,emitted.gpuAddress,emitted.length,1,0,0);
    g_stat_draw++;g_geometry_draws++;g_pso_dirty=g_dss_dirty=1;args_dirty_all();
    if(g_geometry_draws==1) MLOG("几何计算绘制首例：顶点 %u、图元 %u、实例 %u",nv,sizes.primitives,ni);
}
