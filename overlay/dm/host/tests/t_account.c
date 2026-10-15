// 用户枚举容量与跨接口身份一致性：使用真实宿主实现，不进入 Switch 服务。
// Check enumeration bounds and identity consistency across the real host account interfaces.
#include "../hle_account.c"
void hle_signal_system_event(void *event) { (void)event; }
static int checks;
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;} checks++; } while(0)
int main(void) {
    h_acc_Init();
    struct { uint64_t pre; Uid ids[8]; uint64_t post; } all;
    memset(&all,0xcd,sizeof all);
    int count=-1;
    CHECK(h_acc_ListAll(&count,all.ids,0)==0 && count==0);
    for(unsigned i=0;i<sizeof all;i++) CHECK(((unsigned char *)&all)[i]==0xcd);
    CHECK(h_acc_ListAll(&count,all.ids,8)==0 && count==1);
    CHECK(all.pre==0xcdcdcdcdcdcdcdcdull && all.post==0xcdcdcdcdcdcdcdcdull);
    for(unsigned i=sizeof(Uid);i<sizeof all.ids;i++) CHECK(((unsigned char *)all.ids)[i]==0xcd);
    Uid qualified,opened;
    int n=-1;
    CHECK(h_acc_ListQual(&n,&qualified,1)==0 && n==1 && !memcmp(&qualified,&all.ids[0],sizeof(Uid)));
    CHECK(h_acc_ListOpen(&n,&opened,1)==0 && n==1 && !memcmp(&opened,&all.ids[0],sizeof(Uid)));
    CHECK(h_acc_UserCount(&n)==0 && n==1);
    uint8_t exists=0;
    CHECK(h_acc_Exists(&exists,&all.ids[0])==0 && exists);
    UserHandle handle;
    Uid back;
    CHECK(h_acc_OpenUser(&handle,&all.ids[0])==0);
    CHECK(h_acc_GetUserId(&back,&handle)==0 && !memcmp(&back,&all.ids[0],sizeof(Uid)));
    h_acc_CloseUser(&handle);
    printf("ACCOUNT_LOCAL checks=%d failures=0; enumeration and user handles share one identity\n",checks);
    return 0;
}
