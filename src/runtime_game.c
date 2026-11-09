/* The game's version, logged first so a bug report's log tells which dump it came from: param.sfo's
 * TITLE_ID and APP_VER, and the eboot's SHA-256 that scripts/prepare.py records in analysis.json
 * beside the image. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "gpu/deception_gpu.h"

/* eboot.bin builds the runner has been run with. */
static const struct { const char *sha256, *version; } known_builds[]={
    {"",""},
};

/* "source_sha256" from analysis.json in the folder of `image_path`. */
static int eboot_sha256(const char *image_path, char *out) {
    char path[4096];
    const char *slash=strrchr(image_path,'/'), *back=strrchr(image_path,'\\');
    if (back>slash) slash=back;
    const int dir=slash ? (int)(slash-image_path)+1 : 0;
    snprintf(path,sizeof(path),"%.*sanalysis.json",dir,image_path);
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    char head[4096];
    const size_t n=fread(head,1,sizeof(head)-1,f);
    fclose(f);
    head[n]=0;
    const char *sha=strstr(head,"\"source_sha256\": \"");
    if (!sha || strspn(sha+18,"0123456789abcdef")<64) return 0;
    memcpy(out,sha+18,64); out[64]=0;
    return 1;
}

void runtime_game_identify(const char *title_id, const char *app_version, const char *app0, const char *image_path) {
    (void)app0;
    char sha[65]="";
    const char *build=NULL;
    if (eboot_sha256(image_path,sha))
        for (size_t i=0;i<sizeof(known_builds)/sizeof(*known_builds);++i)
            if (!strcmp(sha,known_builds[i].sha256)) build=known_builds[i].version;
    printf("Game: %s version %s, eboot.bin sha256 %s (%s%s)\n",title_id[0] ? title_id : "?",
           app_version[0] ? app_version : "unknown",sha[0] ? sha : "unknown",
           build ? "known build " : "a build the runner was not checked with",build ? build : "");
}


/* DECEPTION_TRACE_CALLS=<offset>[,<offset>...] (hex guest offsets of `call rel32` instructions):
 * each call goes through a stub that logs the callee's return value (rax). A development aid for
 * finding which step of a sequence fails. */
static ABI void trace_return(uint64_t site, uint64_t value) {
    printf("Trace: call at 0x%llx returned 0x%llx\n",(unsigned long long)site,(unsigned long long)value);
    fflush(stdout);
}
/* Writes `n` bytes into the loaded (read-only, executable) image. */
static void write_code(unsigned char *at, const void *bytes, size_t n) {
#ifdef _WIN32
    DWORD old;
    VirtualProtect(at,n,PAGE_EXECUTE_READWRITE,&old);
    memcpy(at,bytes,n);
    VirtualProtect(at,n,old,&old);
#else
    unsigned char *page=(unsigned char *)((uintptr_t)at&~(uintptr_t)4095);
    mprotect(page,(size_t)(at+n-page+4095)&~(size_t)4095,PROT_READ|PROT_WRITE|PROT_EXEC);
    memcpy(at,bytes,n);
    mprotect(page,(size_t)(at+n-page+4095)&~(size_t)4095,PROT_READ|PROT_EXEC);
#endif
}

/* DECEPTION_POKE=<offset>=<hex bytes>[,...]: writes bytes into the image (development). */
static void poke(unsigned char *image) {
    const char *list=getenv("DECEPTION_POKE");
    for (const char *s=list; s && *s;) {
        char *end;
        const uint64_t at=strtoull(s,&end,16);
        if (end==s || *end!='=') break;
        s=end+1;
        unsigned char bytes[64]; size_t n=0;
        while (n<sizeof(bytes) && isxdigit((unsigned char)s[0]) && isxdigit((unsigned char)s[1])) {
            char two[3]={s[0],s[1],0}; bytes[n++]=(unsigned char)strtoul(two,NULL,16); s+=2;
        }
        if (*s==',') ++s;
        write_code(image+at,bytes,n);
        printf("Poke: %zu bytes at 0x%llx\n",n,(unsigned long long)at);
    }
}

void runtime_game_trace_calls(unsigned char *image) {
    poke(image);
    const char *list=getenv("DECEPTION_TRACE_CALLS");
    if (!list || !*list) return;
    unsigned char *stubs=runtime_image_map(65536,PROT_READ|PROT_WRITE|PROT_EXEC), *p=stubs;
    for (const char *s=list; *s && p+128<stubs+65536;) {
        char *end;
        const uint64_t site=strtoull(s,&end,16);
        if (end==s) break;
        s=*end ? end+1 : end;
        unsigned char *at=image+site;
        if (at[0]!=0xe8) { printf("Trace: no call rel32 at 0x%llx\n",(unsigned long long)site); continue; }
        int32_t rel; memcpy(&rel,at+1,4);
        const uint64_t callee=(uint64_t)(uintptr_t)(at+5+rel), logger=(uint64_t)(uintptr_t)trace_return;
        unsigned char *stub=p;
        static const unsigned char head[]={0x48,0x83,0xec,0x08, 0xff,0x15,0x02,0x00,0x00,0x00, 0xeb,0x08}; /* sub rsp,8; call [rip+2]; jmp +8 */
        memcpy(p,head,sizeof(head)); p+=sizeof(head);
        memcpy(p,&callee,8); p+=8;
        static const unsigned char save[]={0x48,0x83,0xc4,0x08, 0x50,0x51,0x52,0x56,0x57,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53};
        memcpy(p,save,sizeof(save)); p+=sizeof(save);              /* add rsp,8; push rax..r11 */
        *p++=0x48; *p++=0xbf; memcpy(p,&site,8); p+=8;             /* mov rdi, site */
        *p++=0x48; *p++=0x89; *p++=0xc6;                           /* mov rsi, rax */
        *p++=0x48; *p++=0xb8; memcpy(p,&logger,8); p+=8;           /* mov rax, logger */
        *p++=0xff; *p++=0xd0;                                      /* call rax */
        static const unsigned char restore[]={0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x5f,0x5e,0x5a,0x59,0x58,0xc3};
        memcpy(p,restore,sizeof(restore)); p+=sizeof(restore);
        const int64_t to=(int64_t)(stub-(at+5));
        if (to!=(int32_t)to) { puts("Trace: stub out of rel32 reach"); continue; }
        rel=(int32_t)to;
#ifdef _WIN32
        DWORD old;
        VirtualProtect(at,5,PAGE_EXECUTE_READWRITE,&old);
        memcpy(at+1,&rel,4);
        VirtualProtect(at,5,old,&old);
#else
        unsigned char *page=(unsigned char *)((uintptr_t)at&~(uintptr_t)4095);
        mprotect(page,8192,PROT_READ|PROT_WRITE|PROT_EXEC);
        memcpy(at+1,&rel,4);
        mprotect(page,8192,PROT_READ|PROT_EXEC);
#endif
        printf("Trace: call at 0x%llx traced\n",(unsigned long long)site);
    }
}
