/* libSceUlt: user-level threads. A PS4 ulthread runtime multiplexes its ulthreads cooperatively
 * on numWorkerThread kernel threads. Deception only creates runtimes and ulthreads and joins them
 * (no Ult mutexes, queues or yields), so each ulthread runs as a guest thread of its own: it starts
 * at once instead of waiting for a free worker, which only adds parallelism. The game's context
 * buffer (the ulthread's stack on PS4) is left unused; the thread has a host stack.
 *
 * The SceUltUlthread and SceUltUlthreadRuntime objects are opaque buffers in guest memory; they
 * hold a pointer to the state below in their first 8 bytes. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ULT_ERROR_NULL ((int32_t)0x80810001)
#define ULT_ERROR_INVALID ((int32_t)0x80810004)
#define ULT_ERROR_STATE ((int32_t)0x80810006)
#define ULT_ERROR_BUSY ((int32_t)0x80810007)

enum { ULT_MAGIC=0x544c55 /* "ULT" */ };
typedef struct { uint32_t magic, max_threads; char name[32]; } Runtime;
typedef struct { uint32_t magic; int joined; void *thread; } Ulthread;

/* sceUltInitialize / sceUltFinalize: the library's global state; nothing to hold here. */
static ABI int32_t ult_initialize(void) { return 0; }
static ABI int32_t ult_finalize(void) { return 0; }

/* _sceUltUlthreadRuntimeOptParamInitialize(param, buildVersion): zeroes the option block. */
static ABI int32_t runtime_opt_init(void *param, uint32_t build) {
    (void)build;
    if (!param) return ULT_ERROR_NULL;
    memset(param,0,128);
    return 0;
}
/* sceUltUlthreadRuntimeGetWorkAreaSize(numMaxUlthread, numWorkerThread): the work area the caller
 * provides. Unused here; a positive size keeps the caller's allocation valid. */
static ABI uint64_t runtime_work_area_size(uint32_t max_threads, uint32_t workers) {
    return 4096+(uint64_t)max_threads*64+(uint64_t)workers*256;
}
/* _sceUltUlthreadRuntimeCreate(runtime, name, numMaxUlthread, numWorkerThread, workArea, opt). */
static ABI int32_t runtime_create(void **object, const char *name, uint32_t max_threads, uint32_t workers,
                                  void *work_area, const void *opt) {
    (void)work_area; (void)opt;
    if (!object) return ULT_ERROR_NULL;
    if (!max_threads || !workers) return ULT_ERROR_INVALID;
    Runtime *r=calloc(1,sizeof(*r));
    if (!r) return ULT_ERROR_INVALID;
    r->magic=ULT_MAGIC; r->max_threads=max_threads;
    snprintf(r->name,sizeof(r->name),"%s",name ? name : "ult");
    *object=r;
    if (runtime_verbose()) printf("Ult: runtime '%s' (%u ulthreads, %u workers)\n",r->name,max_threads,workers);
    return 0;
}
static Runtime *get_runtime(void **object) {
    Runtime *r=object ? *object : NULL;
    return r && r->magic==ULT_MAGIC ? r : NULL;
}
/* sceUltUlthreadRuntimeDestroy(runtime): its ulthreads must have been joined. */
static ABI int32_t runtime_destroy(void **object) {
    Runtime *r=get_runtime(object);
    if (!r) return ULT_ERROR_INVALID;
    r->magic=0;
    free(r);
    *object=NULL;
    return 0;
}

/* _sceUltUlthreadCreate(ulthread, name, entry, arg, context, sizeContext, runtime, opt):
 * entry(arg) returns the int32 status sceUltUlthreadJoin reports. */
static ABI int32_t ulthread_create(void **object, const char *name, void *entry, uint64_t arg,
                                   void *context, uint64_t context_size, void **runtime, const void *opt) {
    (void)context; (void)context_size; (void)opt;
    if (!object || !entry) return ULT_ERROR_NULL;
    if (!get_runtime(runtime)) return ULT_ERROR_INVALID;
    Ulthread *u=calloc(1,sizeof(*u));
    if (!u) return ULT_ERROR_INVALID;
    u->magic=ULT_MAGIC;
    const int32_t e=runtime_thread_spawn(&u->thread,entry,(void *)(uintptr_t)arg,name ? name : "ulthread");
    if (e) { free(u); return e; }
    *object=u;
    return 0;
}
static Ulthread *get_ulthread(void **object) {
    Ulthread *u=object ? *object : NULL;
    return u && u->magic==ULT_MAGIC ? u : NULL;
}
static int32_t join(void **object, int32_t *status, int wait) {
    Ulthread *u=get_ulthread(object);
    if (!u) return ULT_ERROR_INVALID;
    if (u->joined) return ULT_ERROR_STATE;
    if (!wait && !runtime_thread_finished(u->thread)) return ULT_ERROR_BUSY;
    void *result=NULL;
    const int32_t e=runtime_thread_join(u->thread,&result);
    if (e) return e;
    u->joined=1;
    if (status) *status=(int32_t)(uintptr_t)result;
    return 0;
}
/* sceUltUlthreadJoin(ulthread, *status): waits for the ulthread to end. */
static ABI int32_t ulthread_join(void **object, int32_t *status) { return join(object,status,1); }
/* sceUltUlthreadTryJoin(ulthread, *status): BUSY while the ulthread still runs. */
static ABI int32_t ulthread_try_join(void **object, int32_t *status) { return join(object,status,0); }

static const RuntimeExport exports[]={
    {"sceUltInitialize",ult_initialize}, {"sceUltFinalize",ult_finalize},
    {"_sceUltUlthreadRuntimeOptParamInitialize",runtime_opt_init},
    {"sceUltUlthreadRuntimeGetWorkAreaSize",runtime_work_area_size},
    {"_sceUltUlthreadRuntimeCreate",runtime_create}, {"sceUltUlthreadRuntimeDestroy",runtime_destroy},
    {"_sceUltUlthreadCreate",ulthread_create}, {"sceUltUlthreadJoin",ulthread_join},
    {"sceUltUlthreadTryJoin",ulthread_try_join},
};
uintptr_t runtime_ult_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
