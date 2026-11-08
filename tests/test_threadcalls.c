/* Thread creation, scheduling and stack calls (src/runtime_thread.c) against the contracts
 * shadPS4 follows. */
#include "../src/runtime_thread.c"
#include <assert.h>

void *runtime_low_alloc(size_t size) { return calloc(1, size); }
void runtime_low_free(void *pointer) { free(pointer); }
/* Linux guest stacks: none low here, so pthreads places them. */
void *runtime_low_map(size_t size, int prot) { (void)size; (void)prot; return NULL; }
void runtime_thread_keys_cleanup(void) {}
int runtime_verbose(void) { return 0; }

static int32_t own_tid;
static void *ABI inspector(void *arg) {
    (void)arg;
    own_tid = thread_get_id();
    return NULL;
}
/* pthread_exit: the value reaches the joiner. */
static void *ABI leaver(void *arg) {
    thread_exit(arg);
}

int main(void) {
    /* A thread that has finished. */
    GuestThread *t = NULL;
    assert(thread_create(&t, NULL, inspector, NULL, "finished") == 0);
    assert(thread_join(t, NULL) == 0);
    assert(own_tid > 0 && own_tid != thread_get_id());

    /* Scheduling parameters of a live thread; a finished one is ESRCH. */
    GuestThread *self = runtime_thread_current();
    int param = 300, prio = 0;
    assert(thread_set_sched(self, 3, &param) == 0 && self->attr.policy == 3 && self->attr.prio == 300);
    assert(thread_set_sched(self, 3, NULL) == ERR(22) && thread_set_sched(t, 1, &param) == ERR(3));
    assert(thread_set_prio(self, 700) == 0 && thread_get_prio(self, &prio) == 0 && prio == 700);
    assert(thread_get_prio(self, NULL) == ERR(22));

    /* pthread_exit's value, and a detached thread that cannot be joined. */
    void *value = NULL;
    assert(thread_create(&t, NULL, leaver, (void *)0x1234, "leaver") == 0);
    assert(thread_join(t, &value) == 0 && value == (void *)0x1234);
    ThreadAttr *detached = NULL;
    assert(attr_init(&detached) == 0 && attr_set_detach(&detached, 1) == 0);
    assert(thread_create(&t, &detached, inspector, NULL, "detached") == 0);
    const int32_t joined = thread_join(t, NULL); /* EINVAL, or ESRCH once its record is gone */
    assert(joined == ERR(22) || joined == ERR(3));
    assert(attr_destroy(&detached) == 0 && thread_join(self, NULL) == ERR(3)); /* the host-owned main thread */

    /* scePthreadAttrGet reports the stack the thread runs on: it holds this frame's locals. */
    ThreadAttr *attr = NULL;
    assert(attr_init(&attr) == 0 && attr_get(self, &attr) == 0);
    assert(attr->stack_address && attr->stack);
    char local = 0;
    assert((char *)attr->stack_address <= &local && &local < (char *)attr->stack_address + attr->stack);
    assert(attr_destroy(&attr) == 0);
    puts("thread calls test: ok");
    return 0;
}
