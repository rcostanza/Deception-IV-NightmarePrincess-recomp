/* libSceAudiodec: one-shot audio decoders the game drives itself (Deception's music and voices
 * are ATRAC9 in RIFF files). ATRAC9 decodes with LibAtrac9 (MIT, third_party/LibAtrac9); MP3
 * decoders are refused (NOT_SUPPORTED: no MP3 decoder is linked).
 *
 *   sceAudiodecCreateDecoder(ctrl, codecType): ctrl {param*, bsiInfo*, auInfo*, pcmItem*}.
 *     ATRAC9 param {u32 size, s32 wordSize, u8 config[4]}; the 36-byte bsiInfo receives
 *     {size, sampling rate, channels, bit rate, frames per superframe, superframe bytes, frame
 *     samples, encoder delay, 0}. The handle is >= 0.
 *   sceAudiodecDecode(handle, ctrl): one access unit (an ATRAC9 superframe) from auInfo
 *     {u32 size, void *addr, u32 bytes} into pcmItem {u32 size, void *addr, u32 bytes}; bytes
 *     become the bytes consumed and written. wordSize 0 writes 16-bit samples, otherwise 32-bit
 *     floats, interleaved. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "third_party/LibAtrac9/C/src/libatrac9.h"

#define AUDIODEC_ERROR_INVALID_TYPE ((int32_t)0x807F0001)
#define AUDIODEC_ERROR_INVALID_HANDLE ((int32_t)0x807F0003)
#define AUDIODEC_ERROR_INVALID_PARAM ((int32_t)0x807F0004)
#define AUDIODEC_ERROR_INVALID_SIZE ((int32_t)0x807F0005)
#define AUDIODEC_ERROR_INVALID_POINTER ((int32_t)0x807F0006)
#define AUDIODEC_ERROR_DECODE ((int32_t)0x807F0008)
#define AUDIODEC_ERROR_NOT_SUPPORTED ((int32_t)0x807F000A)
enum { TYPE_AT9=1, TYPE_MP3=2, MAX_DECODERS=64 };

typedef struct { void *param, *bsi, *au, *pcm; } Ctrl;
typedef struct { uint32_t size; uint32_t pad; void *addr; uint32_t bytes; uint32_t pad2; } Item;
_Static_assert(sizeof(Item)==24,"OrbisAudiodecAuInfo / PcmItem layout");

typedef struct {
    int used, float_out;
    void *at9;
    Atrac9CodecInfo info;
    unsigned char config[4];
    short *pcm; /* one superframe */
} Decoder;
static Decoder decoders[MAX_DECODERS];
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;

/* sceAudiodecInitLibrary(codecType) / sceAudiodecTermLibrary(codecType). */
static ABI int32_t init_library(uint32_t type) { return type==TYPE_AT9 || type==TYPE_MP3 ? 0 : AUDIODEC_ERROR_INVALID_TYPE; }
static ABI int32_t term_library(uint32_t type) { (void)type; return 0; }

static ABI int32_t create_decoder(const Ctrl *ctrl, uint32_t type) {
    if (!ctrl || !ctrl->param) return AUDIODEC_ERROR_INVALID_POINTER;
    if (type==TYPE_MP3) { puts("Runtime: Audiodec MP3 decoder refused (none linked)"); return AUDIODEC_ERROR_NOT_SUPPORTED; }
    if (type!=TYPE_AT9) return AUDIODEC_ERROR_INVALID_TYPE;
    const uint8_t *param=ctrl->param;
    uint32_t size; int32_t word; memcpy(&size,param,4); memcpy(&word,param+4,4);
    if (size<12) return AUDIODEC_ERROR_INVALID_SIZE;
    void *h=Atrac9GetHandle();
    unsigned char config[4]; memcpy(config,param+8,4);
    if (!h || Atrac9InitDecoder(h,config)) { if (h) Atrac9ReleaseHandle(h); return AUDIODEC_ERROR_INVALID_PARAM; }
    pthread_mutex_lock(&lock);
    int id=-1;
    for (int i=0;i<MAX_DECODERS;++i) if (!decoders[i].used) { id=i; break; }
    if (id<0) { pthread_mutex_unlock(&lock); Atrac9ReleaseHandle(h); return AUDIODEC_ERROR_INVALID_HANDLE; }
    Decoder *d=&decoders[id];
    memset(d,0,sizeof(*d));
    d->used=1; d->at9=h; d->float_out=word!=0; memcpy(d->config,config,4);
    Atrac9GetCodecInfo(h,&d->info);
    d->pcm=malloc((size_t)d->info.framesInSuperframe*d->info.frameSamples*d->info.channels*sizeof(short));
    pthread_mutex_unlock(&lock);
    if (ctrl->bsi) {
        uint32_t *bsi=ctrl->bsi, bsi_size=bsi[0];
        const uint32_t superframe_samples=(uint32_t)(d->info.framesInSuperframe*d->info.frameSamples);
        const uint32_t values[9]={bsi_size,(uint32_t)d->info.samplingRate,(uint32_t)d->info.channels,
            superframe_samples ? (uint32_t)((uint64_t)d->info.superframeSize*8*d->info.samplingRate/superframe_samples) : 0,
            (uint32_t)d->info.framesInSuperframe,(uint32_t)d->info.superframeSize,(uint32_t)d->info.frameSamples,0,0};
        memcpy(bsi+1,values+1,(bsi_size>=36 ? 36 : bsi_size>4 ? bsi_size : 4)-4);
    }
    if (runtime_verbose()) printf("Runtime: Audiodec ATRAC9 decoder %d: %d Hz, %d channels, %d-byte superframes, %s output\n",
                                  id,d->info.samplingRate,d->info.channels,d->info.superframeSize,d->float_out ? "float" : "16-bit");
    return id;
}
static Decoder *get(int32_t handle) {
    return handle>=0 && handle<MAX_DECODERS && decoders[handle].used ? &decoders[handle] : NULL;
}
static ABI int32_t delete_decoder(int32_t handle) {
    pthread_mutex_lock(&lock);
    Decoder *d=get(handle);
    if (d) { Atrac9ReleaseHandle(d->at9); free(d->pcm); memset(d,0,sizeof(*d)); }
    pthread_mutex_unlock(&lock);
    return d ? 0 : AUDIODEC_ERROR_INVALID_HANDLE;
}
static ABI int32_t decode(int32_t handle, const Ctrl *ctrl) {
    Decoder *d=get(handle);
    if (!d) return AUDIODEC_ERROR_INVALID_HANDLE;
    if (!ctrl || !ctrl->au || !ctrl->pcm) return AUDIODEC_ERROR_INVALID_POINTER;
    Item *au=ctrl->au, *pcm=ctrl->pcm;
    if (!au->addr || !pcm->addr || !d->pcm) return AUDIODEC_ERROR_INVALID_POINTER;
    const int frames=d->info.framesInSuperframe, frame_samples=d->info.frameSamples*d->info.channels;
    const uint32_t sample_bytes=d->float_out ? 4 : 2;
    if (au->bytes<(uint32_t)d->info.superframeSize) return AUDIODEC_ERROR_INVALID_SIZE;
    if (pcm->bytes<(uint32_t)(frames*frame_samples)*sample_bytes) return AUDIODEC_ERROR_INVALID_SIZE;
    const unsigned char *in=au->addr;
    uint32_t consumed=0;
    for (int f=0;f<frames;++f) {
        int used=0;
        if (Atrac9Decode(d->at9,in+consumed,d->pcm+(size_t)f*frame_samples,&used)) return AUDIODEC_ERROR_DECODE;
        consumed+=(uint32_t)used;
    }
    const size_t samples=(size_t)frames*frame_samples;
    if (d->float_out) {
        float *out=pcm->addr;
        for (size_t i=0;i<samples;++i) out[i]=(float)d->pcm[i]*(1.0f/32768.0f);
    } else memcpy(pcm->addr,d->pcm,samples*2);
    au->bytes=consumed;
    pcm->bytes=(uint32_t)(samples*sample_bytes);
    return 0;
}
/* sceAudiodecClearContext(handle): the decoder starts over, as after creation. */
static ABI int32_t clear_context(int32_t handle) {
    Decoder *d=get(handle);
    if (!d) return AUDIODEC_ERROR_INVALID_HANDLE;
    Atrac9ReleaseHandle(d->at9);
    d->at9=Atrac9GetHandle();
    return d->at9 && !Atrac9InitDecoder(d->at9,d->config) ? 0 : AUDIODEC_ERROR_INVALID_PARAM;
}

static const RuntimeExport exports[]={
    {"sceAudiodecInitLibrary",init_library}, {"sceAudiodecTermLibrary",term_library},
    {"sceAudiodecCreateDecoder",create_decoder}, {"sceAudiodecDeleteDecoder",delete_decoder},
    {"sceAudiodecDecode",decode}, {"sceAudiodecClearContext",clear_context},
};
uintptr_t runtime_audiodec_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
