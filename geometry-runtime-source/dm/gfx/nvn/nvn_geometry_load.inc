// 在同一翻译链内按 VS/GS/PS 身份载入降级程序；缺失元数据必须明确失败，不能把 GS 静默丢掉。
// Load lowered programs by VS/GS/PS identity in the same chain; missing metadata must fail explicitly rather than silently dropping GS.
static NvGeometry *geometry_load(const char *vertex, const char *geometry, const char *fragment) {
    static NSMutableDictionary<NSString *,NvGeometry *> *cache;
    NSString *vs=[[NSString stringWithUTF8String:vertex] substringToIndex:16];
    NSString *gs=[[NSString stringWithUTF8String:geometry] substringToIndex:16];
    NSString *ps=[[NSString stringWithUTF8String:fragment] substringToIndex:16];
    if(!cache) cache=[NSMutableDictionary dictionary];
    NvGeometry *g=cache[gs];
    if(g) {
        if(![g->metadata[@"vs"] isEqual:vs] || ![g->metadata[@"ps"] isEqual:ps]) MDIE("【红】几何程序配对身份不符");
        return g;
    }
    NSString *path=[g_msl_dir stringByAppendingPathComponent:[gs stringByAppendingString:@".geometry.json"]];
    NSData *data=[NSData dataWithContentsOfFile:path]; NSError *error=nil;
    NSDictionary *m=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:&error]:nil;
    if(![m isKindOfClass:NSDictionary.class] || ![m[@"chain"] isEqual:@(DMSH_CHAIN)] || ![m[@"compiled"] boolValue] ||
       ![m[@"vs"] isEqual:vs] || ![m[@"gs"] isEqual:gs] || ![m[@"ps"] isEqual:ps] ||
       ![m[@"attribute_layout"] isEqual:@"draw-order-float4"] || [m[@"vertex_info_bytes"] intValue]!=1056 ||
       [m[@"vertices_per_primitive"] intValue]!=3 || [m[@"invocations"] intValue]!=1)
        MDIE("【红】几何元数据缺失、过期或布局不兼容：%s",path.UTF8String);
    unsigned maximum=[m[@"max_output_vertices"] unsignedIntValue];
    if(!maximum || maximum%3 || maximum>32 || [m[@"index_stride"] unsignedIntValue]!=maximum+maximum/3)
        MDIE("【红】几何输出尺寸不兼容：%s",geometry);
    g=[NvGeometry new];g->metadata=m;
    NSArray *roles=@[@"vertex_compute",@"geometry_compute",@"passthrough_vertex"];
    for(unsigned role=0;role<3;role++) {
        NSDictionary *p=m[@"programs"][roles[role]];
        NSString *library=[NSString stringWithFormat:@"%@_%@.metallib",gs,roles[role]];
        if(![p[@"library"] isEqual:library] || ![p[@"entry"] isEqual:@"main0"] || ![p[@"sets"] isKindOfClass:NSArray.class])
            MDIE("【红】几何程序库布局不兼容：%s",geometry);
        g->sets[role]=p[@"sets"];
        unsigned seen=0;
        for(NSDictionary *set in g->sets[role]) {
            unsigned index=[set[@"index"] unsignedIntValue],words=[set[@"words"] unsignedIntValue];
            if(index>2 || (seen&(1u<<index)) || !words || words>32) MDIE("【红】几何参数集合越界");
            seen|=1u<<index; unsigned ids=0;
            for(NSDictionary *binding in set[@"bindings"]) {
                unsigned aid=[binding[@"id"] unsignedIntValue];
                if(aid>=words || (ids&(1u<<aid))) MDIE("【红】几何参数编号越界或重复");
                ids|=1u<<aid;
            }
        }
        id<MTLLibrary> lib=lib_from_file([g_msl_dir stringByAppendingPathComponent:library],&error);
        id<MTLFunction> fn=[lib newFunctionWithName:p[@"entry"]];
        if(!fn) MDIE("【红】几何程序载入失败：%s",library.UTF8String);
        if(role==2) {
            g->passthrough=[NvShader new];g->passthrough->fn=fn;g->passthrough->stage=0;
            snprintf(g->passthrough->key,sizeof g->passthrough->key,"%s_gv",geometry);
        } else {
            g->compute[role]=[g_dev newComputePipelineStateWithFunction:fn error:&error];
            if(!g->compute[role]) MDIE("【红】几何计算管线创建失败：%s",error.localizedDescription.UTF8String);
            NSString *hex=p[@"c1"];
            if(![hex isKindOfClass:NSString.class] || (hex.length&1) || hex.length>131072) MDIE("【红】几何 c1 长度无效");
            g->constants[role]=[g_dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
            if(!g->constants[role]) MDIE("【红】几何 c1 分配失败");
            memset(g->constants[role].contents,0,65536);
            unsigned char *bytes=g->constants[role].contents;
            for(NSUInteger i=0;i<hex.length/2;i++) {
                unsigned value=0;if(sscanf([[hex substringWithRange:NSMakeRange(i*2,2)] UTF8String],"%2x",&value)!=1) MDIE("【红】几何 c1 编码无效");
                bytes[i]=(unsigned char)value;
            }
            res_add(g->constants[role]);
        }
    }
    cache[gs]=g;MLOG("几何计算路径已载入：%s",geometry);return g;
}
