/* settings.ini skip_intro: from start to the title screen ("Press ANY button") without waiting.
 * The engine takes no switches for it, so the runner plays the start as a hurried player would:
 *
 *   1. The KOEI TECMO and TECMO logos (about 6 s) run on frames: the game advances its logic once
 *      per flip and flips every second vblank. The vblank runs at FAST_HZ until the next screen
 *      loads, which shows the logos in about 1.5 s.
 *   2. The autosave notice ("This game saves data automatically...") waits for Cross: once its
 *      window texture is read, the vblank goes back to normal and Cross is tapped (100 ms of every
 *      400 ms).
 *   3. The title screen's texture being read ends the taps, before the title takes input.
 *
 * The screens are recognised by the archive entries the game reads: COMMON_WINDOW_G1T (the
 * notice's window) and TITLEMENU_G1T in FILE_PS4/archive00.lnk, found by name in the dump's
 * archive00.h (an enum of the archive's entries, in order) and placed with the archive's table of
 * {offset, size, packed size, flags} records after its 32-byte header. If 20 s pass without the
 * title, the runner stops helping and the game goes on as usual. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include "gpu/deception_gpu.h"

enum { FAST_HZ=600, GIVE_UP_US=20000000 };
enum { PHASE_OFF, PHASE_LOGOS, PHASE_NOTICE, PHASE_DONE };
static _Atomic int phase=PHASE_OFF;
static _Atomic uint64_t phase_since;
static uint64_t window_offset=UINT64_MAX, title_offset=UINT64_MAX;

static uint64_t now_us(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000+(uint64_t)t.tv_nsec/1000;
}
/* The index of INDEX_K300_<name> in archive00.h, or -1. */
static long entry_index(const char *header, const char *name) {
    FILE *f=fopen(header,"rb");
    if (!f) return -1;
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
    return found;
}
static uint64_t entry_offset(FILE *archive, long index) {
    uint64_t record[4];
    if (index<0 || fseek(archive,32+index*32,SEEK_SET) || fread(record,8,4,archive)!=4) return UINT64_MAX;
    return record[0];
}
static void finish(const char *why) {
    if (atomic_exchange(&phase,PHASE_DONE)==PHASE_DONE) return;
    deception_set_vblank_boost(0);
    printf("Intro: %s\n",why);
}

void runtime_intro_start(const char *app0) {
    if (!deception_skip_intro() || !app0) return;
    char header[1024], path[1024];
    snprintf(header,sizeof(header),"%s/FILE_PS4/archive00.h",app0);
    snprintf(path,sizeof(path),"%s/FILE_PS4/archive00.lnk",app0);
    FILE *archive=fopen(path,"rb");
    if (archive) {
        window_offset=entry_offset(archive,entry_index(header,"COMMON_WINDOW_G1T"));
        title_offset=entry_offset(archive,entry_index(header,"TITLEMENU_G1T"));
        fclose(archive);
    }
    if (window_offset==UINT64_MAX || title_offset==UINT64_MAX) {
        puts("Intro: the title screen's data is not where this game version has it; the intro plays");
        return;
    }
    atomic_store(&phase_since,now_us());
    atomic_store(&phase,PHASE_LOGOS);
    deception_set_vblank_boost(FAST_HZ);
    puts("Intro: skipping to the title screen (settings.ini skip_intro=1)");
}

/* runtime_file.c: the game read [offset, offset+size) of /app0/FILE_PS4/archive00.lnk. */
void runtime_intro_archive_read(uint64_t offset, uint64_t size) {
    const int p=atomic_load(&phase);
    if (p!=PHASE_LOGOS && p!=PHASE_NOTICE) return;
    if (title_offset>=offset && title_offset<offset+size) { finish("title screen reached"); return; }
    if (p==PHASE_LOGOS && window_offset>=offset && window_offset<offset+size) {
        deception_set_vblank_boost(0);
        atomic_store(&phase_since,now_us());
        atomic_store(&phase,PHASE_NOTICE);
        puts("Intro: logos done; confirming the autosave notice");
    }
}

/* runtime_pad.c, at each pad read: 1 while Cross should be held for the notice. */
int runtime_intro_cross(void) {
    const int p=atomic_load(&phase);
    if (p!=PHASE_LOGOS && p!=PHASE_NOTICE) return 0;
    const uint64_t t=now_us()-atomic_load(&phase_since);
    if (t>GIVE_UP_US) { finish("no title screen after 20 s; the game goes on as usual"); return 0; }
    return p==PHASE_NOTICE && t>=300000 && (t-300000)%400000<100000;
}
