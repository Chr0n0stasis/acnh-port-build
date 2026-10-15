// 时区规则是可复制的 16 KiB 值；保存名称而不保存宿主指针，所有使用该规则的公开转换都由宿主接管。
// Time-zone rules are copyable 16 KiB values; store a name rather than host pointers and take over public rule conversions.
#include "hle.h"
#include "dm_time_types.h"
#include <CoreFoundation/CoreFoundation.h>
#include <math.h>

#define DM_TZ_MAGIC UINT64_C(0x315a544d44484c45)
static CFTimeZoneRef zone_named(const char name[36]) {
    size_t n = strnlen(name, 36); if (!n || n==36) return NULL;
    CFStringRef s=CFStringCreateWithBytes(NULL,(const UInt8*)name,n,kCFStringEncodingUTF8,false);
    if (!s) return NULL;
    CFTimeZoneRef z=CFTimeZoneCreateWithName(NULL,s,false); CFRelease(s); return z;
}
static CFTimeZoneRef zone_rule(const TimeZoneRule *rule) {
    uint64_t magic=0; if (!rule) return NULL; memcpy(&magic,rule->bytes,8);
    return magic==DM_TZ_MAGIC ? zone_named((const char*)rule->bytes+8) : NULL;
}
static CFCalendarRef zone_calendar(CFTimeZoneRef zone) {
    CFCalendarRef c=CFCalendarCreateWithIdentifier(NULL,kCFGregorianCalendar);
    if (c) { CFCalendarSetTimeZone(c,zone); CFCalendarSetFirstWeekday(c,1); }
    return c;
}
static int calendar_read(CFCalendarRef c, CFAbsoluteTime at, CalendarTime *out) {
    int year,month,day,hour,minute,second;
    if (!CFCalendarDecomposeAbsoluteTime(c,at,"yMdHms",&year,&month,&day,&hour,&minute,&second) || year<1 || year>32767) return 0;
    *out=(CalendarTime){year,month,day,hour,minute,second,0}; return 1;
}
static Result zone_calendar_time(CalendarTime *out, CalendarAdditionalInfo *info, const int64_t *time, CFTimeZoneRef zone) {
    if (!time || !zone) return RES_FAIL;
    CFCalendarRef c=zone_calendar(zone); if (!c) return RES_FAIL;
    CFAbsoluteTime at=(double)*time-kCFAbsoluteTimeIntervalSince1970; CalendarTime value;
    if (!calendar_read(c,at,&value)) { CFRelease(c); return RES_FAIL; }
    CalendarAdditionalInfo extra={0};
    extra.wday=(uint32_t)CFCalendarGetOrdinalityOfUnit(c,kCFCalendarUnitDay,kCFCalendarUnitWeek,at)-1;
    extra.yday=(uint32_t)CFCalendarGetOrdinalityOfUnit(c,kCFCalendarUnitDay,kCFCalendarUnitYear,at)-1;
    extra.dst=CFTimeZoneIsDaylightSavingTime(zone,at); extra.offset=(int32_t)CFTimeZoneGetSecondsFromGMT(zone,at);
    CFStringRef abbreviation=CFTimeZoneCopyAbbreviation(zone,at);
    if (abbreviation) { CFStringGetCString(abbreviation,extra.tzname,sizeof extra.tzname,kCFStringEncodingUTF8); CFRelease(abbreviation); }
    if (out) *out=value; if (info) *info=extra; CFRelease(c); return RES_OK;
}
HLE_FORCE("_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE", "设备时区：读取 Apple 系统 IANA 名称，不连接 Switch 时区服务", void, h_time_DeviceZone, TimeLocationName *out) {
    CFTimeZoneResetSystem(); CFTimeZoneRef z=CFTimeZoneCopySystem(); TimeLocationName name={0};
    if (!z || !CFStringGetCString(CFTimeZoneGetName(z),name.name,sizeof name.name,kCFStringEncodingUTF8)) {
        if (z) CFRelease(z); die("【红】无法读取宿主系统时区名称");
    }
    CFRelease(z); *out=name;
}
HLE_FORCE("_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE", "时区规则：保存可复制名称，转换使用 Apple 时区数据库", Result, h_time_LoadZone, TimeZoneRule *out, const TimeLocationName *name) {
    if (!out || !name) return RES_FAIL;
    CFTimeZoneRef z=zone_named(name->name); if (!z) return RES_FAIL;
    CFRelease(z); memset(out,0,sizeof *out); uint64_t magic=DM_TZ_MAGIC;
    memcpy(out->bytes,&magic,8); memcpy(out->bytes+8,name->name,sizeof name->name); return RES_OK;
}
HLE_FORCE("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE", "指定规则的日历换算：使用规则对应时区，保留夏令时与 UTC 偏移", Result, h_time_CalendarRule, CalendarTime *out, CalendarAdditionalInfo *info, const int64_t *time, const TimeZoneRule *rule) {
    CFTimeZoneRef z=zone_rule(rule); if (!z) return RES_FAIL;
    Result r=zone_calendar_time(out,info,time,z); CFRelease(z); return r;
}
static Result zone_posix(int *count, int64_t *out, int capacity, const CalendarTime *date, CFTimeZoneRef zone) {
    if (!count || capacity<0 || (capacity && !out) || !date || !zone) return RES_FAIL;
    CFTimeZoneRef utc=CFTimeZoneCreateWithTimeIntervalFromGMT(NULL,0); CFCalendarRef c=zone_calendar(utc); CFRelease(utc);
    if (!c) return RES_FAIL;
    CFAbsoluteTime naive; CalendarTime checked;
    int valid=CFCalendarComposeAbsoluteTime(c,&naive,"yMdHms",(int)date->year,(int)date->month,(int)date->day,(int)date->hour,(int)date->minute,(int)date->second)
        && calendar_read(c,naive,&checked) && !memcmp(&checked,date,7);
    CFRelease(c); if (!valid) return RES_FAIL;
    c=zone_calendar(zone); if (!c) return RES_FAIL;
    int n=0; int64_t values[3];
    // 前后两日的偏移候选经完整日历往返校验：跳过时刻给 0 个，回拨重复时刻给 2 个。
    // Validate nearby offset candidates by a full calendar round trip: skipped times yield zero, repeated times yield two.
    for (int day=-2;day<=2;day+=2) {
        double candidate=naive-CFTimeZoneGetSecondsFromGMT(zone,naive+day*86400.0);
        CalendarTime roundtrip; if (!calendar_read(c,candidate,&roundtrip) || memcmp(&roundtrip,date,7)) continue;
        int64_t posix=(int64_t)llround(candidate+kCFAbsoluteTimeIntervalSince1970); int duplicate=0;
        for(int i=0;i<n;i++) duplicate|=values[i]==posix;
        if(!duplicate) values[n++]=posix;
    }
    CFRelease(c);
    for(int i=0;i<n;i++) for(int j=i+1;j<n;j++) if(values[j]<values[i]) {int64_t t=values[i];values[i]=values[j];values[j]=t;}
    if(n>2) return RES_FAIL;
    int written=n<capacity?n:capacity; for(int i=0;i<written;i++) out[i]=values[i]; *count=written; return RES_OK;
}
HLE_FORCE("_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeERKNS0_12TimeZoneRuleE", "指定规则的反向日历换算：处理夏令时重复/缺失时刻，只写容量内结果", Result, h_time_PosixRule, int *count, int64_t *out, int capacity, const CalendarTime *date, const TimeZoneRule *rule) {
    CFTimeZoneRef z=zone_rule(rule); if(!z) return RES_FAIL;
    Result r=zone_posix(count,out,capacity,date,z); CFRelease(z); return r;
}
