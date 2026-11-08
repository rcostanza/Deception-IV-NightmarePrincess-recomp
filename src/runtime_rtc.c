/* libSceRtc: calendar time. A tick is one microsecond since 0001-01-01 00:00
 * (proleptic Gregorian), the PS4 convention; the host clock supplies "now"
 * and the host time zone supplies local time. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define ERR_INVALID_POINTER ((int32_t)0x80B50002)
#define ERR_INVALID_YEAR ((int32_t)0x80B50008)
#define ERR_INVALID_MONTH ((int32_t)0x80B50009)
#define ERR_INVALID_DAY ((int32_t)0x80B5000A)
#define ERR_INVALID_HOUR ((int32_t)0x80B5000B)
#define ERR_INVALID_MINUTE ((int32_t)0x80B5000C)
#define ERR_INVALID_SECOND ((int32_t)0x80B5000D)
#define ERR_INVALID_MICROSECOND ((int32_t)0x80B5000E)
#define UNIX_EPOCH_TICKS UINT64_C(0xdcbffeff2bc000)
#define TICKS_PER_DAY UINT64_C(86400000000)

typedef struct { uint16_t year, month, day, hour, minute, second; uint32_t microsecond; } DateTime;
_Static_assert(sizeof(DateTime)==16,"OrbisRtcDateTime layout");

static int leap(int y) { return (y%4==0 && y%100!=0) || y%400==0; }
static int days_in_month(int y, int m) {
    static const int d[12]={31,28,31,30,31,30,31,31,30,31,30,31};
    return m==2 && leap(y) ? 29 : d[m-1];
}
/* Days since 0001-01-01 (H. Hinnant's days_from_civil, shifted epoch). */
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m<=2;
    int64_t era=(y>=0 ? y : y-399)/400;
    int64_t yoe=y-era*400, doy=(153*(m+(m>2 ? -3 : 9))+2)/5+d-1, doe=yoe*365+yoe/4-yoe/100+doy;
    return era*146097+doe-306; /* 0001-01-01 == 0 */
}
static void civil_from_days(int64_t z, int *y, int *m, int *d) {
    z+=306;
    int64_t era=(z>=0 ? z : z-146096)/146097, doe=z-era*146097;
    int64_t yoe=(doe-doe/1460+doe/36524-doe/146096)/365, doy=doe-(365*yoe+yoe/4-yoe/100);
    int64_t mp=(5*doy+2)/153;
    *d=(int)(doy-(153*mp+2)/5+1); *m=(int)(mp<10 ? mp+3 : mp-9); *y=(int)(yoe+era*400+(*m<=2));
}
static int32_t validate(const DateTime *t) {
    if (t->year<1 || t->year>9999) return ERR_INVALID_YEAR;
    if (t->month<1 || t->month>12) return ERR_INVALID_MONTH;
    if (t->day<1 || t->day>days_in_month(t->year,t->month)) return ERR_INVALID_DAY;
    if (t->hour>23) return ERR_INVALID_HOUR;
    if (t->minute>59) return ERR_INVALID_MINUTE;
    if (t->second>59) return ERR_INVALID_SECOND;
    if (t->microsecond>999999) return ERR_INVALID_MICROSECOND;
    return 0;
}
static uint64_t to_tick(const DateTime *t) {
    return (uint64_t)days_from_civil(t->year,t->month,t->day)*TICKS_PER_DAY+
           ((uint64_t)t->hour*3600+(uint64_t)t->minute*60+t->second)*1000000+t->microsecond;
}
static void from_tick(uint64_t tick, DateTime *t) {
    int y,m,d;
    civil_from_days((int64_t)(tick/TICKS_PER_DAY),&y,&m,&d);
    uint64_t us=tick%TICKS_PER_DAY;
    t->year=(uint16_t)y; t->month=(uint16_t)m; t->day=(uint16_t)d;
    t->hour=(uint16_t)(us/3600000000u); t->minute=(uint16_t)(us/60000000u%60); t->second=(uint16_t)(us/1000000u%60);
    t->microsecond=(uint32_t)(us%1000000u);
}
static uint64_t now_utc(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts);
    return UNIX_EPOCH_TICKS+(uint64_t)ts.tv_sec*1000000u+(uint64_t)ts.tv_nsec/1000u;
}

static ABI int32_t rtc_current_tick(uint64_t *tick) { if (!tick) return ERR_INVALID_POINTER; *tick=now_utc(); return 0; }
static ABI int32_t rtc_get_tick(const DateTime *t, uint64_t *tick) {
    if (!t || !tick) return ERR_INVALID_POINTER;
    int32_t e=validate(t);
    if (e) return e;
    *tick=to_tick(t); return 0;
}
static ABI int32_t rtc_set_tick(DateTime *t, const uint64_t *tick) {
    if (!t || !tick) return ERR_INVALID_POINTER;
    from_tick(*tick,t); return 0;
}
static ABI uint32_t rtc_tick_resolution(void) { return 1000000; }
/* sceRtcTickAddHours(out, in, hours) / sceRtcTickAddSeconds(out, in, seconds): in plus that many
 * microseconds (wrapping as the unsigned tick does). */
static ABI int32_t rtc_add_hours(uint64_t *out, const uint64_t *in, int32_t hours) {
    if (!out || !in) return ERR_INVALID_POINTER;
    *out=*in+(uint64_t)((int64_t)hours*3600000000LL); return 0;
}
static ABI int32_t rtc_add_seconds(uint64_t *out, const uint64_t *in, int64_t seconds) {
    if (!out || !in) return ERR_INVALID_POINTER;
    *out=*in+(uint64_t)(seconds*1000000LL); return 0;
}
static ABI int32_t rtc_get_time_t(const DateTime *t, int64_t *seconds) {
    if (!t || !seconds) return ERR_INVALID_POINTER;
    int32_t e=validate(t);
    if (e) return e;
    uint64_t tick=to_tick(t);
    *seconds=tick<UNIX_EPOCH_TICKS ? 0 : (int64_t)((tick-UNIX_EPOCH_TICKS)/1000000); return 0;
}
/* sceRtcSetTime_t(t, seconds): the date and time of Unix time `seconds` (UTC). */
static ABI int32_t rtc_set_time_t(DateTime *t, int64_t seconds) {
    if (!t) return ERR_INVALID_POINTER;
    if (seconds<0) return ERR_INVALID_YEAR;
    from_tick(UNIX_EPOCH_TICKS+(uint64_t)seconds*1000000u,t); return 0;
}
/* sceRtcGetCurrentClockLocalTime(t): now, in the host's time zone. */
static ABI int32_t rtc_current_clock_local(DateTime *t) {
    if (!t) return ERR_INVALID_POINTER;
    struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts);
    time_t now=ts.tv_sec; struct tm local;
#ifdef _WIN32
    localtime_s(&local,&now);
#else
    localtime_r(&now,&local);
#endif
    t->year=(uint16_t)(local.tm_year+1900); t->month=(uint16_t)(local.tm_mon+1); t->day=(uint16_t)local.tm_mday;
    t->hour=(uint16_t)local.tm_hour; t->minute=(uint16_t)local.tm_min; t->second=(uint16_t)(local.tm_sec>59 ? 59 : local.tm_sec);
    t->microsecond=(uint32_t)(ts.tv_nsec/1000);
    return 0;
}
/* sceRtcGetDayOfWeek(year, month, day): 0 Sunday .. 6 Saturday, or an SCE error. */
static ABI int32_t rtc_day_of_week(int32_t year, int32_t month, int32_t day) {
    if (year<1 || year>9999) return ERR_INVALID_YEAR;
    if (month<1 || month>12) return ERR_INVALID_MONTH;
    if (day<1 || day>days_in_month(year,month)) return ERR_INVALID_DAY;
    return (int32_t)((days_from_civil(year,month,day)+1)%7); /* 0001-01-01 was a Monday */
}

/* sceRtcParseDateTime(tick, text): the UTC tick of an RFC 3339 ("2024-01-02T03:04:05[.frac]
 * (Z|+hh:mm|-hh:mm)"), RFC 2822 ("Tue, 02 Jan 2024 03:04:05 +hhmm") or asctime ("Tue Jan  2
 * 03:04:05 2024") time; a zone offset is subtracted to reach UTC (shadPS4 rtc.cpp adds it).
 * Text in none of these forms is BAD_PARSE; an impossible date fails as sceRtcGetTick. */
#define ERR_BAD_PARSE ((int32_t)0x80B50007)
static int month_of(const char *name) {
    static const char *months[]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    for (int i=0;i<12;++i) if (!strncmp(name,months[i],3)) return i+1;
    return 0;
}
/* Minutes east of UTC from "Z", "GMT", "UT", "+hh:mm" or "+hhmm" (and minus); -1 on bad text. */
static int zone_minutes(const char *z, int *minutes) {
    while (*z==' ') ++z;
    if (!*z || !strcmp(z,"Z") || !strcmp(z,"z") || !strcmp(z,"GMT") || !strcmp(z,"UT")) { *minutes=0; return 0; }
    int h, m, n=0;
    if ((*z!='+' && *z!='-') || (sscanf(z+1,"%2d:%2d%n",&h,&m,&n)!=2 && sscanf(z+1,"%2d%2d%n",&h,&m,&n)!=2) || z[1+n]) return -1;
    *minutes=(*z=='-' ? -1 : 1)*(h*60+m);
    return 0;
}
static ABI int32_t rtc_parse_date_time(uint64_t *tick, const char *text) {
    if (!tick || !text) return ERR_INVALID_POINTER;
    int y, mo, d, h, mi, sec, n=0, zone=0;
    char month[4]={0};
    uint32_t micro=0;
    if (sscanf(text,"%4d-%2d-%2d%*1[Tt ]%2d:%2d:%2d%n",&y,&mo,&d,&h,&mi,&sec,&n)==6 && n) {
        const char *p=text+n;
        if (*p=='.') {
            uint32_t scale=100000;
            for (++p;*p>='0' && *p<='9';++p) { micro+=(uint32_t)(*p-'0')*scale; scale/=10; }
        }
        if (zone_minutes(p,&zone)) return ERR_BAD_PARSE;
    } else if (sscanf(text,"%*3s, %d %3s %d %d:%d:%d%n",&d,month,&y,&h,&mi,&sec,&n)==6 && n) {
        if (!(mo=month_of(month)) || zone_minutes(text+n,&zone)) return ERR_BAD_PARSE;
    } else if (sscanf(text,"%*3s %3s %d %d:%d:%d %d%n",month,&d,&h,&mi,&sec,&y,&n)==6 && n && !text[n]) {
        if (!(mo=month_of(month))) return ERR_BAD_PARSE;
    } else return ERR_BAD_PARSE;
    DateTime t={(uint16_t)y,(uint16_t)mo,(uint16_t)d,(uint16_t)h,(uint16_t)mi,(uint16_t)sec,micro};
    if (y<1 || mo<1 || d<1 || h<0 || mi<0 || sec<0) return validate(&(DateTime){0});
    int32_t r=validate(&t);
    if (r) return r;
    *tick=to_tick(&t)-(uint64_t)((int64_t)zone*60000000);
    return 0;
}
static const RuntimeExport exports[]={
    {"sceRtcGetCurrentNetworkTick",rtc_current_tick}, {"sceRtcGetCurrentTick",rtc_current_tick},
    {"sceRtcSetTime_t",rtc_set_time_t}, {"sceRtcGetCurrentClockLocalTime",rtc_current_clock_local},
    {"sceRtcGetDayOfWeek",rtc_day_of_week},

    {"sceRtcGetTick",rtc_get_tick}, {"sceRtcSetTick",rtc_set_tick},
    {"sceRtcGetTickResolution",rtc_tick_resolution}, {"sceRtcTickAddHours",rtc_add_hours}, {"sceRtcTickAddSeconds",rtc_add_seconds},
    {"sceRtcGetTime_t",rtc_get_time_t}, {"sceRtcParseDateTime",rtc_parse_date_time},
};
uintptr_t runtime_rtc_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
