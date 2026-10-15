// 空缓存的挂载状态、标准错误码和对象不改写回归；SDK 对象生命周期另用原始指令验证。
// Regress empty-cache mount state, standard results and untouched objects; verify SDK lifecycle separately with original instructions.
#include "../hle_bcat.c"
static int checks;
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;} checks++; } while(0)
int main(void) {
    uint64_t dir=0,file=0; uint8_t progress[0x258], saved[0x258];
    memset(progress,0xcd,sizeof progress); memcpy(saved,progress,sizeof saved);
    const char directory[32]="News", name[32]="Event.bin";
    CHECK(h_bcat_Unmount()==0xa7a);
    CHECK(h_bcat_DirOpen(&dir,directory)==0xa7a);
    CHECK(h_bcat_FileOpen(&file,directory,name)==0xa7a);
    h_bcat_Init(); CHECK(h_bcat_Mount()==0);
    CHECK(h_bcat_Mount()==0x87a);
    for(int i=0;i<4;i++) {
        CHECK(h_bcat_DirOpen(&dir,directory)==0x47a && dir==0);
        CHECK(h_bcat_FileOpen(&file,directory,name)==0x47a && file==0);
        CHECK(h_bcat_Sync(progress)==0x107a);
        CHECK(!memcmp(progress,saved,sizeof progress));
    }
    h_bcat_Init(); CHECK(h_bcat_Mount()==0x87a);
    CHECK(h_bcat_Unmount()==0);
    CHECK(h_bcat_Mount()==0);
    CHECK(h_bcat_Unmount()==0);
    CHECK(h_bcat_Sync(progress)==0x107a && !memcmp(progress,saved,sizeof progress));
    printf("BCAT_EMPTY_CACHE checks=%d failures=0; no distributed content or network service provided\n",checks);
    return 0;
}
