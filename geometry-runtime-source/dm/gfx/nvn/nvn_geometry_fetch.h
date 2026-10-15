// 几何计算路径的容量规划与 GPU 顶点/索引准备；所有源数据在同一 GPU 队列读取。
// Capacity planning and GPU vertex/index preparation for geometry emulation; read all source data on the same GPU queue.
#pragma once
#include <stdint.h>
#include <limits.h>

typedef struct {
    uint32_t count, format, stride, divisor;
    uint32_t base_instance, source_bytes, index_bytes, primitive;
    uint32_t restart_enable, restart_index, vertex_count, instances;
    uint32_t primitive_count, index_stride, attribute_offset, reserved;
} nvn_geo_params;

typedef struct {
    uint32_t primitives, index_stride, vertex_words, geometry_words, index_words;
} nvn_geo_sizes;

static int nvn_geo_format_bytes(uint32_t format) {
    static const uint8_t bytes[] = {1,1,1,1,2,2,2,2,2,4,4,4};
    if (format >= 1 && format <= 0x30) return bytes[(format-1)%12] * (1+(format-1)/12);
    return format == 0x3d || format == 0x69 || format == 0x3f || format == 0x9a ? 4 : 0;
}
static int nvn_geo_plan(uint32_t vertices, uint32_t instances, uint32_t primitive,
                         uint32_t vs_words, uint32_t gs_words, uint32_t max_output,
                         nvn_geo_sizes *out) {
    if (!instances || !vs_words || !gs_words || !max_output || !out) return 0;
    uint32_t primitives;
    switch (primitive) {
        case 4: primitives = vertices/3; break;
        case 5: case 6: primitives = vertices > 2 ? vertices-2 : 0; break;
        case 7: primitives = vertices/4*2; break;
        default: return 0;
    }
    uint64_t slots = (uint64_t)vertices*instances;
    uint64_t stride = (uint64_t)max_output + max_output/3;
    if (slots > INT32_MAX || stride > UINT32_MAX ||
        slots > INT32_MAX/vs_words || slots > INT32_MAX/gs_words/max_output ||
        (stride && slots > UINT32_MAX/stride)) return 0;
    *out = (nvn_geo_sizes){primitives,(uint32_t)stride,(uint32_t)(slots*vs_words),
                         (uint32_t)(slots*gs_words*max_output),(uint32_t)(slots*stride)};
    return 1;
}

// 宏只把以下 Metal 源码字符串化；宿主和 GPU 测试编译同一份内核。
// The macro only stringifies the Metal source below; the host and GPU tests compile the same kernels.
#define NVN_GEO_MSL(...) #__VA_ARGS__
static const char k_nvn_geometry_fetch_msl[] = "#include <metal_stdlib>\n" NVN_GEO_MSL(
using namespace metal;
struct GeoParams {
    uint count, format, stride, divisor;
    uint base_instance, source_bytes, index_bytes, primitive;
    uint restart_enable, restart_index, vertex_count, instances;
    uint primitive_count, index_stride, attribute_offset, reserved;
};
uint geo_read(device const uchar *source, ulong offset, uint bytes) {
    uint value=0;
    for (uint i=0;i<bytes;i++) value |= uint(source[offset+i])<<(8*i);
    return value;
}
float geo_scalar(uint value, uint type) {
    switch (type) {
        case 1: return float(value)/255.0f;
        case 2: return max(-1.0f,float(int(value<<24)>>24)/127.0f);
        case 3: return float(value);
        case 4: return float(int(value<<24)>>24);
        case 5: return float(as_type<half>(ushort(value)));
        case 6: return float(value)/65535.0f;
        case 7: return max(-1.0f,float(int(value<<16)>>16)/32767.0f);
        case 8: return float(value);
        case 9: return float(int(value<<16)>>16);
        case 10: return as_type<float>(value);
        case 11: return float(value);
        case 12: return float(as_type<int>(value));
    }
    return 0.0f;
}
float geo_ufloat(uint value, uint mantissa_bits) {
    uint exponent=value>>mantissa_bits, mantissa=value&((1u<<mantissa_bits)-1u);
    if (exponent==31) return as_type<float>(mantissa ? 0x7fc00000u : 0x7f800000u);
    if (exponent==0) return ldexp(float(mantissa),-14-int(mantissa_bits));
    return ldexp(1.0f+float(mantissa)/float(1u<<mantissa_bits),int(exponent)-15);
}
kernel void geo_fetch(constant GeoParams &p [[buffer(0)]], device const uchar *source [[buffer(1)]],
                      device float4 *output [[buffer(2)]], device const uint *indices [[buffer(3)]],
                      uint index [[thread_position_in_grid]]) {
    if (index>=p.count || !p.vertex_count) return;
    uint ordinal=index%p.vertex_count, instance=index/p.vertex_count;
    long element=p.stride==0 ? 0 : p.divisor ? long(p.base_instance)+long(instance/p.divisor) : long(indices[ordinal])+long(as_type<int>(p.reserved));
    if (element<0) { output[index]=float4(0.0f,0.0f,0.0f,1.0f); return; }
    ulong at=ulong(p.attribute_offset)+ulong(element)*p.stride;
    float4 result=float4(0.0f,0.0f,0.0f,1.0f);
    if (p.format>=1 && p.format<=0x30) {
        uint type=(p.format-1)%12+1, components=(p.format-1)/12+1;
        uint bytes=type<=4 ? 1 : type<=9 ? 2 : 4;
        if (at+components*bytes<=p.source_bytes)
            for (uint component=0;component<components;component++)
                result[component]=geo_scalar(geo_read(source,at+component*bytes,bytes),type);
    } else if (at+4<=p.source_bytes) {
        uint value=geo_read(source,at,4);
        if (p.format==0x3d) result=float4(value&1023u,(value>>10)&1023u,(value>>20)&1023u,value>>30)/float4(1023.0f,1023.0f,1023.0f,3.0f);
        else if (p.format==0x69) {
            result=float4(int(value<<22)>>22,int(value<<12)>>22,int(value<<2)>>22,as_type<int>(value)>>30);
            result=max(float4(-1.0f),result/float4(511.0f,511.0f,511.0f,1.0f));
        } else if (p.format==0x3f) result=float4(geo_ufloat(value&2047u,6),geo_ufloat((value>>11)&2047u,6),geo_ufloat(value>>22,5),1.0f);
        else if (p.format==0x9a) result=float4((value>>16)&255u,(value>>8)&255u,value&255u,value>>24)/255.0f;
    }
    output[index]=result;
}
kernel void geo_indices(constant GeoParams &p [[buffer(0)]], device const uchar *source [[buffer(1)]],
                        device uint *output [[buffer(2)]], uint index [[thread_position_in_grid]]) {
    if (index>=p.count) return;
    ulong at=ulong(index)*p.index_bytes;
    bool supported=p.index_bytes==1 || p.index_bytes==2 || p.index_bytes==4;
    output[index]=!p.index_bytes ? index : supported && at+p.index_bytes<=p.source_bytes ? geo_read(source,at,p.index_bytes) : 0xffffffffu;
}
void geo_emit(device uint *topology, device uint *valid, uint capacity, thread uint &count, uint a, uint b, uint c) {
    if (count>=capacity) return;
    topology[count*3]=a; topology[count*3+1]=b; topology[count*3+2]=c; valid[count]=1; count++;
}
kernel void geo_topology(constant GeoParams &p [[buffer(0)]], device const uint *indices [[buffer(1)]],
                         device uint *topology [[buffer(2)]], device uint *valid [[buffer(3)]],
                         uint index [[thread_position_in_grid]]) {
    if (index) return;
    for (uint i=0;i<p.primitive_count;i++) { topology[i*3]=0; topology[i*3+1]=0; topology[i*3+2]=0; valid[i]=0; }
    uint group[4], n=0, count=0;
    uint mask=p.index_bytes==1 ? 255u : p.index_bytes==2 ? 65535u : 0xffffffffu;
    for (uint i=0;i<p.vertex_count;i++) {
        if (p.index_bytes && p.restart_enable && indices[i]==(p.restart_index&mask)) { n=0; continue; }
        if (p.primitive==4) {
            group[n++]=i;
            if (n==3) { geo_emit(topology,valid,p.primitive_count,count,group[0],group[1],group[2]); n=0; }
        } else if (p.primitive==7) {
            group[n++]=i;
            if (n==4) { geo_emit(topology,valid,p.primitive_count,count,group[0],group[1],group[2]); geo_emit(topology,valid,p.primitive_count,count,group[0],group[2],group[3]); n=0; }
        } else {
            if (n==0) group[0]=i;
            else if (n==1) group[1]=i;
            else if (p.primitive==5) {
                bool odd=((n-2)&1)!=0;
                geo_emit(topology,valid,p.primitive_count,count,group[odd?1:0],group[odd?0:1],i);
                group[0]=group[1]; group[1]=i;
            } else { geo_emit(topology,valid,p.primitive_count,count,group[0],group[1],i); group[1]=i; }
            n++;
        }
    }
}
kernel void geo_initialize_indices(constant GeoParams &p [[buffer(0)]], device uint *output [[buffer(1)]],
                                   uint index [[thread_position_in_grid]]) {
    if (index<p.vertex_count*p.instances*p.index_stride) output[index]=0xffffffffu;
}
kernel void geo_mask_indices(constant GeoParams &p [[buffer(0)]], device const uint *valid [[buffer(1)]],
                             device uint *output [[buffer(2)]], uint index [[thread_position_in_grid]]) {
    uint total=p.vertex_count*p.instances*p.index_stride;
    if (index>=total) return;
    uint primitive=(index/p.index_stride)%p.vertex_count;
    if (primitive>=p.primitive_count || !valid[primitive]) output[index]=0xffffffffu;
}
);
#undef NVN_GEO_MSL
