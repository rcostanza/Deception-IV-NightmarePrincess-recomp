/* Kernel event flags: a 64-bit pattern threads wait on (shadPS4 kernel/threads/event_flag.cpp).
 * The handle is the address of the flag's record, below 1 TiB as on the PS4; the registry turns
 * an unknown handle into ESRCH. Priority-ordered queues are served FIFO, as semaphores are:
 * host threads have no PS4 priority scheduler. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define ATTR_TH_FIFO 0x01
#define ATTR_TH_PRIO 0x02
#define ATTR_SINGLE 0x10
#define ATTR_MULTI 0x20
#define WAIT_AND 0x01
#define WAIT_OR 0x02
#define CLEAR_ALL 0x10
#define CLEAR_PAT 0x20
typedef struct Waiter {
    uint64_t bits, result;
    uint32_t mode;
    int32_t status; /* 0 waiting, else the wait's result (OK is reported as 1) */
    pthread_cond_t event;
    struct Waiter *next;
} Waiter;
typedef struct Flag {
    uint64_t bits;
    uint32_t attr;
    unsigned active;
    int deleted;
    Waiter *first;
    struct Flag *next;
    char name[32];
} Flag;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static Flag *registry;
static size_t created, deleted, waits, timed_out, sets;
static Flag *find(const Flag *handle) {
    for (Flag *f=registry;f;f=f->next) if (f==handle) return f;
    return NULL;
}
static void host_check(int e) {
    if (e) { fprintf(stderr,"STOP: event flag host error %d\n",e); exit(21); }
}
/* Same clock as semaphore waits: winpthreads condition variables accept only CLOCK_REALTIME. */
#ifdef _WIN32
#define WAIT_CLOCK CLOCK_REALTIME
#else
#define WAIT_CLOCK CLOCK_MONOTONIC
#endif
static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(WAIT_CLOCK,&t)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static int met(const Flag *f, uint64_t bits, uint32_t mode) {
    return (mode&0xf)==WAIT_AND ? (f->bits&bits)==bits : (f->bits&bits)!=0;
}
static void clear(Flag *f, uint64_t bits, uint32_t mode) {
    if ((mode&0xf0)==CLEAR_ALL) f->bits=0;
    else if ((mode&0xf0)==CLEAR_PAT) f->bits&=~bits;
}
static int valid_mode(uint32_t mode) {
    uint32_t wait=mode&0xf, how=mode&0xf0;
    return !(mode&~0xffu) && (wait==WAIT_AND || wait==WAIT_OR) && (how==0 || how==CLEAR_ALL || how==CLEAR_PAT);
}
/* sceKernelCreateEventFlag(flag, name, attr, pattern, options): attr is a queue order (FIFO or
 * priority) and SINGLE (one waiter at a time, the default) or MULTI; options must be NULL. */
static ABI int32_t evf_create(Flag **out, const char *name, uint32_t attr, uint64_t pattern, const void *options) {
    if (!out || !name || options || attr>(ATTR_MULTI|ATTR_TH_PRIO) || (attr&0xf)==3 || (attr&0xf0)==0x30) return ERR(22);
    if (strlen(name)>=32) return ERR(63);
    Flag *f=runtime_low_alloc(sizeof(*f));
    if (!f) return ERR(12);
    f->bits=pattern; f->attr=attr;
    snprintf(f->name,sizeof(f->name),"%s",name);
    pthread_mutex_lock(&lock);
    f->next=registry; registry=f; ++created;
    pthread_mutex_unlock(&lock);
    *out=f;
    return 0;
}
static void wake_all(Flag *f, int32_t status, uint64_t result) {
    while (f->first) {
        Waiter *w=f->first; f->first=w->next;
        w->status=status; w->result=result;
        host_check(pthread_cond_signal(&w->event));
    }
}
/* sceKernelDeleteEventFlag: waiting threads return EACCES; the record is freed after the last. */
static ABI int32_t evf_delete(Flag *handle) {
    pthread_mutex_lock(&lock);
    Flag **p=&registry;
    while (*p && *p!=handle) p=&(*p)->next;
    if (!*p) { pthread_mutex_unlock(&lock); return ERR(3); }
    Flag *f=*p; *p=f->next; f->deleted=1; ++deleted;
    wake_all(f,ERR(13),f->bits);
    if (!f->active) runtime_low_free(f);
    pthread_mutex_unlock(&lock);
    return 0;
}
/* sceKernelWaitEventFlag(flag, bits, mode, result, timeout): OK once the pattern meets the mode,
 * with the pattern from before the clear in *result. A timeout writes the remaining microseconds
 * (0 when it expired; a zero timeout fails at once). SINGLE flags allow one waiter (EPERM). */
static int32_t evf_wait_for(Flag *handle, uint64_t bits, uint32_t mode, uint64_t *result, uint32_t *timeout) {
    if (!bits || !valid_mode(mode)) return ERR(22);
    pthread_mutex_lock(&lock);
    Flag *f=find(handle);
    int32_t r=0;
    if (!f) r=ERR(3);
    else if ((f->attr&ATTR_MULTI)==0 && f->first) r=ERR(1);
    else if (met(f,bits,mode)) { if (result) *result=f->bits; clear(f,bits,mode); }
    else if (timeout && !*timeout) { if (result) *result=f->bits; r=ERR(60); ++timed_out; }
    else {
        Waiter w={.bits=bits,.mode=mode};
        pthread_condattr_t attr;
        host_check(pthread_condattr_init(&attr));
        host_check(pthread_condattr_setclock(&attr,WAIT_CLOCK));
        host_check(pthread_cond_init(&w.event,&attr));
        host_check(pthread_condattr_destroy(&attr));
        Waiter **tail=&f->first;
        while (*tail) tail=&(*tail)->next;
        *tail=&w; ++f->active; ++waits;
        uint64_t deadline=timeout ? now_ns()+(uint64_t)*timeout*1000 : 0;
        struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
        while (!w.status) {
            int e=timeout ? pthread_cond_timedwait(&w.event,&lock,&end) : pthread_cond_wait(&w.event,&lock);
            if (e==ETIMEDOUT && !w.status) {
                Waiter **p=&f->first;
                while (*p!=&w) p=&(*p)->next;
                *p=w.next; w.status=ERR(60); w.result=f->bits; ++timed_out;
            } else if (e!=ETIMEDOUT) host_check(e);
        }
        r=w.status==1 ? 0 : w.status;
        if (result) *result=w.result;
        if (timeout) {
            uint64_t now=now_ns();
            *timeout=r==ERR(60) || now>=deadline ? 0 : (uint32_t)((deadline-now)/1000);
        }
        host_check(pthread_cond_destroy(&w.event));
        if (!--f->active && f->deleted) runtime_low_free(f);
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t evf_wait(Flag *handle, uint64_t bits, uint32_t mode, uint64_t *result, uint32_t *timeout) {
    const uint64_t wait_start=runtime_wait_begin();
    const int32_t r=evf_wait_for(handle,bits,mode,result,timeout);
    runtime_wait_end("event flag wait",wait_start,"bits",(long long)bits);
    return r;
}
/* sceKernelSetEventFlag: ORs bits in, then releases the waiters it satisfies in queue order,
 * each one's clear applying before the next is checked. */
static ABI int32_t evf_set(Flag *handle, uint64_t bits) {
    pthread_mutex_lock(&lock);
    Flag *f=find(handle);
    if (!f) { pthread_mutex_unlock(&lock); return ERR(3); }
    f->bits|=bits; ++sets;
    for (Waiter **p=&f->first;*p;) {
        Waiter *w=*p;
        if (!met(f,w->bits,w->mode)) { p=&w->next; continue; }
        w->result=f->bits; clear(f,w->bits,w->mode);
        *p=w->next; w->status=1;
        host_check(pthread_cond_signal(&w->event));
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
/* sceKernelPollEventFlag(flag, bits, mode, result): the wait without blocking; EBUSY when the
 * pattern does not meet the mode (*result still gets the pattern). */
static ABI int32_t evf_poll(Flag *handle, uint64_t bits, uint32_t mode, uint64_t *result) {
    if (!bits || !valid_mode(mode)) return ERR(22);
    pthread_mutex_lock(&lock);
    Flag *f=find(handle);
    int32_t r=0;
    if (!f) r=ERR(3);
    else if ((f->attr&ATTR_MULTI)==0 && f->first) r=ERR(1);
    else {
        if (result) *result=f->bits;
        if (met(f,bits,mode)) clear(f,bits,mode); else r=ERR(16);
    }
    pthread_mutex_unlock(&lock);
    return r;
}
/* sceKernelClearEventFlag(flag, bits): ANDs the pattern with bits (the bits to keep). */
static ABI int32_t evf_clear(Flag *handle, uint64_t bits) {
    pthread_mutex_lock(&lock);
    Flag *f=find(handle);
    if (f) f->bits&=bits;
    pthread_mutex_unlock(&lock);
    return f ? 0 : ERR(3);
}
static const RuntimeExport exports[]={
    {"sceKernelPollEventFlag",evf_poll}, {"sceKernelClearEventFlag",evf_clear},
    {"sceKernelCreateEventFlag",evf_create}, {"sceKernelDeleteEventFlag",evf_delete},
    {"sceKernelWaitEventFlag",evf_wait},
    {"sceKernelSetEventFlag",evf_set},
};
uintptr_t runtime_eventflag_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_eventflag_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: event flags created=%zu, deleted=%zu, blocking waits=%zu, sets=%zu, timeouts=%zu\n",
           created,deleted,waits,sets,timed_out);
    pthread_mutex_unlock(&lock);
}
