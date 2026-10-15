// 几何缓冲容量上界回归，避免原型中实例间空洞被错误地按图元数分配。
// Geometry capacity regressions, preventing allocation by primitive count when the lowered layout leaves instance gaps.
#include "../nvn/nvn_geometry_fetch.h"
#include <stdio.h>

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); } } while (0)
int main(void) {
    nvn_geo_sizes p;
    CHECK(sizeof(nvn_geo_params)==64);
    CHECK(nvn_geo_plan(6,2,4,17,29,6,&p));
    CHECK(p.primitives==2 && p.index_stride==8 && p.vertex_words==204 && p.geometry_words==2088 && p.index_words==96);
    CHECK(nvn_geo_plan(8,2,5,13,25,6,&p) && p.primitives==6);
    CHECK(nvn_geo_plan(8,2,6,13,25,6,&p) && p.primitives==6);
    CHECK(nvn_geo_plan(8,2,7,13,25,6,&p) && p.primitives==4);
    CHECK(nvn_geo_plan(0,1,4,13,25,6,&p) && p.primitives==0 && p.geometry_words==0);
    CHECK(!nvn_geo_plan(UINT32_MAX,UINT32_MAX,4,17,29,6,&p));
    CHECK(!nvn_geo_plan(65536,65536,4,17,29,6,&p));
    CHECK(!nvn_geo_plan(6,0,4,17,29,6,&p));
    CHECK(!nvn_geo_plan(6,1,1,17,29,6,&p));
    CHECK(!nvn_geo_plan(6,1,4,17,29,UINT32_MAX,&p));
    CHECK(nvn_geo_format_bytes(0x16)==8 && nvn_geo_format_bytes(0x22)==12 && nvn_geo_format_bytes(0x2e)==16);
    CHECK(nvn_geo_format_bytes(0x29)==8 && nvn_geo_format_bytes(0x3d)==4 && nvn_geo_format_bytes(0x69)==4);
    CHECK(!nvn_geo_format_bytes(0) && !nvn_geo_format_bytes(0xff));
    CHECK(k_nvn_geometry_fetch_msl[0]=='#');
    printf("GEOMETRY_PLAN checks=%d failures=%d\n",checks,failures);
    return failures!=0;
}
