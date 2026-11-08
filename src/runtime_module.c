/* Modules the game loads at run time. link.py links every module of the dump into the
 * image; the deferred ones (any in sce_module besides libc and Fios2) are mapped and relocated at
 * boot and started here on request, as shadPS4's Linker::LoadAndStartModule does. Handles are
 * table index + 1 (0 is the eboot). */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#endif
#define KERNEL_ENOENT ((int32_t)0x80020002)
#define KERNEL_ESRCH ((int32_t)0x80020003)
#define KERNEL_EFAULT ((int32_t)0x8002000e)
#define KERNEL_EINVAL ((int32_t)0x80020016)

static unsigned char *image;
static RuntimeModule *modules;
static uint64_t module_count;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned starts, lookups, misses;
static RuntimeModule main_module; /* the eboot (runtime_module_set_main) */

void runtime_module_register(unsigned char *base, RuntimeModule *table, uint64_t count) {
    image=base; modules=table; module_count=count;
}

/* Guest return addresses on the calling thread's stack, for traces: each word between here and
 * the stack base (at most 64 KiB) that points into a module's code segment just after a call
 * (e8 rel32, or ff /2 of 2, 3, 6 or 7 bytes) prints as "Module.prx+0xoffset". Stale words from
 * returned calls show up too, so read it as a candidate list, innermost first. */
void runtime_guest_backtrace(char *out, size_t size, int max) {
    size_t used=0; out[0]=0;
    if (!image || !size) return;
    uintptr_t low=(uintptr_t)__builtin_frame_address(0), high=low+65536;
#ifdef _WIN32
    ULONG_PTR stack_low, stack_high;
    GetCurrentThreadStackLimits(&stack_low,&stack_high);
    if (high>stack_high) high=stack_high;
#endif
    for (uintptr_t p=low; p+8<=high && max>0; p+=8) {
        uintptr_t v=*(const uintptr_t *)p;
        for (uint64_t i=0;i<=module_count;++i) {
            const RuntimeModule *m=i<module_count ? &modules[i] : &main_module;
            uintptr_t start=(uintptr_t)image+m->seg0, end=start+m->seg0_size;
            if (!m->seg0_size || v<start+7 || v>=end) continue;
            const unsigned char *c=(const unsigned char *)v;
            if (c[-5]!=0xe8 && c[-2]!=0xff && c[-3]!=0xff && c[-6]!=0xff && c[-7]!=0xff) break;
            int n=snprintf(out+used,size-used,"%s%s+0x%llx",used ? " " : "",m->name,
                           (unsigned long long)(v-(uintptr_t)image-m->base));
            if (n<0 || (size_t)n>=size-used) return;
            used+=(size_t)n; --max;
            break;
        }
    }
}

/* SHA-1 (FIPS 180-1), only for symbol NIDs. */
static uint32_t rol(uint32_t v, int n) { return v<<n | v>>(32-n); }
static void sha1_block(uint32_t h[5], const unsigned char *b) {
    uint32_t w[80];
    for (int i=0;i<16;++i) w[i]=(uint32_t)b[i*4]<<24 | (uint32_t)b[i*4+1]<<16 | (uint32_t)b[i*4+2]<<8 | b[i*4+3];
    for (int i=16;i<80;++i) w[i]=rol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    uint32_t a=h[0], c=h[2], d=h[3], e=h[4], bb=h[1];
    for (int i=0;i<80;++i) {
        uint32_t f, k;
        if (i<20) { f=(bb&c)|(~bb&d); k=0x5a827999; }
        else if (i<40) { f=bb^c^d; k=0x6ed9eba1; }
        else if (i<60) { f=(bb&c)|(bb&d)|(c&d); k=0x8f1bbcdc; }
        else { f=bb^c^d; k=0xca62c1d6; }
        uint32_t t=rol(a,5)+f+e+k+w[i];
        e=d; d=c; c=rol(bb,30); bb=a; a=t;
    }
    h[0]+=a; h[1]+=bb; h[2]+=c; h[3]+=d; h[4]+=e;
}
static void sha1(const unsigned char *data, size_t size, unsigned char out[20]) {
    uint32_t h[5]={0x67452301,0xefcdab89,0x98badcfe,0x10325476,0xc3d2e1f0};
    unsigned char block[64];
    size_t i=0;
    for (; size-i>=64; i+=64) sha1_block(h,data+i);
    size_t rest=size-i;
    memset(block,0,64); memcpy(block,data+i,rest); block[rest]=0x80;
    if (rest>=56) { sha1_block(h,block); memset(block,0,64); }
    uint64_t bits=(uint64_t)size*8;
    for (int j=0;j<8;++j) block[63-j]=(unsigned char)(bits>>(j*8));
    sha1_block(h,block);
    for (int j=0;j<5;++j) { out[j*4]=(unsigned char)(h[j]>>24); out[j*4+1]=(unsigned char)(h[j]>>16); out[j*4+2]=(unsigned char)(h[j]>>8); out[j*4+3]=(unsigned char)h[j]; }
}
/* A symbol name's NID: the first 8 bytes of SHA-1(name + salt) as a little-endian number, in
 * the PS4's base64 alphabet (shadPS4 module.cpp, StringToNid). 0 when the name is too long. */
static int name_to_nid(const char *name, char nid[12]) {
    static const unsigned char salt[16]={0x51,0x8d,0x64,0xa6,0x35,0xde,0xd8,0xc1,0xe6,0xb0,0x39,0xb1,0xc3,0xe5,0x52,0x30};
    static const char codes[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    unsigned char input[512+16], hash[20];
    size_t n=strlen(name);
    if (n>512) return 0;
    memcpy(input,name,n); memcpy(input+n,salt,16);
    sha1(input,n+16,hash);
    uint64_t digest=0;
    for (int i=7;i>=0;--i) digest=digest<<8 | hash[i];
    for (int i=0;i<10;++i) nid[i]=codes[(digest>>(58-i*6))&0x3f];
    nid[10]=codes[(digest&0xf)*4]; nid[11]=0;
    return 1;
}

static const char *base_name(const char *path) {
    const char *slash=strrchr(path,'/');
    return slash ? slash+1 : path;
}
static int64_t find(const char *file) {
    for (uint64_t i=0;i<module_count;++i) if (!strcmp(modules[i].name,file)) return (int64_t)i;
    return -1;
}
int32_t runtime_module_start(const char *file, uint64_t args, const void *argp, int32_t *result) {
    int64_t i=find(file);
    if (i<0) return KERNEL_ENOENT;
    pthread_mutex_lock(&lock);
    int started=modules[i].started;
    modules[i].started=1; /* before the start routine: a module that loads itself gets its handle */
    pthread_mutex_unlock(&lock);
    if (started) return (int32_t)i+1;
    typedef int32_t (ABI *ModuleStart)(uint64_t,const void *,void *);
    printf("Runtime: starting module %s at image offset 0x%llx\n",file,(unsigned long long)modules[i].init);
    int32_t r=((ModuleStart)(image+modules[i].init))(args,argp,modules[i].procparam ? image+modules[i].procparam : NULL);
    printf("Runtime: module %s start returned %d\n",file,r);
    __atomic_add_fetch(&starts,1,__ATOMIC_RELAXED);
    if (result) *result=r;
    return (int32_t)i+1;
}
/* sceKernelLoadStartModule(path, args, argp, flags, option, result): the module's handle, or
 * ENOENT when the dump has no module of that file name. A module that already runs returns its
 * handle without starting again and leaves *result untouched. */
static ABI int32_t load_start_module(const char *path, uint64_t args, const void *argp, uint32_t flags,
                                     const void *option, int32_t *result) {
    (void)option;
    if (!path) return KERNEL_EFAULT;
    if (flags) { printf("Runtime: sceKernelLoadStartModule(%s) flags 0x%x unsupported\n",path,flags); return KERNEL_EINVAL; }
    int32_t handle=runtime_module_start(base_name(path),args,argp,result);
    if (runtime_verbose()) printf("Runtime: sceKernelLoadStartModule(%s) -> 0x%x\n",path,(unsigned)handle);
    return handle;
}
/* sceKernelDlsym(handle, name, address): the export of that module whose NID is the name's hash
 * (any of its libraries); ESRCH for an unknown or not started module, or no such export. */
static ABI int32_t kernel_dlsym(int32_t handle, const char *name, void **address) {
    if (!name || !address) return KERNEL_EFAULT;
    if (handle<1 || (uint64_t)handle>module_count || !modules[handle-1].started) return KERNEL_ESRCH;
    char nid[12];
    __atomic_add_fetch(&lookups,1,__ATOMIC_RELAXED);
    if (name_to_nid(name,nid)) {
        const RuntimeModule *m=&modules[handle-1];
        for (uint64_t i=0;i<m->export_count;++i)
            if (!strcmp(m->exports[i].nid,nid)) { *address=image+m->exports[i].address; return 0; }
    }
    __atomic_add_fetch(&misses,1,__ATOMIC_RELAXED);
    if (runtime_verbose()) printf("Runtime: sceKernelDlsym(%s, %s) not found\n",modules[handle-1].name,name);
    *address=NULL;
    return KERNEL_ESRCH;
}

/* The eboot's record: image offset 0 up to the first module, with its unwind fields (loader.c). */
static RuntimeModule main_module;
void runtime_module_set_main(const RuntimeModule *m) { main_module=*m; }

/* DECEPTION_THROW_TRACE=1: every C++ throw through libc.prx's __cxa_throw prints the thrown type
 * and the guest backtrace. The modules'
 * pointer slots bound to __cxa_throw get throw_thunk instead, which logs and then jumps on, so no
 * host frame is on the stack while the guest unwinds. */
static void *real_throw __attribute__((used));
static int readable(const void *p, size_t n) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION m;
    if (!p || !VirtualQuery(p,&m,sizeof(m)) || m.State!=MEM_COMMIT || (m.Protect & (PAGE_NOACCESS|PAGE_GUARD))) return 0;
    return (const char *)p+n<=(const char *)m.BaseAddress+m.RegionSize;
#else
    return p && (uintptr_t)p>=4096 && n;
#endif
}
/* __cxa_throw(object, tinfo, destructor): std::type_info is {vtable, mangled name}; the name is
 * printed as the Itanium ABI spells it (St13runtime_error). */
static ABI __attribute__((used)) void throw_log(void *object, void **tinfo) {
    char text[200]="(unknown type)", frames[2048];
    const char *name=readable(tinfo,16) ? (const char *)tinfo[1] : NULL;
    if (readable(name,1)) snprintf(text,sizeof(text),"%s",name);
    runtime_guest_backtrace(frames,sizeof(frames),24);
    printf("Throw trace: %s at %p\n    from %s\n",text,object,frames);
}
void throw_thunk(void);
__asm__(".text\n.globl throw_thunk\nthrow_thunk:\n"
        "push %rdi\npush %rsi\npush %rdx\n"  /* entry rsp%16==8: three pushes align the call */
        "call throw_log\n"
        "pop %rdx\npop %rsi\npop %rdi\n"
        "jmp *real_throw(%rip)\n");
void runtime_throw_trace_install(void) {
    const char *e=getenv("DECEPTION_THROW_TRACE");
    if (!e || e[0]!='1' || !image) return;
    for (uint64_t i=0;i<module_count && !real_throw;++i)
        if (!strcmp(modules[i].name,"libc.prx"))
            for (uint64_t x=0;x<modules[i].export_count;++x)
                if (!strcmp(modules[i].exports[x].nid,"vkuuLfhnSZI")) real_throw=image+modules[i].exports[x].address;
    if (!real_throw) { puts("Runtime: throw trace: libc.prx exports no __cxa_throw"); return; }
    unsigned slots=0;
    for (uint64_t i=0;i<=module_count;++i) {
        const RuntimeModule *m=i<module_count ? &modules[i] : &main_module;
        uint64_t *p=(uint64_t *)(image+((m->base+7)&~(uint64_t)7)), *end=(uint64_t *)(image+m->base+m->size);
        for (; p<end; ++p) {
            if (!((uintptr_t)p & 4095) && !readable(p,4096)) { p+=4096/8-1; continue; } /* segment gaps */
            if (*p!=(uint64_t)(uintptr_t)real_throw) continue;
            const unsigned char *at=(const unsigned char *)p;
            if (at>=image+m->seg0 && at<image+m->seg0+m->seg0_size) continue; /* code, not a slot */
#ifdef _WIN32
            DWORD old;
            if (!VirtualProtect(p,8,PAGE_READWRITE,&old)) continue;
            *p=(uint64_t)(uintptr_t)throw_thunk;
            VirtualProtect(p,8,old,&old);
#else
            *p=(uint64_t)(uintptr_t)throw_thunk;
#endif
            ++slots;
        }
    }
    printf("Runtime: throw trace on (%u __cxa_throw slots)\n",slots);
}
/* sceKernelGetModuleInfoForUnwind(address, flags, info) (shadPS4 process.cpp): flags 3 and up are
 * EINVAL (info zeroed), no info EFAULT, info->st_size below the 304-byte record EINVAL, an address
 * outside every module ESRCH. Otherwise the module's name, .eh_frame_hdr, .eh_frame and its size,
 * and first segment: libc's unwinder finds the FDEs of a frame with it. */
typedef struct {
    uint64_t st_size;
    char name[256];
    uint64_t eh_frame_hdr, eh_frame, eh_frame_size, seg0, seg0_size;
} UnwindInfo;
static ABI int32_t module_info_for_unwind(uint64_t address, int32_t flags, UnwindInfo *info) {
    if (flags>=3) { if (info) memset(info,0,sizeof(*info)); return KERNEL_EINVAL; }
    if (!info) return KERNEL_EFAULT;
    if (info->st_size<sizeof(*info)) return KERNEL_EINVAL;
    const RuntimeModule *m=NULL;
    uint64_t offset=address-(uint64_t)(uintptr_t)image;
    if (address>=(uint64_t)(uintptr_t)image) {
        if (offset<main_module.size) m=&main_module;
        for (uint64_t i=0;!m && i<module_count;++i)
            if (offset>=modules[i].base && offset-modules[i].base<modules[i].size) m=&modules[i];
    }
    if (!m) return KERNEL_ESRCH;
    uint64_t base=(uint64_t)(uintptr_t)image;
    memset(info,0,sizeof(*info));
    info->st_size=sizeof(*info);
    snprintf(info->name,sizeof(info->name),"%s",m->name);
    if (m->eh_frame_hdr) { info->eh_frame_hdr=base+m->eh_frame_hdr; info->eh_frame=base+m->eh_frame; info->eh_frame_size=m->eh_frame_size; }
    info->seg0=base+m->seg0; info->seg0_size=m->seg0_size;
    return 0;
}

static const RuntimeExport exports[]={
    {"sceKernelLoadStartModule",load_start_module}, {"sceKernelDlsym",kernel_dlsym},
    /* sceSysmodule's variant also hides the names of game-shipped system modules; not done here. */
    {"sceKernelGetModuleInfoForUnwind",module_info_for_unwind},
};
uintptr_t runtime_module_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_module_report(void) {
    unsigned deferred=0, started=0;
    for (uint64_t i=0;i<module_count;++i) { deferred+=(modules[i].flags & RUNTIME_MODULE_DEFERRED)!=0; started+=modules[i].started!=0; }
    printf("Runtime: modules linked=%llu (deferred=%u), running=%u, run-time starts=%u, dlsym=%u (%u not found)\n",
           (unsigned long long)module_count,deferred,started,starts,lookups,misses);
}
