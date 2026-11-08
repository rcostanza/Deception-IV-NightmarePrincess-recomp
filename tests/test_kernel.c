/* Time-zone conversions and other libkernel extras (src/runtime_kernel.c), against the contracts
 * shadPS4 follows. */
#define _GNU_SOURCE
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#include "../src/runtime_kernel.c"
#include <assert.h>

/* The process time zone, as a POSIX TZ string. */
static void set_zone(const char *zone) {
#ifdef _WIN32
    _putenv_s("TZ",zone); _tzset();
#else
    setenv("TZ",zone,1); tzset();
#endif
}
int32_t *runtime_errno(void) { static int32_t value; return &value; }
void runtime_report(void) {}
void runtime_thread_count_yield(int zero_sleep) { (void)zero_sleep; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) { (void)table; (void)count; (void)name; return 0; }

int main(void) {
    /* Before anything loads the zone: the Windows CRT keeps the machine zone's summer time bias
     * when TZ changes later. */
    set_zone("EST5EDT");

    /* UTC <-> local: the two conversions are inverse; west_sec is the standard offset and dst_sec
     * the summer time in effect, so libc's mktime and localtime see summer time (US Eastern time,
     * where 2026-01-15 is winter and 2026-07-15 summer), and a zone without it. */
    const int64_t winter=1768478400, summer=1784116800; /* 12:00 UTC */
    int64_t local=0, back=0; GuestTimesec st; uint64_t dst=7; int32_t dst_seconds=-1;
    assert(kernel_utc_to_local(winter,NULL,NULL,NULL)==ERR(22));
    assert(kernel_local_to_utc(winter,0,&back,NULL,NULL)==ERR(22));
    assert(kernel_utc_to_local(winter,&local,&st,&dst)==0 && local==winter-5*3600 && st.t==winter);
    assert(st.west_sec==-5*3600 && st.dst_sec==0 && dst==0);
    assert(kernel_local_to_utc(local,0,&back,&st,&dst_seconds)==0 && back==winter && st.t==winter);
    assert(st.west_sec==-5*3600 && st.dst_sec==0 && dst_seconds==3600);
    assert(kernel_utc_to_local(summer,&local,&st,&dst)==0 && local==summer-4*3600);
    assert(st.west_sec==-5*3600 && st.dst_sec==3600 && dst==3600);
    assert(kernel_local_to_utc(local,1,&back,&st,&dst_seconds)==0 && back==summer);
    assert(st.west_sec==-5*3600 && st.dst_sec==3600 && dst_seconds==3600);
    /* 02:30 on 2026-03-08 does not exist there: any answer within the hour, but an answer. */
    const int64_t gap=1772937000;
    assert(kernel_local_to_utc(gap,0,&back,&st,&dst_seconds)==0 && llabs(back-(gap+5*3600))<=3600);
    set_zone("SGT-8");
    assert(kernel_utc_to_local(summer,&local,&st,&dst)==0 && local==summer+8*3600);
    assert(st.west_sec==8*3600 && st.dst_sec==0 && dst==0);
    assert(kernel_local_to_utc(local,0,&back,&st,&dst_seconds)==0 && back==summer && dst_seconds==0);

    /* libc's internal segment. */
    uint64_t info[2]={1,2};
    assert(internal_segment_info(info)==0 && !info[0] && !info[1]);
    assert(internal_segment_info(NULL)==ERR(14) && kernel_sleep(0)==0);

    puts("kernel tests passed");
    return 0;
}
