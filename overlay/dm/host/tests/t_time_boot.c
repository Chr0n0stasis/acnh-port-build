// 实际 Apple 时区数据库与完整调用入口回归；预期日期/偏移独立给定，不从被测实现生成。
// Exercise the real Apple zone database and public entry points with independently specified dates and offsets.
#include "../hle_time.c"
#include "../hle_time_zone.c"
module_t g_mod[MAX_MOD]; int g_nmod; uint64_t now_ns(void) { return 0; }
void *module_sym(module_t *m, const char *name) { (void)m; (void)name; return NULL; }
static int checks;
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;} checks++; } while(0)
static Result named_rule(TimeZoneRule *out, const char *name) {
    TimeLocationName location={0}; strlcpy(location.name,name,sizeof location.name); return h_time_LoadZone(out,&location);
}
int main(int argc,char **argv) {
    int fixed=argc>1 && !strcmp(argv[1],"--fixed");
    if(fixed) setenv("DM_FIXED_TIME","1792022400",1); else unsetenv("DM_FIXED_TIME");
    int64_t before=time(NULL),current=0; CHECK(h_time_Initialize()==0 && h_time_IsInitialized());
    h_time_Adjust(); h_time_Adjust(); CHECK(h_time_AdjustableCurrent(&current)==0);
    CHECK(fixed ? current==1792022400 : current>=before && current<=time(NULL));
    int64_t standard=0; CHECK(h_time_GetCurrentTime(&standard)==0);
    CHECK(llabs(standard-current)<=1); CHECK(llabs(h_time_AdjustableNow()-standard)<=1);
    struct { uint8_t pre[16]; TimeLocationName name; uint8_t post[16]; } location;
    memset(&location,0xcd,sizeof location); h_time_DeviceZone(&location.name);
    CHECK(memchr(location.name.name,0,36) && strlen(location.name.name)>0);
    for(int i=0;i<16;i++) CHECK(location.pre[i]==0xcd && location.post[i]==0xcd);
    struct { uint64_t pre; TimeZoneRule rule; uint64_t post; } storage={.pre=UINT64_MAX,.post=UINT64_MAX};
    CHECK(h_time_LoadZone(&storage.rule,&location.name)==0);
    CHECK(storage.pre==UINT64_MAX && storage.post==UINT64_MAX);
    TimeZoneRule rule,copy; CHECK(named_rule(&rule,"Asia/Shanghai")==0); memcpy(&copy,&rule,sizeof copy);
    memset(&rule,0xcd,sizeof rule);
    struct { uint64_t pre; CalendarTime value; uint64_t post; } calendar={.pre=UINT64_MAX,.post=UINT64_MAX};
    struct { uint64_t pre; CalendarAdditionalInfo value; uint64_t post; } info={.pre=UINT64_MAX,.post=UINT64_MAX};
    int64_t epoch=1792022400;
    CHECK(h_time_CalendarRule(&calendar.value,&info.value,&epoch,&copy)==0);
    CalendarTime expected={2026,10,15,8,0,0,0}; CHECK(!memcmp(&calendar.value,&expected,sizeof expected));
    CHECK(info.value.offset==28800 && info.value.wday==4 && info.value.yday==287 && info.value.dst==0);
    CHECK(calendar.pre==UINT64_MAX && calendar.post==UINT64_MAX && info.pre==UINT64_MAX && info.post==UINT64_MAX);
    int count=-1; int64_t times[3]={-1,-1,INT64_MAX};
    CHECK(h_time_PosixRule(&count,times,2,&expected,&copy)==0 && count==1 && times[0]==epoch && times[1]==-1 && times[2]==INT64_MAX);
    CHECK(h_time_PosixRule(&count,NULL,0,&expected,&copy)==0 && count==0);
    CHECK(h_time_PosixRule(&count,times,-1,&expected,&copy)!=0);
    TimeLocationName invalid; memset(&invalid,'x',sizeof invalid);
    CHECK(h_time_LoadZone(&rule,&invalid)!=0);
    CHECK(named_rule(&rule,"Definitely/NotAZone")!=0);
    CHECK(h_time_CalendarRule(&calendar.value,&info.value,&epoch,&rule)!=0);
    CHECK(named_rule(&rule,"America/New_York")==0);
    CalendarTime repeated={2026,11,1,1,30,0,0}; times[0]=times[1]=-1;
    CHECK(h_time_PosixRule(&count,times,2,&repeated,&rule)==0 && count==2);
    CHECK(times[0]==1793511000 && times[1]==1793514600 && times[2]==INT64_MAX);
    CHECK(h_time_CalendarRule(&calendar.value,&info.value,&times[0],&rule)==0 && info.value.dst==1 && info.value.offset==-14400);
    CHECK(h_time_CalendarRule(&calendar.value,&info.value,&times[1],&rule)==0 && info.value.dst==0 && info.value.offset==-18000);
    times[1]=-1; CHECK(h_time_PosixRule(&count,times,1,&repeated,&rule)==0 && count==1 && times[0]==1793511000 && times[1]==-1);
    CalendarTime skipped={2026,3,8,2,30,0,0}; times[0]=times[1]=-1;
    CHECK(h_time_PosixRule(&count,times,2,&skipped,&rule)==0 && count==0 && times[0]==-1 && times[1]==-1);
    CHECK(named_rule(&rule,"Australia/Lord_Howe")==0);
    CalendarTime halfhour={2026,4,5,1,45,0,0};
    CHECK(h_time_PosixRule(&count,times,2,&halfhour,&rule)==0 && count==2 && times[0]==1775313900 && times[1]==1775315700);
    CHECK(named_rule(&rule,"Pacific/Apia")==0); CalendarTime skipped_day={2011,12,30,12,0,0,0};
    CHECK(h_time_PosixRule(&count,times,2,&skipped_day,&rule)==0 && count==0);
    CHECK(named_rule(&rule,"Etc/UTC")==0); CalendarTime leap={2024,2,29,12,34,56,0};
    CHECK(h_time_PosixRule(&count,times,2,&leap,&rule)==0 && count==1);
    CHECK(h_time_CalendarRule(&calendar.value,&info.value,&times[0],&rule)==0 && !memcmp(&calendar.value,&leap,sizeof leap) && info.value.yday==59);
    CalendarTime bad={2025,2,29,0,0,0,0}; CHECK(h_time_PosixRule(&count,times,2,&bad,&rule)!=0);
    printf("TIME_BOOT checks=%d failures=0 fixed=%d; real CoreFoundation timezone conversions, not device startup\n",checks,fixed);
    return 0;
}
