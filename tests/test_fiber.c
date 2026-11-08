/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceFiber (src/runtime_fiber.c): Run from a thread, Switch between fibers, ReturnToThread,
 * the arguments each hands over, and the calls refused outside or inside a fiber. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/runtime_fiber.c"

void *runtime_low_alloc(size_t size) {
    void *p=NULL;
#ifdef _WIN32
    p=_aligned_malloc(size,16);
#else
    if (posix_memalign(&p,16,size)) p=NULL;
#endif
    if (p) memset(p,0,size);
    return p;
}
void runtime_low_free(void *pointer) {
#ifdef _WIN32
    _aligned_free(pointer);
#else
    free(pointer);
#endif
}
int runtime_verbose(void) { return 0; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static Fiber a, b;
static _Alignas(16) unsigned char context_a[0x2800], context_b[0x2800];
static int log_at;
static int trace[32];
static double carried=1.5;

/* As the game's job loop: work, then yield to the thread; the second time, switch to b first. */
static ABI void entry_a(uint64_t init, uint64_t run) {
    assert(init==11 && run==100);
    Fiber *self=NULL;
    assert(!fiber_get_self(&self) && self==&a);
    assert(fiber_run(&b,0,NULL)==FIBER_ERROR_PERMISSION);
    volatile double kept=carried*2; /* lives across switches on this stack */
    trace[log_at++]=1;
    uint64_t next=0;
    assert(!fiber_return_to_thread(200,&next));
    assert(next==101 && kept==3.0);
    trace[log_at++]=2;
    uint64_t back=0;
    assert(!fiber_switch(&b,300,&back));
    assert(back==301);
    trace[log_at++]=4;
    assert(!fiber_return_to_thread(202,&next));
    for (;;) fiber_return_to_thread(0,&next);
}
static ABI void entry_b(uint64_t init, uint64_t run) {
    assert(init==22 && run==300);
    trace[log_at++]=3;
    uint64_t next=0;
    assert(!fiber_switch(&a,301,&next));
    for (;;) fiber_return_to_thread(0,&next);
}

int main(void) {
    assert(fiber_initialize(&a,"job a",entry_a,11,context_a,sizeof(context_a),NULL,0x4000000)==0);
    assert(fiber_initialize(&b,"job b",entry_b,22,context_b,sizeof(context_b),NULL,0x4000000)==0);
    assert(fiber_initialize(&b,"x",entry_b,0,context_b+8,512,NULL,0)==FIBER_ERROR_ALIGNMENT);
    assert(fiber_initialize(&b,"x",entry_b,0,context_b,100,NULL,0)==FIBER_ERROR_RANGE);
    assert(fiber_initialize(&b,"job b",entry_b,22,context_b,sizeof(context_b),NULL,0x4000000)==0);
    Fiber *self=NULL;
    assert(fiber_get_self(&self)==FIBER_ERROR_PERMISSION);
    assert(fiber_switch(&a,0,NULL)==FIBER_ERROR_PERMISSION);
    assert(fiber_return_to_thread(0,NULL)==FIBER_ERROR_PERMISSION);
    uint64_t back=0;
    assert(!fiber_run(&a,100,&back) && back==200);
    assert(!fiber_run(&a,101,&back) && back==202);
    assert(log_at==4 && trace[0]==1 && trace[1]==2 && trace[2]==3 && trace[3]==4);
    assert(fiber_get_self(&self)==FIBER_ERROR_PERMISSION);
    assert(!fiber_finalize(&a) && !fiber_finalize(&b)); /* both suspended where they yielded */
    assert(fiber_run(&a,0,NULL)==FIBER_ERROR_INVALID);
    puts("fiber test: ok");
    return 0;
}
