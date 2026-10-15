// 公开时间 ABI；大小断言保证宿主只写游戏提供的结构范围。
// Public time ABI; size assertions keep host writes inside guest structures.
#pragma once
#include <stdint.h>
typedef struct { int16_t year; int8_t month, day, hour, minute, second, pad; } CalendarTime;
typedef struct { uint32_t wday, yday; char tzname[8]; uint32_t dst; int32_t offset; } CalendarAdditionalInfo;
typedef struct { char name[36]; } TimeLocationName;
typedef struct { uint8_t bytes[0x4000]; } TimeZoneRule;
_Static_assert(sizeof(CalendarTime)==8, "CalendarTime ABI");
_Static_assert(sizeof(CalendarAdditionalInfo)==24, "CalendarAdditionalInfo ABI");
_Static_assert(sizeof(TimeLocationName)==36 && sizeof(TimeZoneRule)==0x4000, "Time zone ABI");
