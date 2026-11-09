/* settings.ini skip_intro: from start to the main menu, or to the title screen ("Press ANY button")
 * when there is no save to continue. The engine takes no switches for it, so the runner plays the
 * start as a hurried player would:
 *
 *   1. The KOEI TECMO and TECMO logos run on frames: the game advances its logic once per flip
 *      and flips every second vblank, so the vblank runs at FAST_HZ (about 1.5 s for the logos).
 *   2. The autosave notice ("This game saves data automatically...") waits for Cross: still fast,
 *      Cross is tapped until the title screen loads.
 *   3. With a save to continue, still fast, Cross is tapped on "Press ANY button" and then on the
 *      title menu, whose cursor starts on Continue, until the main menu loads; the vblank then
 *      goes back to normal. Without a save the game rests on the title screen (Cross there would
 *      start a new game) at normal speed.
 *
 * The screens are recognised by the archive entries the game reads: COMMON_WINDOW_G1T (the
 * notice's window) and TITLEMENU_G1T in FILE_PS4/archive00.lnk, CHAPTER_SELECT_G1T (main menu) in
 * FILE_PS4/ARCHIVE/archive001.lnk. Each is found by name in its archive's .h (an enum of the
 * entries, in order) and placed with the archive's table of {offset, size, packed size, flags}
 * records after its 32-byte header. If GIVE_UP_US pass in a phase, the runner stops helping and
 * the game goes on as usual. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include "gpu/deception_gpu.h"

enum { FAST_HZ=600, GIVE_UP_US=20000000 };
enum { PHASE_OFF, PHASE_LOGOS, PHASE_NOTICE, PHASE_TITLE, PHASE_DONE };
static _Atomic int phase=PHASE_OFF;
static _Atomic uint64_t phase_since;
static uint64_t window_offset=UINT64_MAX, title_offset=UINT64_MAX, menu_offset=UINT64_MAX;
static int continue_save;

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
/* A progress save the title menu's Continue loads: savedata/<user>/<title>/game-auto. */
static int has_save(void) {
    const char *user=runtime_file_user_dir();
    char path[1024];
    snprintf(path,sizeof(path),"%s/savedata",user ? user : "");
    DIR *users=opendir(path);
    if (!users) return 0;
    int found=0;
    for (struct dirent *u; !found && (u=readdir(users));) {
        if (u->d_name[0]=='.') continue;
        char titles_path[1100];
        snprintf(titles_path,sizeof(titles_path),"%s/%s",path,u->d_name);
        DIR *titles=opendir(titles_path);
        for (struct dirent *t; titles && !found && (t=readdir(titles));) {
            if (t->d_name[0]=='.') continue;
            char save[1400]; struct stat st;
            snprintf(save,sizeof(save),"%s/%s/game-auto",titles_path,t->d_name);
            found=!stat(save,&st) && S_ISDIR(st.st_mode);
        }
        if (titles) closedir(titles);
    }
    closedir(users);
    return found;
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
    menu_offset=entry_offset("/app0/FILE_PS4/ARCHIVE/archive001.lnk","/app0/FILE_PS4/ARCHIVE/archive001.h","CHAPTER_SELECT_G1T");
    if (window_offset==UINT64_MAX || title_offset==UINT64_MAX) {
        puts("Intro: the title screen's data is not where this game version has it; the intro plays");
        return;
    }
    continue_save=menu_offset!=UINT64_MAX && has_save();
    deception_set_vblank_boost(FAST_HZ);
    enter(PHASE_LOGOS,continue_save ? "skipping to the main menu (settings.ini skip_intro=1)"
                                    : "skipping to the title screen (settings.ini skip_intro=1; no save to continue)");
}

static int reads(uint64_t entry, uint64_t offset, uint64_t size) { return entry>=offset && entry<offset+size; }

/* runtime_file.c: the game read [offset, offset+size) of the file at guest_path. */
void runtime_intro_archive_read(const char *guest_path, uint64_t offset, uint64_t size) {
    const int p=atomic_load(&phase);
    if (p==PHASE_OFF || p==PHASE_DONE) return;
    if (!strcmp(guest_path,"/app0/FILE_PS4/archive00.lnk")) {
        if (p==PHASE_LOGOS && reads(window_offset,offset,size)) enter(PHASE_NOTICE,"logos done; confirming the autosave notice");
        else if ((p==PHASE_LOGOS || p==PHASE_NOTICE) && reads(title_offset,offset,size)) {
            if (!continue_save) { finish("title screen reached"); return; }
            enter(PHASE_TITLE,"title screen reached; continuing to the main menu");
        }
    } else if (p==PHASE_TITLE && !strcmp(guest_path,"/app0/FILE_PS4/ARCHIVE/archive001.lnk") && reads(menu_offset,offset,size)) {
        finish("main menu reached");
    }
}

/* runtime_pad.c, at each pad read: 1 while Cross should be held. */
int runtime_intro_cross(void) {
    const int p=atomic_load(&phase);
    if (p==PHASE_OFF || p==PHASE_DONE || p==PHASE_LOGOS) return 0;
    const uint64_t t=now_us()-atomic_load(&phase_since);
    if (t>GIVE_UP_US) { finish("the screen did not change after 20 s; the game goes on as usual"); return 0; }
    /* Short taps on the notice. On the title a tap every 400 ms: Continue reads the main menu's
     * data at once, which ends the taps before the next one could reach the main menu. */
    if (p==PHASE_NOTICE) return t>=50000 && (t-50000)%150000<50000;
    return t>=300000 && (t-300000)%400000<100000;
}
