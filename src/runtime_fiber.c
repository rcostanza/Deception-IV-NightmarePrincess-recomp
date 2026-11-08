/* libSceFiber: user-mode fibers on the calling thread (shadPS4 fiber.cpp has the contracts).
 * Deception runs its jobs as coroutines: one fiber per job, resumed with sceFiberRun from a thread
 * (sceFiberSwitch from inside another fiber), and the job yields with sceFiberReturnToThread.
 *
 *   sceFiberRun(fiber, argOnRunTo, *argOnReturn): from a thread, not a fiber: runs `fiber` until
 *     some fiber of this thread calls sceFiberReturnToThread(argOnReturn, ...), whose argument
 *     Run then returns.
 *   sceFiberSwitch(fiber, argOnRunTo, *argOnRun): from a fiber: suspends it and runs `fiber`.
 *   A fiber's first run calls entry(argOnInitialize, argOnRunTo); a suspended one resumes with
 *   argOnRunTo in the *argOnRun of the call that suspended it.
 *
 * Each fiber runs on a stack of its own, at least 256 KiB, not the game's context buffer: host
 * runtime code also runs on that stack when the fiber calls into it, and the game's buffers are
 * sized for its own code (10 KiB). Nothing the game imports reads the buffer. The 128-byte
 * OrbisFiber is opaque to the game and holds the state below. On Windows the TEB's stack bounds
 * follow the stack in use, as Windows' own fibers do. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define FIBER_ERROR_NULL ((int32_t)0x80590001)
#define FIBER_ERROR_ALIGNMENT ((int32_t)0x80590002)
#define FIBER_ERROR_RANGE ((int32_t)0x80590003)
#define FIBER_ERROR_INVALID ((int32_t)0x80590004)
#define FIBER_ERROR_PERMISSION ((int32_t)0x80590005)
#define FIBER_ERROR_STATE ((int32_t)0x80590006)
#define MAGIC_START 0xDEF1649Cu
#define MAGIC_END 0xB37592A0u
#define CONTEXT_MINIMUM 512
#define HOST_STACK (256u*1024u)

enum { IDLE, RUNNING, SUSPENDED, FINISHED };
typedef ABI void (*FiberEntry)(uint64_t arg_on_initialize, uint64_t arg_on_run);
typedef struct {
    uint32_t magic_start, state;
    FiberEntry entry;
    uint64_t arg_on_initialize;
    void *context;          /* the game's buffer, kept for its size only */
    uint64_t context_size;
    char name[32];
    unsigned char *stack;   /* ours, HOST_STACK or more */
    uint64_t stack_size;
    uint64_t rsp;           /* saved while suspended (or before the first run) */
    uint64_t arg_on_run;    /* handed to the fiber when it next runs */
    uint64_t *arg_out;      /* where the suspended call wants it */
    uint64_t reserved;
    uint32_t reserved2, magic_end;
} Fiber;
_Static_assert(sizeof(Fiber)==128,"OrbisFiber is 128 bytes");

/* The thread's side: its stack while one of its fibers runs, and the fiber running. */
typedef struct {
    uint64_t rsp;
    Fiber *current;
    uint64_t arg_on_return;
    uint64_t *arg_out;
#ifdef _WIN32
    void *stack_base, *stack_limit;
#endif
} ThreadSide;
static _Thread_local ThreadSide side;

/* fiber_swap(save, rsp): pushes the SysV callee-saved registers, MXCSR and the x87 control word,
 * stores the stack pointer in *save and continues on `rsp` (a stack fiber_swap saved, or one
 * fiber_stack built). fiber_start: a new fiber's first return, into fiber_main(fiber). */
ABI void fiber_swap(uint64_t *save, uint64_t rsp);
ABI void fiber_start(void);
__asm__(
    ".text\n"
    ".globl fiber_swap\n"
    "fiber_swap:\n"
    "    pushq %rbp\n    pushq %rbx\n    pushq %r12\n    pushq %r13\n    pushq %r14\n    pushq %r15\n"
    "    subq $8, %rsp\n"
    "    stmxcsr (%rsp)\n    fnstcw 4(%rsp)\n"
    "    movq %rsp, (%rdi)\n"
    "    movq %rsi, %rsp\n"
    "    ldmxcsr (%rsp)\n    fldcw 4(%rsp)\n"
    "    addq $8, %rsp\n"
    "    popq %r15\n    popq %r14\n    popq %r13\n    popq %r12\n    popq %rbx\n    popq %rbp\n"
    "    ret\n"
    ".globl fiber_start\n"
    "fiber_start:\n"
    "    movq %r12, %rdi\n"
    "    andq $-16, %rsp\n"
    "    call fiber_main\n"
    "    ud2\n");

static void stack_bounds(void *low, uint64_t size) {
#ifdef _WIN32
    NT_TIB *tib=(NT_TIB *)NtCurrentTeb();
    tib->StackLimit=low;
    tib->StackBase=(unsigned char *)low+size;
#else
    (void)low; (void)size;
#endif
}
static void thread_bounds(void) {
#ifdef _WIN32
    NT_TIB *tib=(NT_TIB *)NtCurrentTeb();
    tib->StackLimit=side.stack_limit;
    tib->StackBase=side.stack_base;
#endif
}

/* A fresh fiber's stack: fiber_swap's frame (MXCSR and control word as the caller's, r12 = the
 * fiber) returning into fiber_start. */
static void fiber_stack(Fiber *f) {
    uint64_t *top=(uint64_t *)(f->stack+f->stack_size);
    uint64_t *sp=top-8; /* 8 slots: control, r15..r12, rbx, rbp, return */
    uint32_t control[2];
    __asm__ volatile("stmxcsr %0\n\tfnstcw %1" : "=m"(control[0]), "=m"(control[1]));
    memcpy(&sp[0],control,8);
    sp[1]=0; sp[2]=0; sp[3]=0;           /* r15 r14 r13 */
    sp[4]=(uint64_t)(uintptr_t)f;        /* r12: the fiber */
    sp[5]=0; sp[6]=0;                    /* rbx rbp */
    sp[7]=(uint64_t)(uintptr_t)fiber_start;
    f->rsp=(uint64_t)(uintptr_t)sp;
}

/* A fiber's whole life: its entry must not return (the SDK treats that as fatal). */
ABI __attribute__((noreturn, used)) void fiber_main(Fiber *f) {
    f->entry(f->arg_on_initialize,f->arg_on_run);
    fprintf(stderr,"STOP: fiber %s returned from its entry function\n",f->name);
    fflush(NULL);
    exit(21);
}

static int valid(const Fiber *f) { return f->magic_start==MAGIC_START && f->magic_end==MAGIC_END; }

/* Continues `to` (its first run or a resumption), saving the running stack in *save. */
static void enter(Fiber *to, uint64_t arg, uint64_t *save) {
    to->arg_on_run=arg;
    if (to->arg_out) { *to->arg_out=arg; to->arg_out=NULL; }
    to->state=RUNNING;
    side.current=to;
    stack_bounds(to->stack,to->stack_size);
    fiber_swap(save,to->rsp);
}

/* _sceFiberInitializeImpl(fiber, name, entry, argOnInitialize, addrContext, sizeContext,
 * optParam, buildVersion): fiber 8-byte aligned; a context buffer, when given, 16-byte aligned
 * and at least 512 bytes, its size a multiple of 16. */
static ABI int32_t fiber_initialize(Fiber *f, const char *name, FiberEntry entry, uint64_t arg,
                                    void *context, uint64_t context_size, const void *option, uint32_t build) {
    (void)option; (void)build;
    if (!f || !name || !entry) return FIBER_ERROR_NULL;
    if (((uintptr_t)f & 7) || ((uintptr_t)context & 15)) return FIBER_ERROR_ALIGNMENT;
    if (context && (context_size<CONTEXT_MINIMUM || (context_size & 15))) return FIBER_ERROR_RANGE;
    if (!context && context_size) return FIBER_ERROR_INVALID;
    uint64_t size=context_size>HOST_STACK ? (context_size+15)&~UINT64_C(15) : HOST_STACK;
    unsigned char *stack=runtime_low_alloc(size);
    if (!stack) return FIBER_ERROR_RANGE;
    memset(f,0,sizeof(*f));
    f->magic_start=MAGIC_START; f->magic_end=MAGIC_END;
    f->entry=entry; f->arg_on_initialize=arg;
    f->context=context; f->context_size=context_size;
    snprintf(f->name,sizeof(f->name),"%s",name);
    f->stack=stack; f->stack_size=size;
    fiber_stack(f);
    f->state=IDLE;
    if (runtime_verbose()) printf("Runtime: fiber %s initialized (%llu-byte context, %llu-byte stack)\n",
                                  f->name,(unsigned long long)context_size,(unsigned long long)size);
    return 0;
}

/* sceFiberFinalize(fiber): any fiber that is not running (never run, or suspended: the game's job
 * destructor finalizes its fiber where it yielded); its stack is freed. */
static ABI int32_t fiber_finalize(Fiber *f) {
    if (!f) return FIBER_ERROR_NULL;
    if ((uintptr_t)f & 7) return FIBER_ERROR_ALIGNMENT;
    if (!valid(f)) return FIBER_ERROR_INVALID;
    if (f->state==RUNNING) return FIBER_ERROR_STATE;
    runtime_low_free(f->stack);
    memset(f,0,sizeof(*f));
    return 0;
}

/* sceFiberRun(fiber, argOnRunTo, argOnReturn): see the top. PERMISSION from inside a fiber. */
static ABI int32_t fiber_run(Fiber *f, uint64_t arg, uint64_t *arg_on_return) {
    if (!f) return FIBER_ERROR_NULL;
    if ((uintptr_t)f & 7) return FIBER_ERROR_ALIGNMENT;
    if (!valid(f)) return FIBER_ERROR_INVALID;
    if (side.current) return FIBER_ERROR_PERMISSION;
    if (f->state==RUNNING) return FIBER_ERROR_STATE;
#ifdef _WIN32
    NT_TIB *tib=(NT_TIB *)NtCurrentTeb();
    side.stack_base=tib->StackBase; side.stack_limit=tib->StackLimit;
#endif
    enter(f,arg,&side.rsp);
    /* Back on the thread: a fiber called sceFiberReturnToThread. */
    thread_bounds();
    if (arg_on_return) *arg_on_return=side.arg_on_return;
    return 0;
}

/* sceFiberSwitch(fiber, argOnRunTo, argOnRun): see the top. PERMISSION outside a fiber. */
static ABI int32_t fiber_switch(Fiber *f, uint64_t arg, uint64_t *arg_on_run) {
    if (!f) return FIBER_ERROR_NULL;
    if ((uintptr_t)f & 7) return FIBER_ERROR_ALIGNMENT;
    if (!valid(f)) return FIBER_ERROR_INVALID;
    Fiber *self=side.current;
    if (!self) return FIBER_ERROR_PERMISSION;
    if (f->state==RUNNING) return FIBER_ERROR_STATE;
    self->state=SUSPENDED;
    self->arg_out=arg_on_run;
    enter(f,arg,&self->rsp);
    return 0; /* resumed: enter() stored the argument through arg_out */
}

/* sceFiberGetSelf(fiber): the running fiber; PERMISSION on a thread outside its fibers. */
static ABI int32_t fiber_get_self(Fiber **out) {
    if (!out) return FIBER_ERROR_NULL;
    if (!side.current) return FIBER_ERROR_PERMISSION;
    *out=side.current;
    return 0;
}

/* sceFiberReturnToThread(argOnReturn, argOnRun): back to the sceFiberRun of this thread; the
 * fiber stays suspended until run again. PERMISSION outside a fiber. */
static ABI int32_t fiber_return_to_thread(uint64_t arg_on_return, uint64_t *arg_on_run) {
    Fiber *self=side.current;
    if (!self) return FIBER_ERROR_PERMISSION;
    self->state=SUSPENDED;
    self->arg_out=arg_on_run;
    side.arg_on_return=arg_on_return;
    side.current=NULL;
    fiber_swap(&self->rsp,side.rsp);
    return 0;
}

static const RuntimeExport exports[]={
    {"_sceFiberInitializeImpl",fiber_initialize}, {"sceFiberFinalize",fiber_finalize},
    {"sceFiberRun",fiber_run}, {"sceFiberSwitch",fiber_switch},
    {"sceFiberGetSelf",fiber_get_self}, {"sceFiberReturnToThread",fiber_return_to_thread},
};
uintptr_t runtime_fiber_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
