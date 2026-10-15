// 共享字体加载与枚举使用真实 TTF，检查 BFTTF 包装、容量边界、语言顺序及扩展字体回退。
// Load and enumerate real TTFs, checking BFTTF framing, capacity boundaries, language order and extension fallback.
#include "../hle_sharedfont.c"
#include <stddef.h>
static int checks;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } checks++; } while (0)
static uint64_t language(const char *s) { uint64_t v=0; memcpy(&v,s,strlen(s)>8?8:strlen(s)); return v; }
int main(int argc, char **argv) {
    CHECK(argc==3);
    CHECK(!setenv("DM_SHARED_FONT",argv[1],1));
    CHECK(!setenv("DM_SHARED_FONT_EXT",argv[2],1));
    unsetenv("DM_FONT_DIR");
    CHECK(offsetof(shared_font_data,address)==8 && offsetof(shared_font_data,size)==16);
    for (int type=0; type<6; type++) {
        h_pl_RequestLoad(type);
        CHECK(h_pl_LoadState(type)==1);
        font_t f=font_get(type);
        CHECK(f.p && f.n>12 && h_pl_Addr(type)==f.p && h_pl_Size(type)==f.n);
        uint32_t marker, encoded; memcpy(&marker,f.p,4); memcpy(&encoded,f.p+4,4);
        CHECK(marker==0x18029a7fu && (__builtin_bswap32(encoded)^0x49621806u)==f.n-8);
        CHECK(!memcmp(f.p+8,"\0\1\0\0",4) || !memcmp(f.p+8,"OTTO",4) || !memcmp(f.p+8,"ttcf",4));
    }
    shared_font_data data[8]; memset(data,0xcd,sizeof data); int count=-1;
    CHECK(h_pl_Get(&count,data,0,language("en-US"))==1 && count==6);
    CHECK(((uint8_t*)data)[0]==0xcd);
    CHECK(h_pl_Get(&count,data,1,language("zh-Hans"))==1 && count==6 && data[0].type==1);
    CHECK(((uint8_t*)&data[1])[0]==0xcd);
    CHECK(h_pl_Get(&count,data,6,language("zh-Hant"))==1 && count==6 && data[0].type==3);
    unsigned seen=0;
    for(int k=0;k<6;k++) {
        CHECK((unsigned)data[k].type<6 && !(seen&(1u<<data[k].type)));
        seen|=1u<<data[k].type;
        CHECK(data[k].address==h_pl_Addr(data[k].type) && data[k].size==h_pl_Size(data[k].type) && data[k].reserved==0);
    }
    CHECK(seen==63 && ((uint8_t*)&data[6])[0]==0xcd);
    CHECK(h_pl_Get(&count,data,6,language("ko"))==1 && data[0].type==4);
    CHECK(h_pl_Get(&count,NULL,0,language("ja"))==1 && count==6);
    CHECK(h_pl_Get(&count,data,1,language("en-US"))==1 && data[0].type==0);
    if(!strcmp(argv[2],"/missing-font.ttf")) CHECK(h_pl_Addr(5)==h_pl_Addr(0));
    printf("SHARED_FONT checks=%d failures=0; real font framing and enumeration only\n",checks);
    return 0;
}
