/* libSceNgs2: Sony's software audio synthesizer. A system holds racks (samplers, submixers, the
 * mastering rack...), each with voices; the game sets voice parameters with sceNgs2VoiceControl
 * and pulls mixed PCM with sceNgs2SystemRender, which it sends to sceAudioOut.
 *
 * This implementation keeps the objects and renders silence: voices accept every command and
 * report that they are not playing, so the game's sound code runs on without sound. The handle
 * objects live in low memory (runtime_low_alloc): the game may keep handles in 40 bits. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define NGS2_ERROR_INVALID_HANDLE ((int32_t)0x804A0230)
#define NGS2_ERROR_INVALID_BUFFER_ADDRESS ((int32_t)0x804A0207)
#define NGS2_ERROR_INVALID_BUFFER_SIZE ((int32_t)0x804A0206)
#define NGS2_ERROR_INVALID_VOICE_INDEX ((int32_t)0x804A0302)
#define NGS2_ERROR_INVALID_OUT_ADDRESS ((int32_t)0x804A0053)

enum { KIND_SYSTEM=0x53324753, KIND_RACK=0x52324753, KIND_VOICE=0x56324753 }; /* "SG2S" "SG2R" "SG2V" */
enum { MAX_VOICES=1024 };

typedef struct Ngs2Object Ngs2Object;
struct Ngs2Object {
    uint32_t kind, rack_id, index, voices;
    Ngs2Object *owner;
    Ngs2Object **voice;
    pthread_mutex_t lock;
};

/* SceNgs2ContextBufferInfo: hostBuffer, hostBufferSize, reserved[5], userData. */
typedef struct { void *buffer; uint64_t size; uint64_t reserved[5]; uint64_t user; } BufferInfo;
/* SceNgs2RenderBufferInfo: buffer, bufferSize, waveformType, numChannels. */
typedef struct { void *buffer; uint64_t size; uint32_t waveform, channels; } RenderBufferInfo;
_Static_assert(sizeof(BufferInfo)==64 && sizeof(RenderBufferInfo)==24,"Ngs2 buffer layouts");

static Ngs2Object *new_object(uint32_t kind, Ngs2Object *owner) {
    Ngs2Object *o=runtime_low_alloc(sizeof(*o));
    if (!o) return NULL;
    memset(o,0,sizeof(*o));
    o->kind=kind; o->owner=owner;
    pthread_mutex_init(&o->lock,NULL);
    return o;
}
static Ngs2Object *as(void *handle, uint32_t kind) {
    Ngs2Object *o=handle;
    return o && o->kind==kind ? o : NULL;
}
static void free_object(Ngs2Object *o) {
    pthread_mutex_destroy(&o->lock);
    o->kind=0;
    runtime_low_free(o);
}

/* The host buffer the game allocates for a system or a rack: a size it can always provide. */
enum { CONTEXT_BYTES=64*1024 };
/* sceNgs2SystemQueryBufferSize(option, *bufferInfo) / sceNgs2RackQueryBufferSize(rackId,
 * option, *bufferInfo): the context buffer each needs. */
static ABI int32_t system_query_size(const void *option, BufferInfo *info) {
    (void)option;
    if (!info) return NGS2_ERROR_INVALID_OUT_ADDRESS;
    memset(info,0,sizeof(*info)); info->size=CONTEXT_BYTES;
    return 0;
}
static ABI int32_t rack_query_size(uint32_t rack_id, const void *option, BufferInfo *info) {
    (void)rack_id; (void)option;
    if (!info) return NGS2_ERROR_INVALID_OUT_ADDRESS;
    memset(info,0,sizeof(*info)); info->size=CONTEXT_BYTES;
    return 0;
}
/* sceNgs2SystemCreate(option, bufferInfo, *handle). */
static ABI int32_t system_create(const void *option, const BufferInfo *info, void **out) {
    (void)option;
    if (!out) return NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (!info || !info->buffer) return NGS2_ERROR_INVALID_BUFFER_ADDRESS;
    if (info->size<CONTEXT_BYTES) return NGS2_ERROR_INVALID_BUFFER_SIZE;
    Ngs2Object *s=new_object(KIND_SYSTEM,NULL);
    if (!s) return NGS2_ERROR_INVALID_BUFFER_ADDRESS;
    *out=s;
    puts("Runtime: Ngs2 system created (silent: no voice is rendered)");
    return 0;
}
/* sceNgs2SystemDestroy(handle, *bufferInfo): the buffer given at creation goes back to the caller
 * (here: its fields zeroed; the game frees what it allocated). */
static ABI int32_t system_destroy(void *handle, BufferInfo *info) {
    Ngs2Object *s=as(handle,KIND_SYSTEM);
    if (!s) return NGS2_ERROR_INVALID_HANDLE;
    if (info) memset(info,0,sizeof(*info));
    free_object(s);
    return 0;
}
/* sceNgs2SystemLock / Unlock(handle): serialize parameter changes with rendering. */
static ABI int32_t system_lock(void *handle) {
    Ngs2Object *s=as(handle,KIND_SYSTEM);
    if (!s) return NGS2_ERROR_INVALID_HANDLE;
    pthread_mutex_lock(&s->lock); return 0;
}
static ABI int32_t system_unlock(void *handle) {
    Ngs2Object *s=as(handle,KIND_SYSTEM);
    if (!s) return NGS2_ERROR_INVALID_HANDLE;
    pthread_mutex_unlock(&s->lock); return 0;
}
/* sceNgs2SystemRender(handle, bufferInfo[], count): one grain of PCM into each buffer; silence. */
static ABI int32_t system_render(void *handle, const RenderBufferInfo *buffers, uint32_t count) {
    if (!as(handle,KIND_SYSTEM)) return NGS2_ERROR_INVALID_HANDLE;
    if (count && !buffers) return NGS2_ERROR_INVALID_BUFFER_ADDRESS;
    for (uint32_t i=0;i<count;++i) if (buffers[i].buffer) memset(buffers[i].buffer,0,buffers[i].size);
    return 0;
}
/* sceNgs2RackCreate(system, rackId, option, bufferInfo, *handle): option->maxVoices (the 32-bit
 * field after the 16-byte option header and the 16-byte name) voices, default 1. */
static ABI int32_t rack_create(void *system, uint32_t rack_id, const uint8_t *option, const BufferInfo *info, void **out) {
    Ngs2Object *s=as(system,KIND_SYSTEM);
    if (!s) return NGS2_ERROR_INVALID_HANDLE;
    if (!out) return NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (!info || !info->buffer) return NGS2_ERROR_INVALID_BUFFER_ADDRESS;
    uint32_t voices=1;
    if (option) { memcpy(&voices,option+32,4); if (!voices || voices>MAX_VOICES) voices=voices ? MAX_VOICES : 1; }
    Ngs2Object *r=new_object(KIND_RACK,s);
    if (!r) return NGS2_ERROR_INVALID_BUFFER_ADDRESS;
    r->rack_id=rack_id; r->voices=voices;
    r->voice=calloc(voices,sizeof(*r->voice));
    for (uint32_t i=0;r->voice && i<voices;++i) {
        r->voice[i]=new_object(KIND_VOICE,r);
        if (r->voice[i]) r->voice[i]->index=i;
    }
    *out=r;
    if (runtime_verbose()) printf("Runtime: Ngs2 rack 0x%x created with %u voices\n",rack_id,voices);
    return 0;
}
/* sceNgs2RackDestroy(rack, *bufferInfo). */
static ABI int32_t rack_destroy(void *handle, BufferInfo *info) {
    Ngs2Object *r=as(handle,KIND_RACK);
    if (!r) return NGS2_ERROR_INVALID_HANDLE;
    if (info) memset(info,0,sizeof(*info));
    for (uint32_t i=0;r->voice && i<r->voices;++i) if (r->voice[i]) free_object(r->voice[i]);
    free(r->voice);
    free_object(r);
    return 0;
}
/* sceNgs2RackGetVoiceHandle(rack, voiceIndex, *handle). */
static ABI int32_t rack_voice(void *handle, uint32_t index, void **out) {
    Ngs2Object *r=as(handle,KIND_RACK);
    if (!r) return NGS2_ERROR_INVALID_HANDLE;
    if (!out) return NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (index>=r->voices || !r->voice || !r->voice[index]) return NGS2_ERROR_INVALID_VOICE_INDEX;
    *out=r->voice[index];
    return 0;
}
/* sceNgs2VoiceControl(voice, paramList): parameters accepted, nothing is played. */
static ABI int32_t voice_control(void *handle, const void *params) {
    (void)params;
    return as(handle,KIND_VOICE) ? 0 : NGS2_ERROR_INVALID_HANDLE;
}
/* sceNgs2VoiceGetState(voice, *state, size): a voice that is not in use (flags 0). */
static ABI int32_t voice_state(void *handle, void *state, uint64_t size) {
    if (!as(handle,KIND_VOICE)) return NGS2_ERROR_INVALID_HANDLE;
    if (!state) return NGS2_ERROR_INVALID_OUT_ADDRESS;
    memset(state,0,size);
    return 0;
}
/* sceNgs2VoiceGetStateFlags(voice, *flags). */
static ABI int32_t voice_state_flags(void *handle, uint32_t *flags) {
    if (!as(handle,KIND_VOICE)) return NGS2_ERROR_INVALID_HANDLE;
    if (!flags) return NGS2_ERROR_INVALID_OUT_ADDRESS;
    *flags=0;
    return 0;
}
/* sceNgs2VoiceGetMatrixInfo(voice, matrixId, *info, size) / sceNgs2VoiceGetPortInfo(voice,
 * port, *info, size): an unset matrix and an unconnected port (zeros). */
static ABI int32_t voice_matrix_info(void *handle, uint32_t id, void *info, uint64_t size) {
    (void)id;
    if (!as(handle,KIND_VOICE)) return NGS2_ERROR_INVALID_HANDLE;
    if (!info) return NGS2_ERROR_INVALID_OUT_ADDRESS;
    memset(info,0,size);
    return 0;
}
static ABI int32_t voice_port_info(void *handle, uint32_t port, void *info, uint64_t size) {
    (void)port;
    if (!as(handle,KIND_VOICE)) return NGS2_ERROR_INVALID_HANDLE;
    if (!info) return NGS2_ERROR_INVALID_OUT_ADDRESS;
    memset(info,0,size);
    return 0;
}

static const RuntimeExport exports[]={
    {"sceNgs2SystemQueryBufferSize",system_query_size}, {"sceNgs2SystemCreate",system_create},
    {"sceNgs2SystemDestroy",system_destroy}, {"sceNgs2SystemLock",system_lock},
    {"sceNgs2SystemUnlock",system_unlock}, {"sceNgs2SystemRender",system_render},
    {"sceNgs2RackQueryBufferSize",rack_query_size}, {"sceNgs2RackCreate",rack_create},
    {"sceNgs2RackDestroy",rack_destroy}, {"sceNgs2RackGetVoiceHandle",rack_voice},
    {"sceNgs2VoiceControl",voice_control}, {"sceNgs2VoiceGetState",voice_state},
    {"sceNgs2VoiceGetStateFlags",voice_state_flags}, {"sceNgs2VoiceGetMatrixInfo",voice_matrix_info},
    {"sceNgs2VoiceGetPortInfo",voice_port_info},
};
uintptr_t runtime_ngs2_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
