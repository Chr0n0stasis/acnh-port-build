// 公开时间 ABI；大小断言保证宿主只写游戏提供的结构范围。
// Public time ABI; size assertions keep host writes inside guest structures.
#pragma once
#include <stdint.h>
typedef struct { int16_t year; int8_t month, day, hour, minute, second, pad; } CalendarTime;
typedef struct { uint32_t wday, yday; char tzname[8]; uint32_t dst; int32_t offset; } CalendarAdditionalInfo;
typedef struct { char name[36]; } TimeLocationName;
typedef struct { uint8_t bytes[0x4000]; } TimeZoneRule;

typedef struct {
    uint8_t raw[16];
    int64_t offset;
    uint8_t pad[8];
} SystemClockContext;

typedef struct {
    int64_t time_point;
    uint8_t raw[16];
} SteadyClockTimePoint;

typedef struct {
    SystemClockContext user_clock_ctx;                           // 0x00 .. 0x20
    SystemClockContext network_clock_ctx;                        // 0x20 .. 0x40
    int64_t user_clock_posix_time;                               // 0x40 .. 0x48
    int64_t network_clock_posix_time;                            // 0x48 .. 0x50
    CalendarTime user_clock_calendar_time;                       // 0x50 .. 0x58
    CalendarAdditionalInfo user_clock_calendar_additional_info; // 0x58 .. 0x70
    CalendarTime network_clock_calendar_time;                    // 0x70 .. 0x78
    CalendarAdditionalInfo network_clock_calendar_additional_info; // 0x78 .. 0x90
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
