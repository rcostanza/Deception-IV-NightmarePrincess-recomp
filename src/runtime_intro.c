/* settings.ini skip_intro: from start to the title menu (NEW GAME / CONTINUE), where the player
 * takes over. The engine takes no switches for it, so the runner plays the start as a hurried
 * player would:
 *
 *   1. The KOEI TECMO and TECMO logos run on frames: the game advances its logic once per flip
 *      and flips every second vblank, so the vblank runs at FAST_HZ (about 1.5 s for the logos).
 *   2. The autosave notice ("This game saves data automatically...") waits for Cross: still fast,
 *      Cross is tapped until the title screen loads.
 *   3. "Press ANY button" gets one Cross, TITLE_TAP_US after its data loads, and the vblank goes
 *      back to normal: the title menu opens with nothing chosen.
 *
 * The screens are recognised by the archive entries the game reads: COMMON_WINDOW_G1T (the
 * notice's window) and TITLEMENU_G1T in FILE_PS4/archive00.lnk. Each is found by name in the
 * archive's .h (an enum of the entries, in order) and placed with the archive's table of {offset,
 * size, packed size, flags} records after its 32-byte header. If GIVE_UP_US pass in a phase, the
 * runner stops helping and the game goes on as usual. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include "gpu/deception_gpu.h"

/* "Press ANY button" ignores input while its screen builds up: a tap 1.5 s after its data loads
 * was lost, 2.5 s was taken. One tap only, so a late one cannot also choose a menu entry. */
enum { FAST_HZ=600, GIVE_UP_US=20000000, TITLE_TAP_US=3000000, TAP_US=100000 };
enum { PHASE_OFF, PHASE_LOGOS, PHASE_NOTICE, PHASE_TITLE, PHASE_DONE };
static _Atomic int phase=PHASE_OFF;
static _Atomic uint64_t phase_since;
static uint64_t window_offset=UINT64_MAX, title_offset=UINT64_MAX;

static uint64_t now_us(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000+(uint64_t)t.tv_nsec/1000;
}
/* The offset of entry INDEX_K300_<name> of the archive at guest path `lnk` (its enum in `header`),
 * or UINT64_MAX. */
static uint64_t entry_offset(const char *lnk, const char *header, const char *name) {
    char host[1024];
    if (runtime_file_host_path(header,host,sizeof(host))) return UINT64_MAX;
    FILE *f=fopen(host,"rb");
    if (!f) return UINT64_MAX;
    char line[256], want[96];
    snprintf(want,sizeof(want),"INDEX_K300_%s,",name);
    long index=0, found=-1;
    while (found<0 && fgets(line,sizeof(line),f)) {
        const char *p=strstr(line,"INDEX_K300_");
        if (!p) continue;
        if (!strncmp(p,want,strlen(want))) found=index;
        ++index;
    }
    fclose(f);
    if (found<0 || runtime_file_host_path(lnk,host,sizeof(host)) || !(f=fopen(host,"rb"))) return UINT64_MAX;
    uint64_t record[4];
    const int ok=!fseek(f,32+found*32,SEEK_SET) && fread(record,8,4,f)==4;
    fclose(f);
    return ok ? record[0] : UINT64_MAX;
}
static void enter(int p, const char *why) {
    atomic_store(&phase_since,now_us());
    atomic_store(&phase,p);
    if (why) printf("Intro: %s\n",why);
}
static void finish(const char *why) {
    if (atomic_exchange(&phase,PHASE_DONE)==PHASE_DONE) return;
    deception_set_vblank_boost(0);
    printf("Intro: %s\n",why);
}

void runtime_intro_start(const char *app0) {
    if (!deception_skip_intro() || !app0) return;
    window_offset=entry_offset("/app0/FILE_PS4/archive00.lnk","/app0/FILE_PS4/archive00.h","COMMON_WINDOW_G1T");
    title_offset=entry_offset("/app0/FILE_PS4/archive00.lnk","/app0/FILE_PS4/archive00.h","TITLEMENU_G1T");
    if (window_offset==UINT64_MAX || title_offset==UINT64_MAX) {
        puts("Intro: the title screen's data is not where this game version has it; the intro plays");
        return;
    }
    deception_set_vblank_boost(FAST_HZ);
    enter(PHASE_LOGOS,"skipping to the title menu (settings.ini skip_intro=1)");
}

static int reads(uint64_t entry, uint64_t offset, uint64_t size) { return entry>=offset && entry<offset+size; }

/* runtime_file.c: the game read [offset, offset+size) of the file at guest_path. */
void runtime_intro_archive_read(const char *guest_path, uint64_t offset, uint64_t size) {
    const int p=atomic_load(&phase);
    if ((p!=PHASE_LOGOS && p!=PHASE_NOTICE) || strcmp(guest_path,"/app0/FILE_PS4/archive00.lnk")) return;
    if (p==PHASE_LOGOS && reads(window_offset,offset,size)) enter(PHASE_NOTICE,"logos done; confirming the autosave notice");
    else if (reads(title_offset,offset,size)) enter(PHASE_TITLE,"title screen reached; opening the title menu");
}

/* runtime_pad.c, at each pad read: 1 while Cross should be held. */
int runtime_intro_cross(void) {
    const int p=atomic_load(&phase);
    if (p==PHASE_OFF || p==PHASE_DONE || p==PHASE_LOGOS) return 0;
    const uint64_t t=now_us()-atomic_load(&phase_since);
    if (p==PHASE_NOTICE) {
        if (t>GIVE_UP_US) { finish("the screen did not change after 20 s; the game goes on as usual"); return 0; }
        return t>=50000 && (t-50000)%150000<50000; /* short taps */
    }
    if (t<TITLE_TAP_US) return 0; /* one Cross on "Press ANY button", then the player's turn */
    if (t<TITLE_TAP_US+TAP_US) return 1;
    finish("title menu reached");
    return 0;
}
