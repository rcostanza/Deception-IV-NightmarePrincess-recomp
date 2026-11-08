/* libSceAjm: the audio decoder service. Wwise initializes it and registers its codecs, but
 * Deception's sounds are all Wwise ADPCM and Vorbis (audio.pck), decoded on the CPU: no decoder
 * instance is ever created, so contexts and codec registration are kept and an instance stops
 * the run, naming the codec. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#define ERR_INVALID_CONTEXT ((int32_t)0x80930002)
#define ERR_INVALID_INSTANCE ((int32_t)0x80930003)
#define ERR_INVALID_BATCH ((int32_t)0x80930004)
#define ERR_INVALID_PARAMETER ((int32_t)0x80930005)
#define ERR_OUT_OF_RESOURCES ((int32_t)0x80930007)
#define ERR_ALREADY_REGISTERED ((int32_t)0x80930009)
#define ERR_NOT_REGISTERED ((int32_t)0x8093000A)
#define MAX_CONTEXTS 8
#define CODECS 24

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int contexts[MAX_CONTEXTS+1][CODECS+1]; /* [id][0]: in use; [id][1+codec]: registered */

/* sceAjmInitialize(reserved, context): a new context id (1..8). */
static ABI int32_t ajm_initialize(int64_t reserved, uint32_t *out) {
    if (!out || reserved) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&lock);
    for (uint32_t i=1;i<=MAX_CONTEXTS;++i) if (!contexts[i][0]) {
        contexts[i][0]=1; pthread_mutex_unlock(&lock);
        *out=i; puts("Runtime: Ajm context initialized (no decoders: Deception's audio is Wwise ADPCM and Vorbis)");
        return 0;
    }
    pthread_mutex_unlock(&lock);
    return ERR_OUT_OF_RESOURCES;
}
static int *context(uint32_t id) { return id && id<=MAX_CONTEXTS && contexts[id][0] ? contexts[id] : NULL; }
/* sceAjmFinalize(context): drops the context and its registrations. */
static ABI int32_t ajm_finalize(uint32_t id) {
    pthread_mutex_lock(&lock);
    int *c=context(id);
    if (c) for (int i=0;i<=CODECS;++i) c[i]=0;
    pthread_mutex_unlock(&lock);
    return c ? 0 : ERR_INVALID_CONTEXT;
}
/* sceAjmModuleRegister(context, codec, reserved): ALREADY_REGISTERED the second time. */
static ABI int32_t ajm_module_register(uint32_t id, uint32_t codec, int64_t reserved) {
    if (reserved || codec>=CODECS) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&lock);
    int *c=context(id);
    int32_t r=!c ? ERR_INVALID_CONTEXT : c[1+codec] ? ERR_ALREADY_REGISTERED : 0;
    if (!r) c[1+codec]=1;
    pthread_mutex_unlock(&lock);
    return r;
}
/* sceAjmModuleUnregister(context, codec): NOT_REGISTERED for a codec never registered. */
static ABI int32_t ajm_module_unregister(uint32_t id, uint32_t codec) {
    pthread_mutex_lock(&lock);
    int *c=context(id);
    int32_t r=!c ? ERR_INVALID_CONTEXT : codec>=CODECS ? ERR_INVALID_PARAMETER : !c[1+codec] ? ERR_NOT_REGISTERED : 0;
    if (!r) c[1+codec]=0;
    pthread_mutex_unlock(&lock);
    return r;
}
/* sceAjmInstanceCreate(context, codec, flags, instance): a decoder this runner does not have. */
static ABI int32_t ajm_instance_create(uint32_t id, uint32_t codec, uint64_t flags, uint32_t *out) {
    (void)id; (void)out;
    fprintf(stderr,"STOP: Ajm decoder instance for codec %u (flags %#llx; 0=MP3, 1=ATRAC9, 2=AAC) is not implemented\n",
            codec,(unsigned long long)flags);
    exit(21);
}
/* Without instances there is nothing to destroy, build a job for or run. */
static ABI int32_t ajm_instance_destroy(uint32_t id, uint32_t instance) { (void)id; (void)instance; return ERR_INVALID_INSTANCE; }
static ABI void *ajm_job_none(void *buffer) { (void)buffer; return NULL; }
static ABI int32_t ajm_batch_start(uint32_t id, void *buffer, uint32_t size, int priority, void *error, uint32_t *out) {
    (void)buffer; (void)size; (void)priority; (void)error; (void)out;
    return context(id) ? ERR_INVALID_INSTANCE : ERR_INVALID_CONTEXT;
}
static ABI int32_t ajm_batch_wait(uint32_t id, uint32_t batch, uint32_t timeout, void *error) {
    (void)batch; (void)timeout; (void)error;
    return context(id) ? ERR_INVALID_BATCH : ERR_INVALID_CONTEXT;
}

static const RuntimeExport exports[]={
    {"sceAjmInitialize",ajm_initialize}, {"sceAjmFinalize",ajm_finalize},
    {"sceAjmModuleRegister",ajm_module_register}, {"sceAjmModuleUnregister",ajm_module_unregister},
    {"sceAjmInstanceCreate",ajm_instance_create}, {"sceAjmInstanceDestroy",ajm_instance_destroy},
    {"sceAjmBatchJobControlBufferRa",ajm_job_none}, {"sceAjmBatchJobRunSplitBufferRa",ajm_job_none},
    {"sceAjmBatchStartBuffer",ajm_batch_start}, {"sceAjmBatchWait",ajm_batch_wait},
};
uintptr_t runtime_ajm_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
