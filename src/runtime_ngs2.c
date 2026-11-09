/* libSceNgs2: Sony's software audio synthesizer. A system holds racks, each with voices; the game
 * sets voice parameters with sceNgs2VoiceControl and pulls mixed PCM with sceNgs2SystemRender,
 * which it sends to sceAudioOut.
 *
 * Deception's graph: sampler racks (0x1000: "Sfx" 256 voices of ATRAC9 banks, "Stream" 12 voices
 * of 16-bit PCM it decodes with sceAudiodec, "Movie") patch into the custom racks (0x4002:
 * "DuckingComp", "MultiFx"), which patch into "Mastering" (0x3000), whose input is what Render
 * writes. Sampler port 1 sends to "Reverb" (0x2001); "DuckingTrig" (0x2000) is a side chain whose
 * peak the game reads to duck the music. Here samplers play their waveform blocks (16-bit PCM,
 * float PCM or ATRAC9 through LibAtrac9) resampled to 48 kHz with their pitch, each port mixes
 * through its matrix ([dst][src] levels) and volume into the patched voice's 8-channel input, and
 * every other voice passes its input on unchanged: no reverb, compression or filters, and the side
 * chain feeds nothing.
 *
 * Parameter blocks: {u16 size, s16 next (offset to the next block, 0 ends), u32 id, ...}.
 *   1 matrix levels {matrixId, numLevels, float *levels}   2 port volume {port, float level}
 *   3 port matrix {port, s32 matrixId}                       5 patch {port, inputId, destHandle}
 *   6 kick event {event: 0 play, 1 stop, 2 stop now, 3 kill, 4 pause, 5 resume}
 *   0x10000000 sampler setup {type, channels, rate, config, frameOffset, frameMargin, flags}
 *   0x10000001 add blocks {data, flags, count, block *}: block {offset, size, repeats, skip,
 *              samples, reserved, u64 userData}   0x10000002 replace address {from, to}
 *   0x10000004 exit loop   0x10000005 pitch {float ratio}
 * The handle objects live in low memory (runtime_low_alloc): the game may keep handles in 40 bits. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include "third_party/LibAtrac9/C/src/libatrac9.h"

#define NGS2_ERROR_INVALID_HANDLE ((int32_t)0x804A0230)
#define NGS2_ERROR_INVALID_BUFFER_ADDRESS ((int32_t)0x804A0207)
#define NGS2_ERROR_INVALID_BUFFER_SIZE ((int32_t)0x804A0206)
#define NGS2_ERROR_INVALID_VOICE_INDEX ((int32_t)0x804A0302)
#define NGS2_ERROR_INVALID_OUT_ADDRESS ((int32_t)0x804A0053)

enum { KIND_SYSTEM=0x53324753, KIND_RACK=0x52324753, KIND_VOICE=0x56324753 }; /* "SG2S" "SG2R" "SG2V" */
enum { MAX_VOICES=1024, MAX_RACKS=32, CH=8, GRAIN=1024, PORTS=8, MATRICES=8, BLOCKS=64, OUT_RATE=48000 };
enum { RACK_SAMPLER=0x1000, RACK_MASTERING=0x3000 };
enum { WAVE_I16L=0x12, WAVE_F32L=0x18, WAVE_AT9=0x40 };
enum { FLAG_IN_USE=1, FLAG_PLAYING=2, FLAG_PAUSED=4, FLAG_STOPPED=8, FLAG_EMPTY=0x20 };
enum { RELEASE_FRAMES=256 };

typedef struct Ngs2Object Ngs2Object;
typedef struct { Ngs2Object *dest; uint32_t input; float volume; int32_t matrix; } Port;
typedef struct { uint32_t count; float level[CH*CH]; } Matrix;
typedef struct { const uint8_t *data; uint32_t offset, size, skip, samples; int32_t repeats; uint64_t user; } Block;

/* A voice's mixing state (ordinary memory; the handle object only points at it). */
typedef struct {
    Port port[PORTS];
    Matrix matrix[MATRICES];
    float (*in)[GRAIN];            /* CH x GRAIN input, voices that take input */
    float peak;
    /* sampler */
    uint32_t wave, channels, rate, config;
    Block block[BLOCKS]; uint32_t blocks;
    int playing, paused, releasing;
    int fed;                       /* a frame has played since the last play event */
    float pitch;
    void *at9; Atrac9CodecInfo info;
    short *pcm; uint32_t pcm_len, pcm_pos; /* decoded frames of the current superframe */
    uint32_t byte_pos, frame_pos;          /* in the current block: bytes read, frames decoded */
    int block_started;
    float cur[CH], nxt[CH], frac; int primed;
    uint64_t decoded_samples, decoded_bytes;
} Mix;

struct Ngs2Object {
    uint32_t kind, rack_id, index, voices;
    Ngs2Object *owner;             /* voice: its rack; rack: its system */
    Ngs2Object **voice;            /* rack: its voices */
    Ngs2Object *rack[MAX_RACKS];   /* system: its racks */
    uint32_t racks;
    char name[17];
    Mix *mix;
    pthread_mutex_t lock;          /* system: sceNgs2SystemLock */
    pthread_mutex_t render;        /* system: parameters against rendering */
};

/* SceNgs2ContextBufferInfo: hostBuffer, hostBufferSize, reserved[5], userData. */
typedef struct { void *buffer; uint64_t size; uint64_t reserved[5]; uint64_t user; } BufferInfo;
/* SceNgs2RenderBufferInfo: buffer, bufferSize, waveformType, numChannels. */
typedef struct { void *buffer; uint64_t size; uint32_t waveform, channels; } RenderBufferInfo;
_Static_assert(sizeof(BufferInfo)==64 && sizeof(RenderBufferInfo)==24,"Ngs2 buffer layouts");

/* DECEPTION_NGS2_TRACE=1 logs racks, render buffers and voice parameter blocks. */
static int trace_level(void) {
    static int level=-1;
    if (level<0) { const char *e=getenv("DECEPTION_NGS2_TRACE"); level=e ? atoi(e) : 0; }
    return level;
}

static Ngs2Object *new_object(uint32_t kind, Ngs2Object *owner) {
    Ngs2Object *o=runtime_low_alloc(sizeof(*o));
    if (!o) return NULL;
    memset(o,0,sizeof(*o));
    o->kind=kind; o->owner=owner;
    /* sceNgs2SystemLock nests: Render takes it too, and the game may render while holding it. */
    pthread_mutexattr_t recursive;
    pthread_mutexattr_init(&recursive);
    pthread_mutexattr_settype(&recursive,PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&o->lock,&recursive);
    pthread_mutexattr_destroy(&recursive);
    pthread_mutex_init(&o->render,NULL);
    return o;
}
static Ngs2Object *as(void *handle, uint32_t kind) {
    Ngs2Object *o=handle;
    return o && o->kind==kind ? o : NULL;
}
static void free_object(Ngs2Object *o) {
    pthread_mutex_destroy(&o->lock);
    pthread_mutex_destroy(&o->render);
    o->kind=0;
    runtime_low_free(o);
}
static Ngs2Object *system_of(Ngs2Object *voice) { return voice->owner->owner; }
static int is_sampler(const Ngs2Object *voice) { return (voice->owner->rack_id&0xf000)==RACK_SAMPLER; }

static void reset_ports(Mix *m) {
    for (int i=0;i<PORTS;++i) m->port[i]=(Port){NULL,0,1.0f,-1};
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
    puts("Runtime: Ngs2 system created");
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

/* --- sampler voices ------------------------------------------------------------------------ */

static void at9_release(Mix *m) {
    if (m->at9) Atrac9ReleaseHandle(m->at9);
    m->at9=NULL;
}
/* The block at the head of the queue starts over (decoder reset, its leading samples skipped). */
static void block_start(Mix *m) {
    m->byte_pos=m->frame_pos=0; m->pcm_len=m->pcm_pos=0; m->block_started=1;
    if (m->wave==WAVE_AT9) {
        at9_release(m);
        unsigned char config[4]={(unsigned char)(m->config>>24),(unsigned char)(m->config>>16),(unsigned char)(m->config>>8),(unsigned char)m->config};
        m->at9=Atrac9GetHandle();
        if (m->at9 && Atrac9InitDecoder(m->at9,config)) at9_release(m);
        if (m->at9) {
            Atrac9GetCodecInfo(m->at9,&m->info);
            if (!m->pcm) m->pcm=malloc(sizeof(short)*4*1024*CH);
        }
    }
}
static void drop_block(Mix *m) {
    if (!m->blocks) return;
    memmove(m->block,m->block+1,sizeof(Block)*(m->blocks-1));
    --m->blocks;
    m->block_started=0;
}
static void stop_voice(Mix *m) {
    m->playing=m->paused=m->releasing=0; m->primed=0;
}
/* The block's next frame of source samples into out[channels]; 0 when the queue runs dry. */
static int read_frame(Mix *m, float *out) {
    for (int guard=0;guard<BLOCKS*4;++guard) {
        if (!m->blocks) return 0;
        Block *b=&m->block[0];
        if (!m->block_started) block_start(m);
        const uint32_t ch=m->channels ? m->channels : 1;
        int have=0;
        if (m->wave==WAVE_I16L || m->wave==WAVE_F32L) {
            const uint32_t bytes=m->wave==WAVE_I16L ? 2 : 4;
            const uint32_t total=b->samples ? b->samples+b->skip : b->size/(bytes*ch);
            while (m->frame_pos<b->skip && m->frame_pos<total) ++m->frame_pos;
            if (m->frame_pos<total && (uint64_t)(m->frame_pos+1)*bytes*ch<=b->size) {
                const uint8_t *p=b->data+b->offset+(size_t)m->frame_pos*bytes*ch;
                for (uint32_t c=0;c<ch && c<CH;++c) {
                    if (bytes==2) { int16_t v; memcpy(&v,p+c*2,2); out[c]=v*(1.0f/32768.0f); }
                    else memcpy(&out[c],p+c*4,4);
                }
                ++m->frame_pos; m->decoded_bytes+=bytes*ch; have=1;
            }
        } else if (m->wave==WAVE_AT9 && m->at9) {
            const uint32_t total=b->samples+b->skip;
            while (!have && (!b->samples || m->frame_pos<total)) {
                if (m->pcm_pos>=m->pcm_len) {
                    const uint32_t sf=(uint32_t)m->info.superframeSize;
                    if (!sf || m->byte_pos+sf>b->size) break;
                    const unsigned char *in=b->data+b->offset+m->byte_pos;
                    const int frames=m->info.framesInSuperframe, fs=m->info.frameSamples*m->info.channels;
                    uint32_t used_total=0; int ok=1;
                    for (int f=0;f<frames;++f) {
                        int used=0;
                        if (Atrac9Decode(m->at9,in+used_total,m->pcm+(size_t)f*fs,&used)) { ok=0; break; }
                        used_total+=(uint32_t)used;
                    }
                    if (!ok) break;
                    /* frames vary in size; the superframe is padded to its fixed size */
                    m->byte_pos+=sf; m->decoded_bytes+=sf;
                    m->pcm_len=(uint32_t)(frames*m->info.frameSamples); m->pcm_pos=0;
                }
                const uint32_t at=m->frame_pos++;
                const short *s=m->pcm+(size_t)m->pcm_pos++*m->info.channels;
                if (at<b->skip) continue;
                for (int c=0;c<m->info.channels && c<CH;++c) out[c]=s[c]*(1.0f/32768.0f);
                have=1;
            }
        }
        if (have) { ++m->decoded_samples; m->fed=1; return 1; }
        /* The block is done: again while it repeats (a negative count loops until exit loop). */
        if (b->repeats) { if (b->repeats>0) --b->repeats; m->block_started=0; continue; }
        drop_block(m);
    }
    return 0;
}
/* frames of the sampler's output (its own channels) at 48 kHz into out. */
static void sampler_render(Mix *m, float out[CH][GRAIN], uint32_t frames) {
    const uint32_t ch=m->channels ? (m->channels<CH ? m->channels : CH) : 1;
    for (uint32_t c=0;c<ch;++c) memset(out[c],0,sizeof(float)*frames);
    if (!m->playing || m->paused) return;
    const float step=m->pitch*(float)(m->rate ? m->rate : OUT_RATE)/(float)OUT_RATE;
    if (!m->primed) {
        memset(m->cur,0,sizeof(m->cur)); memset(m->nxt,0,sizeof(m->nxt));
        /* Played before its blocks arrive (the game kicks the stream voice first): it waits. */
        if (!read_frame(m,m->cur)) { if (m->fed) stop_voice(m); return; }
        if (!read_frame(m,m->nxt)) memset(m->nxt,0,sizeof(m->nxt));
        m->frac=0; m->primed=1;
    }
    for (uint32_t i=0;i<frames;++i) {
        float gain=1.0f;
        if (m->releasing) {
            gain=(float)m->releasing/RELEASE_FRAMES;
            if (!--m->releasing) { stop_voice(m); return; }
        }
        for (uint32_t c=0;c<ch;++c) out[c][i]=(m->cur[c]+(m->nxt[c]-m->cur[c])*m->frac)*gain;
        m->frac+=step;
        while (m->frac>=1.0f) {
            m->frac-=1.0f;
            memcpy(m->cur,m->nxt,sizeof(m->cur));
            if (!read_frame(m,m->nxt)) {
                if (!m->blocks) { /* the last frame plays out, then the voice stops */
                    memset(m->nxt,0,sizeof(m->nxt));
                    if (i+1<frames) for (uint32_t c=0;c<ch;++c) out[c][i+1]=0;
                    stop_voice(m); return;
                }
            }
        }
    }
}

/* --- mixing -------------------------------------------------------------------------------- */

/* src (src_ch channels) through port p into its destination's input. */
static void mix_port(const Mix *m, const Port *p, float src[CH][GRAIN], uint32_t src_ch, uint32_t frames) {
    Mix *d=p->dest && p->dest->mix ? p->dest->mix : NULL;
    if (!d || !d->in || p->volume==0.0f) return;
    const Matrix *mx=p->matrix>=0 && p->matrix<MATRICES && m->matrix[p->matrix].count ? &m->matrix[p->matrix] : NULL;
    if (mx) {
        const uint32_t dst_ch=mx->count/src_ch;
        for (uint32_t o=0;o<dst_ch && o<CH;++o)
            for (uint32_t s=0;s<src_ch;++s) {
                const float k=mx->level[o*src_ch+s]*p->volume;
                if (k!=0.0f) for (uint32_t i=0;i<frames;++i) d->in[o][i]+=src[s][i]*k;
            }
    } else {
        for (uint32_t c=0;c<src_ch && c<CH;++c)
            for (uint32_t i=0;i<frames;++i) d->in[c][i]+=src[c][i]*p->volume;
    }
}
/* Whether a voice's output goes on: the side chain and the reverb (not modelled) feed nothing. */
static int passes_input(const Ngs2Object *v) {
    return v->owner->rack_id!=0x2001 && strcmp(v->owner->name,"DuckingTrig")!=0;
}

static void render_mix(Ngs2Object *s, uint32_t frames) {
    static float tmp[CH][GRAIN];
    Ngs2Object *order[MAX_VOICES]; uint32_t count=0;
    static uint32_t indeg[MAX_VOICES];
    /* Every voice, inputs cleared; the samplers first, then the others in patch order. */
    for (uint32_t r=0;r<s->racks;++r) {
        Ngs2Object *rack=s->rack[r];
        for (uint32_t i=0;i<rack->voices && count<MAX_VOICES;++i) {
            Ngs2Object *v=rack->voice[i];
            if (!v || !v->mix) continue;
            if (v->mix->in) for (int c=0;c<CH;++c) memset(v->mix->in[c],0,sizeof(float)*frames);
            v->index=i;
            order[count++]=v;
        }
    }
    /* Kahn: a voice renders after every voice patched into it. */
    for (uint32_t i=0;i<count;++i) indeg[i]=0;
    for (uint32_t i=0;i<count;++i)
        for (int p=0;p<PORTS;++p) {
            Ngs2Object *d=order[i]->mix->port[p].dest;
            if (!d || d==order[i]) continue;
            for (uint32_t j=0;j<count;++j) if (order[j]==d) { ++indeg[j]; break; }
        }
    uint8_t done[MAX_VOICES]={0};
    for (uint32_t rendered=0;rendered<count;) {
        int progress=0;
        for (uint32_t i=0;i<count;++i) {
            if (done[i] || indeg[i]) continue;
            done[i]=1; ++rendered; progress=1;
            Ngs2Object *v=order[i]; Mix *m=v->mix;
            uint32_t ch=CH;
            if (is_sampler(v)) {
                ch=m->channels ? (m->channels<CH ? m->channels : CH) : 1;
                sampler_render(m,tmp,frames);
            } else if (passes_input(v) && m->in) {
                for (int c=0;c<CH;++c) memcpy(tmp[c],m->in[c],sizeof(float)*frames);
            } else {
                for (int c=0;c<CH;++c) memset(tmp[c],0,sizeof(float)*frames);
            }
            float peak=0;
            const float (*level)[GRAIN]=m->in && !is_sampler(v) ? (const float (*)[GRAIN])m->in : (const float (*)[GRAIN])tmp;
            for (uint32_t c=0;c<ch;++c) for (uint32_t k=0;k<frames;++k) { const float a=fabsf(level[c][k]); if (a>peak) peak=a; }
            m->peak=peak;
            for (int p=0;p<PORTS;++p) {
                Ngs2Object *d=m->port[p].dest;
                if (!d || d==v) continue;
                mix_port(m,&m->port[p],tmp,ch,frames);
                for (uint32_t j=0;j<count;++j) if (order[j]==d) { if (indeg[j]) --indeg[j]; break; }
            }
        }
        if (!progress) break; /* a cycle: the rest stays silent */
    }
}

/* sceNgs2SystemRender(handle, bufferInfo[], count): one grain of the mastering voices' input into
 * each buffer (float or 16-bit, up to 8 channels). */
static ABI int32_t system_render(void *handle, const RenderBufferInfo *buffers, uint32_t count) {
    Ngs2Object *s=as(handle,KIND_SYSTEM);
    if (!s) return NGS2_ERROR_INVALID_HANDLE;
    if (count && !buffers) return NGS2_ERROR_INVALID_BUFFER_ADDRESS;
    static int told;
    if (trace_level() && !told++) for (uint32_t i=0;i<count;++i)
        printf("Ngs2: render buffer %u: %llu bytes, waveform 0x%x, %u channels\n",i,(unsigned long long)buffers[i].size,buffers[i].waveform,buffers[i].channels);
    pthread_mutex_lock(&s->lock); /* the game's parameter changes land between grains */
    pthread_mutex_lock(&s->render);
    for (uint32_t b=0;b<count;++b) {
        const RenderBufferInfo *rb=&buffers[b];
        if (!rb->buffer) continue;
        const uint32_t ch=rb->channels ? rb->channels : 2, bytes=rb->waveform==WAVE_I16L ? 2 : 4;
        uint32_t frames=(uint32_t)(rb->size/(ch*bytes));
        if (frames>GRAIN) frames=GRAIN;
        if (b==0) render_mix(s,frames);
        memset(rb->buffer,0,rb->size);
        for (uint32_t r=0;r<s->racks;++r) {
            Ngs2Object *rack=s->rack[r];
            if ((rack->rack_id&0xf000)!=RACK_MASTERING) continue;
            for (uint32_t i=0;i<rack->voices;++i) {
                Mix *m=rack->voice[i] && rack->voice[i]->mix ? rack->voice[i]->mix : NULL;
                if (!m || !m->in) continue;
                for (uint32_t c=0;c<ch && c<CH;++c)
                    for (uint32_t k=0;k<frames;++k) {
                        const float v=m->in[c][k];
                        if (bytes==4) ((float *)rb->buffer)[k*ch+c]+=v;
                        else {
                            int16_t *o=(int16_t *)rb->buffer+k*ch+c;
                            float x=*o+v*32767.0f; *o=(int16_t)(x>32767.0f ? 32767 : x<-32768.0f ? -32768 : x);
                        }
                    }
            }
        }
    }
    pthread_mutex_unlock(&s->render);
    pthread_mutex_unlock(&s->lock);
    return 0;
}

/* sceNgs2RackCreate(system, rackId, option, bufferInfo, *handle): option {16-byte header, char
 * name[16], u32 flags, u32 maxGrainSamples, u32 maxVoices, ...}. */
static ABI int32_t rack_create(void *system, uint32_t rack_id, const uint8_t *option, const BufferInfo *info, void **out) {
    Ngs2Object *s=as(system,KIND_SYSTEM);
    if (!s) return NGS2_ERROR_INVALID_HANDLE;
    if (!out) return NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (!info || !info->buffer) return NGS2_ERROR_INVALID_BUFFER_ADDRESS;
    if (s->racks>=MAX_RACKS) return NGS2_ERROR_INVALID_BUFFER_ADDRESS;
    uint32_t voices=1;
    if (option) { memcpy(&voices,option+32,4); if (!voices || voices>MAX_VOICES) voices=voices ? MAX_VOICES : 1; }
    Ngs2Object *r=new_object(KIND_RACK,s);
    if (!r) return NGS2_ERROR_INVALID_BUFFER_ADDRESS;
    r->rack_id=rack_id; r->voices=voices;
    if (option) memcpy(r->name,option+8,16);
    r->voice=calloc(voices,sizeof(*r->voice));
    const int sampler=(rack_id&0xf000)==RACK_SAMPLER;
    for (uint32_t i=0;r->voice && i<voices;++i) {
        Ngs2Object *v=r->voice[i]=new_object(KIND_VOICE,r);
        if (!v) continue;
        v->index=i;
        v->mix=calloc(1,sizeof(Mix));
        if (!v->mix) continue;
        reset_ports(v->mix);
        v->mix->pitch=1.0f;
        if (!sampler) v->mix->in=calloc(CH,sizeof(float)*GRAIN);
    }
    pthread_mutex_lock(&s->render);
    s->rack[s->racks++]=r;
    pthread_mutex_unlock(&s->render);
    *out=r;
    if (runtime_verbose() || trace_level()) printf("Runtime: Ngs2 rack 0x%x \"%s\" created with %u voices\n",rack_id,r->name,voices);
    return 0;
}
/* sceNgs2RackDestroy(rack, *bufferInfo). */
static ABI int32_t rack_destroy(void *handle, BufferInfo *info) {
    Ngs2Object *r=as(handle,KIND_RACK);
    if (!r) return NGS2_ERROR_INVALID_HANDLE;
    if (info) memset(info,0,sizeof(*info));
    Ngs2Object *s=r->owner;
    pthread_mutex_lock(&s->render);
    for (uint32_t i=0;i<s->racks;++i) if (s->rack[i]==r) { memmove(s->rack+i,s->rack+i+1,sizeof(*s->rack)*(s->racks-i-1)); --s->racks; break; }
    /* Ports elsewhere that lead into this rack's voices lead nowhere now. */
    for (uint32_t q=0;q<s->racks;++q)
        for (uint32_t i=0;i<s->rack[q]->voices;++i) {
            Mix *m=s->rack[q]->voice[i] ? s->rack[q]->voice[i]->mix : NULL;
            for (int p=0;m && p<PORTS;++p) if (m->port[p].dest && m->port[p].dest->owner==r) m->port[p].dest=NULL;
        }
    for (uint32_t i=0;r->voice && i<r->voices;++i) if (r->voice[i]) {
        Mix *m=r->voice[i]->mix;
        if (m) { at9_release(m); free(m->pcm); free(m->in); free(m); }
        free_object(r->voice[i]);
    }
    pthread_mutex_unlock(&s->render);
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

/* One parameter block. */
static void voice_param(Ngs2Object *v, uint32_t id, const uint8_t *p, uint32_t size) {
    Mix *m=v->mix;
    uint32_t a=0, b=0; float f=0;
    if (size>=12) memcpy(&a,p+8,4);
    if (size>=16) { memcpy(&b,p+12,4); memcpy(&f,p+12,4); }
    switch (id) {
    case 1: if (size>=24 && a<MATRICES) { /* matrix levels */
        const float *levels; memcpy(&levels,p+16,8);
        const uint32_t n=b<CH*CH ? b : CH*CH;
        m->matrix[a].count=levels ? n : 0;
        if (levels) memcpy(m->matrix[a].level,levels,sizeof(float)*n);
    } break;
    case 2: if (a<PORTS) m->port[a].volume=f; break;
    case 3: if (a<PORTS) m->port[a].matrix=(int32_t)b; break;
    case 5: if (size>=24 && a<PORTS) { /* patch */
        void *dest; memcpy(&dest,p+16,8);
        m->port[a].dest=as(dest,KIND_VOICE); m->port[a].input=b;
    } break;
    case 6: /* kick event */
        if (a==0) { m->playing=1; m->paused=0; m->releasing=0; m->primed=0; m->fed=0; m->decoded_samples=m->decoded_bytes=0; m->block_started=0; }
        else if (a==1) { if (m->playing) m->releasing=RELEASE_FRAMES; }
        else if (a==2) stop_voice(m);
        else if (a==3) { stop_voice(m); m->blocks=0; m->block_started=0; }
        else if (a==4) m->paused=1;
        else if (a==5) m->paused=0;
        break;
    case 0x10000000: if (size>=24) { /* sampler setup */
        memcpy(&m->wave,p+8,4); memcpy(&m->channels,p+12,4); memcpy(&m->rate,p+16,4); memcpy(&m->config,p+20,4);
        if (m->channels>CH) m->channels=CH;
        stop_voice(m); m->blocks=0; m->block_started=0;
        if (m->wave!=WAVE_I16L && m->wave!=WAVE_F32L && m->wave!=WAVE_AT9) {
            static int told;
            if (!told++) printf("Runtime: Ngs2 waveform type 0x%x not supported; that voice stays silent\n",m->wave);
        }
    } break;
    case 0x10000001: if (size>=32) { /* add waveform blocks */
        const uint8_t *data, *blocks; uint32_t n; memcpy(&data,p+8,8); memcpy(&n,p+20,4); memcpy(&blocks,p+24,8);
        for (uint32_t i=0;data && blocks && i<n && m->blocks<BLOCKS;++i) {
            uint32_t w[6]; uint64_t user; memcpy(w,blocks+i*32,24); memcpy(&user,blocks+i*32+24,8);
            m->block[m->blocks++]=(Block){data,w[0],w[1],w[3],w[4],(int32_t)w[2],user};
            if (trace_level()) printf("Ngs2:   block: data %p offset %u size %u repeats %d skip %u samples %u user %llu\n",(const void *)data,w[0],w[1],(int)w[2],w[3],w[4],(unsigned long long)user);
        }
    } break;
    case 0x10000002: if (size>=24) { /* replace waveform address */
        const uint8_t *from, *to; memcpy(&from,p+8,8); memcpy(&to,p+16,8);
        for (uint32_t i=0;i<m->blocks;++i) if (m->block[i].data==from) m->block[i].data=to;
    } break;
    case 0x10000004: if (m->blocks) m->block[0].repeats=0; break; /* exit loop */
    case 0x10000005: memcpy(&m->pitch,p+8,4); if (!(m->pitch>0.0f) || m->pitch>16.0f) m->pitch=1.0f; break;
    default: break; /* envelopes, filters, effects: not modelled */
    }
}

/* sceNgs2VoiceControl(voice, paramList): each block of the list in order. */
static ABI int32_t voice_control(void *handle, const void *params) {
    Ngs2Object *v=as(handle,KIND_VOICE);
    if (!v) return NGS2_ERROR_INVALID_HANDLE;
    if (!params || !v->mix) return 0;
    Ngs2Object *s=system_of(v);
    pthread_mutex_lock(&s->render);
    const uint8_t *p=params;
    for (int n=0;n<64;++n) {
        uint16_t size; int16_t next; uint32_t id;
        memcpy(&size,p,2); memcpy(&next,p+2,2); memcpy(&id,p+4,4);
        if (trace_level()) {
            printf("Ngs2: \"%s\" voice %u param 0x%08x size %u:",v->owner->name,v->index,id,size);
            for (unsigned i=8;i<size && i<8+64;i+=4) { uint32_t w; memcpy(&w,p+i,4); printf(" %08x",w); }
            putchar('\n');
        }
        voice_param(v,id,p,size);
        if (!next) break;
        p+=next;
    }
    pthread_mutex_unlock(&s->render);
    return 0;
}
static uint32_t state_flags(const Mix *m, int sampler) {
    if (!sampler) return FLAG_IN_USE;
    if (!m->playing) return m->blocks ? FLAG_IN_USE|FLAG_STOPPED : 0;
    return FLAG_IN_USE|(m->paused ? FLAG_PAUSED : FLAG_PLAYING)|(m->blocks ? 0 : FLAG_EMPTY);
}
/* sceNgs2VoiceGetState(voice, *state, size): {flags, envelope height, peak height, reserved}, and
 * for samplers {decoded samples, decoded bytes, the current block's userData and data}. */
static ABI int32_t voice_state(void *handle, void *state, uint64_t size) {
    Ngs2Object *v=as(handle,KIND_VOICE);
    if (!v) return NGS2_ERROR_INVALID_HANDLE;
    if (!state) return NGS2_ERROR_INVALID_OUT_ADDRESS;
    memset(state,0,size);
    Mix *m=v->mix;
    if (!m) return 0;
    Ngs2Object *s=system_of(v);
    pthread_mutex_lock(&s->render);
    uint8_t out[48]={0};
    const uint32_t flags=state_flags(m,is_sampler(v));
    const float envelope=m->playing || !is_sampler(v) ? 1.0f : 0.0f, peak=m->peak;
    memcpy(out,&flags,4); memcpy(out+4,&envelope,4); memcpy(out+8,&peak,4);
    if (is_sampler(v)) {
        const uint64_t user=m->blocks ? m->block[0].user : 0;
        const void *data=m->blocks ? m->block[0].data : NULL;
        memcpy(out+16,&m->decoded_samples,8); memcpy(out+24,&m->decoded_bytes,8);
        memcpy(out+32,&user,8); memcpy(out+40,&data,8);
    }
    pthread_mutex_unlock(&s->render);
    memcpy(state,out,size<sizeof(out) ? size : sizeof(out));
    if (trace_level()>1) printf("Ngs2: \"%s\" voice %u state (%llu bytes) flags 0x%x\n",v->owner->name,v->index,(unsigned long long)size,flags);
    return 0;
}
/* sceNgs2VoiceGetStateFlags(voice, *flags). */
static ABI int32_t voice_state_flags(void *handle, uint32_t *flags) {
    Ngs2Object *v=as(handle,KIND_VOICE);
    if (!v) return NGS2_ERROR_INVALID_HANDLE;
    if (!flags) return NGS2_ERROR_INVALID_OUT_ADDRESS;
    *flags=v->mix ? state_flags(v->mix,is_sampler(v)) : 0;
    if (trace_level()>1) printf("Ngs2: \"%s\" voice %u flags 0x%x\n",v->owner->name,v->index,*flags);
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
