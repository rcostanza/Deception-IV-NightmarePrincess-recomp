/* Event flags (src/runtime_eventflag.c) against the PS4 contract shadPS4 follows. */
#include "../src/runtime_eventflag.c"
#include <assert.h>

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) { (void)table; (void)count; (void)nid; return 0; }
void *runtime_low_alloc(size_t size) { return calloc(1, size); }
void runtime_low_free(void *pointer) { free(pointer); }
uint64_t runtime_wait_begin(void) { return 0; }
void runtime_wait_end(const char *what, uint64_t start, const char *detail, long long arg) { (void)what; (void)start; (void)detail; (void)arg; }

static Flag *shared;
static uint64_t seen;
static int32_t waited;
static void *waiter_thread(void *arg) {
    (void)arg;
    waited = evf_wait(shared, 0x6, WAIT_AND | CLEAR_PAT, &seen, NULL);
    return NULL;
}
static void *blocked_thread(void *arg) {
    (void)arg;
    waited = evf_wait(shared, 0x100, WAIT_OR, &seen, NULL);
    return NULL;
}
static void wait_for_waiter(void) {
    for (;;) {
        pthread_mutex_lock(&lock);
        int queued = shared->first != NULL;
        pthread_mutex_unlock(&lock);
        if (queued) return;
        sched_yield();
    }
}

int main(void) {
    Flag *f = NULL;
    uint64_t result = 0;
    uint32_t timeout;
    assert(evf_create(&f, "test", 0x40, 0, NULL) == ERR(22));          /* unknown attribute */
    assert(evf_create(&f, "a name that is thirty-two chars!", 0, 0, NULL) == ERR(63));
    assert(evf_create(&f, "test", ATTR_MULTI | ATTR_TH_FIFO, 0x1, NULL) == 0);

    /* OR and AND modes and their clears; a zero timeout fails at once when unmet. */
    timeout = 0;
    assert(evf_wait(f, 0x3, WAIT_AND, &result, &timeout) == ERR(60) && result == 0x1);
    assert(evf_wait(f, 0x3, WAIT_OR, &result, NULL) == 0 && result == 0x1);
    assert(evf_set(f, 0x2) == 0);
    assert(evf_wait(f, 0x3, WAIT_AND | CLEAR_PAT, &result, NULL) == 0 && result == 0x3);
    timeout = 0;
    assert(evf_wait(f, 0x3, WAIT_OR, &result, &timeout) == ERR(60) && result == 0);
    assert(evf_wait(f, 0, WAIT_OR, &result, NULL) == ERR(22));
    assert(evf_wait(f, 1, 0x3, &result, NULL) == ERR(22));              /* bad wait mode */
    assert(evf_set(f, 0xf0) == 0);
    assert(evf_wait(f, 0x30, WAIT_OR | CLEAR_ALL, &result, NULL) == 0 && result == 0xf0);
    timeout = 0;
    assert(evf_wait(f, 0xff, WAIT_OR, &result, &timeout) == ERR(60) && result == 0);

    /* A timed wait that expires: ETIMEDOUT, no time left. */
    timeout = 2000;
    assert(evf_wait(f, 0x1, WAIT_OR, &result, &timeout) == ERR(60) && timeout == 0);

    /* A blocked waiter wakes when its pattern is complete, with the bits from before its clear. */
    shared = f;
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, waiter_thread, NULL));
    wait_for_waiter();
    assert(evf_set(f, 0x2) == 0);                                       /* not yet: needs 0x4 too */
    assert(evf_set(f, 0x5) == 0);
    pthread_join(thread, NULL);
    assert(waited == 0 && seen == 0x7);
    assert(evf_wait(f, 0x7, WAIT_OR, &result, NULL) == 0 && result == 0x1); /* 0x6 cleared by the waiter */

    /* SINGLE flags take one waiter at a time. */
    Flag *single = NULL;
    assert(evf_create(&single, "single", ATTR_SINGLE, 0, NULL) == 0);
    shared = single;
    assert(!pthread_create(&thread, NULL, blocked_thread, NULL));
    wait_for_waiter();
    assert(evf_wait(single, 0x1, WAIT_OR, &result, NULL) == ERR(1));
    /* Deleting wakes the waiter with EACCES; the handle is then unknown. */
    assert(evf_delete(single) == 0);
    pthread_join(thread, NULL);
    assert(waited == ERR(13));
    assert(evf_set(single, 1) == ERR(3) && evf_delete(single) == ERR(3));

    assert(evf_delete(f) == 0);
    puts("eventflag test: ok");
    return 0;
}
