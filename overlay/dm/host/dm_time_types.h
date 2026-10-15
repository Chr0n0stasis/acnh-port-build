// 公开时间 ABI；大小断言保证宿主只写游戏提供的结构范围。
// Public time ABI; size assertions keep host writes inside guest structures.
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct { int16_t year; int8_t month, day, hour, minute, second, pad; } CalendarTime;
typedef struct { uint32_t wday, yday; char tzname[8]; uint32_t dst; int32_t offset; } CalendarAdditionalInfo;
typedef struct { char name[36]; } TimeLocationName;
typedef struct { uint8_t bytes[0x4000]; } TimeZoneRule;

typedef struct {
    int64_t time_point;
    uint8_t raw[16];
} SteadyClockTimePoint;

typedef struct {
    int64_t offset;
    SteadyClockTimePoint timestamp;
} SystemClockContext;

// SDK 快照内部使用单字节星期；公开的 CalendarAdditionalInfo 使用 32 位星期。
// The SDK snapshot stores a byte weekday; public CalendarAdditionalInfo uses a 32-bit weekday.
typedef struct {
    int8_t wday;
    uint8_t pad[3];
    uint32_t yday;
    char tzname[8];
    uint32_t dst;
    int32_t offset;
} SnapshotCalendarAdditionalInfo;

typedef struct {
    SystemClockContext user_clock_ctx;                           // 0x00 .. 0x20
    SystemClockContext network_clock_ctx;                        // 0x20 .. 0x40
    int64_t user_clock_posix_time;                               // 0x40 .. 0x48
    int64_t network_clock_posix_time;                            // 0x48 .. 0x50
    CalendarTime user_clock_calendar_time;                       // 0x50 .. 0x58
    CalendarTime network_clock_calendar_time;                    // 0x58 .. 0x60
    SnapshotCalendarAdditionalInfo user_clock_calendar_additional_info; // 0x60 .. 0x78
    SnapshotCalendarAdditionalInfo network_clock_calendar_additional_info; // 0x78 .. 0x90
    SteadyClockTimePoint steady_clock_time_point;                // 0x90 .. 0xa8
    TimeLocationName location_name;                              // 0xa8 .. 0xcc
    uint8_t is_automatic_correction_enabled;                     // 0xcc
    uint8_t initial_type;                                        // 0xcd
    uint8_t pad[2];                                              // 0xce .. 0xd0
} ClockSnapshot;

_Static_assert(sizeof(CalendarTime)==8, "CalendarTime ABI");
_Static_assert(sizeof(CalendarAdditionalInfo)==24, "CalendarAdditionalInfo ABI");
_Static_assert(sizeof(TimeLocationName)==36 && sizeof(TimeZoneRule)==0x4000, "Time zone ABI");
_Static_assert(sizeof(SystemClockContext) == 32, "SystemClockContext ABI");
_Static_assert(sizeof(SteadyClockTimePoint) == 24, "SteadyClockTimePoint ABI");
_Static_assert(sizeof(ClockSnapshot) == 0xd0, "ClockSnapshot ABI");
_Static_assert(offsetof(SystemClockContext, timestamp) == 8, "SystemClockContext timestamp ABI");
_Static_assert(offsetof(ClockSnapshot, network_clock_calendar_time) == 0x58, "SDK network date ABI");
_Static_assert(offsetof(ClockSnapshot, user_clock_calendar_additional_info) == 0x60, "SDK user calendar info ABI");
_Static_assert(offsetof(ClockSnapshot, network_clock_calendar_additional_info) == 0x78, "SDK network calendar info ABI");
_Static_assert(offsetof(ClockSnapshot, steady_clock_time_point) == 0x90, "SDK steady time ABI");
_Static_assert(offsetof(ClockSnapshot, location_name) == 0xa8, "SDK location ABI");
_Static_assert(offsetof(ClockSnapshot, initial_type) == 0xcd, "SDK snapshot type ABI");

void dm_time_device_zone(TimeLocationName *out);
int dm_time_snapshot_calendar(CalendarTime *date, SnapshotCalendarAdditionalInfo *info,
                              TimeLocationName *location, int64_t posix);
