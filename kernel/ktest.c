/* In-kernel self-test suite (KTEST). Tests are registered into the "ktests"
 * linker section by the KTEST() macro and run at boot. */
#include "kernel.h"
#include "kbpf.h"
#include "mpsc_queue.h"
#include "slab.h"
#include "spsc_ring.h"

extern const struct ktest __start_ktests[], __stop_ktests[];

int ktest_run_all(void)
{
    int pass = 0, fail = 0;
    uint32_t n = (uint32_t)(__stop_ktests - __start_ktests);
    kprintf("[ktest] running %u self-tests\n", n);
    for (const struct ktest *k = __start_ktests; k < __stop_ktests; k++) {
        struct ktest_ctx t = {0, 0, 0, 0};
        uint32_t t0 = ticks;
        k->fn(&t);
        if (t.failed) {
            fail++;
            kprintf("KTEST %-28s FAIL (%s:%d: %s)\n", k->name, t.file, t.line, t.expr);
        } else {
            pass++;
            kprintf("KTEST %-28s PASS (%u ticks)\n", k->name, ticks - t0);
        }
    }
    kprintf("[ktest] summary: %d passed, %d failed\n", pass, fail);
    return fail;
}

/* ---------------- memory ---------------- */
KTEST(pmm_alloc_free_unique)
{
    uint32_t before = pmm_free_count();
    uint32_t f[64];
    for (int i = 0; i < 64; i++) {
        f[i] = pmm_alloc_frame();
        KASSERT(f[i] != 0 && (f[i] & 0xfff) == 0);
        for (int j = 0; j < i; j++)
            KASSERT(f[i] != f[j]);
    }
    KASSERT(pmm_free_count() == before - 64);
    for (int i = 0; i < 64; i++)
        pmm_free_frame(f[i]);
    KASSERT(pmm_free_count() == before);
}

KTEST(pmm_double_free_rejected)
{
    uint32_t f = pmm_alloc_frame();
    KASSERT(f);
    KASSERT(!pmm_double_free_check(f)); /* first free ok */
    KASSERT(pmm_double_free_check(f));  /* second free rejected */
    KASSERT(pmm_double_free_check(0x1000)); /* never-free low memory */
}

KTEST(pmm_contiguous)
{
    uint32_t a = pmm_alloc_frames(16);
    KASSERT(a);
    for (uint32_t i = 0; i < 16; i++)
        KASSERT(vmm_translate(a + i * PAGE_SIZE, 0) == a + i * PAGE_SIZE);
    pmm_free_frames(a, 16);
}

KTEST(kmalloc_size_classes)
{
    static const uint32_t sizes[] = {1, 8, 13, 64, 100, 256, 700, 1024, 1500, 5000, 70000};
    void *p[11];
    for (int i = 0; i < 11; i++) {
        p[i] = kmalloc(sizes[i]);
        KASSERT(p[i]);
        KASSERT(kheap_usable_size(p[i]) >= sizes[i]);
        memset(p[i], i + 1, sizes[i]);
    }
    for (int i = 0; i < 11; i++) {
        unsigned char *c = p[i];
        KASSERT(c[0] == i + 1 && c[sizes[i] - 1] == i + 1);
        kfree(p[i]);
    }
}

static int obj_ctor_runs;
static void obj_ctor(void *o)
{
    obj_ctor_runs++;
    *(uint32_t *)o = 0xfeedface;
}

KTEST(slab_cache_ctor_reuse)
{
    static struct kmem_cache c;
    KASSERT(kmem_cache_init(&c, "ktest-obj", 48, obj_ctor, &kernel_page_source) == 0);
    void *objs[200];
    for (int i = 0; i < 200; i++) {
        objs[i] = kmem_cache_alloc(&c);
        KASSERT(objs[i] && *(uint32_t *)objs[i] == 0xfeedface);
    }
    KASSERT(c.nr_slabs >= 200 / c.objs_per_slab);
    KASSERT(obj_ctor_runs == 200);
    void *first = objs[0];
    for (int i = 0; i < 200; i++)
        KASSERT(kmem_cache_free(&c, objs[i]));
    KASSERT(!kmem_cache_free(&c, first)); /* double free detected */
    KASSERT(c.active_objs == 0);
    kmem_cache_destroy(&c);
    KASSERT(c.nr_slabs == 0);
}

KTEST(paging_map_translate_unmap)
{
    uint32_t pa = pmm_alloc_frame();
    uint32_t va = DEMAND_END - PAGE_SIZE; /* inside a kernel-only window */
    KASSERT(vmm_map(va, pa, PTE_W) == 0);
    uint32_t fl = 0;
    KASSERT(vmm_translate(va + 0x123, &fl) == pa + 0x123);
    KASSERT((fl & PTE_U) == 0);
    *(volatile uint32_t *)va = 0xabcd1234;
    KASSERT(*(volatile uint32_t *)pa == 0xabcd1234); /* alias via identity map */
    vmm_unmap(va);
    KASSERT(vmm_translate(va, 0) == 0);
    pmm_free_frame(pa);
}

KTEST(page_fault_demand_paging)
{
    uint32_t before = demand_faults;
    volatile uint32_t *p = (volatile uint32_t *)(DEMAND_BASE + 0x2000 + 4);
    KASSERT(vmm_translate((uint32_t)p, 0) == 0);
    *p = 77; /* faults, handler maps a zeroed frame, instruction restarts */
    KASSERT(*p == 77);
    KASSERT(demand_faults == before + 1);
    KASSERT(p[1] == 0); /* fresh page is zeroed */
    uint32_t pa = vmm_translate((uint32_t)p, 0) & ~0xfffu;
    vmm_unmap((uint32_t)p & ~0xfffu);
    pmm_free_frame(pa);
}

KTEST(user_copy_validation)
{
    char buf[8];
    KASSERT(access_ok_user(0x00100000, 4, false) == -EFAULT); /* kernel address */
    KASSERT(access_ok_user(USER_BASE, 4, false) == -EFAULT);  /* not mapped */
    KASSERT(access_ok_user(0xfffffff0, 0x20, false) == -EFAULT); /* wraps */
    KASSERT(access_ok_user(USER_END - 4, 8, false) == -EFAULT);  /* straddles end */
    uint32_t pa = pmm_alloc_frame();
    vmm_map(USER_BASE, pa, PTE_U); /* read-only user page */
    memcpy((void *)pa, "kestrel", 8);
    KASSERT(copy_from_user(buf, USER_BASE, 8) == 0);
    KASSERT(memcmp(buf, "kestrel", 8) == 0);
    KASSERT(copy_to_user(USER_BASE, buf, 8) == -EFAULT); /* not writable */
    KASSERT(access_ok_user(USER_BASE + PAGE_SIZE - 4, 8, false) == -EFAULT); /* 2nd page missing */
    vmm_unmap(USER_BASE);
    pmm_free_frame(pa);
}

/* ---------------- scheduling & sync ---------------- */
static volatile uint32_t spin_counts[2];
static volatile bool spin_stop;
static void spinner(void *arg)
{
    uint32_t i = (uint32_t)arg;
    while (!spin_stop)
        spin_counts[i]++; /* never yields: only preemption lets the other run */
}

KTEST(preemption_round_robin)
{
    spin_counts[0] = spin_counts[1] = 0;
    spin_stop = false;
    uint32_t sw0 = context_switches;
    struct thread *a = thread_create("spin0", spinner, (void *)0);

    struct thread *b = thread_create("spin1", spinner, (void *)1);
    thread_sleep(20);
    spin_stop = true;
    thread_join(a);
    thread_join(b);
    KASSERT(spin_counts[0] > 0 && spin_counts[1] > 0);
    KASSERT(context_switches - sw0 >= 4);
}

KTEST(sleep_duration)
{
    uint32_t t0 = ticks;
    thread_sleep(10);
    uint32_t dt = ticks - t0;
    KASSERT(dt >= 10 && dt <= 13);
}

static struct spinlock test_lock = SPINLOCK_INIT("ktest");
static volatile uint32_t shared_counter;
static volatile uint32_t local_total;
static volatile bool use_lock;

/* Each worker hammers a deliberately non-atomic read-modify-write for ~15
 * ticks of wall time, so timer preemption lands inside the window often. */
static void rmw_worker(void *arg)
{
    (void)arg;
    uint32_t mine = 0, end = ticks + 15;
    while ((int32_t)(ticks - end) < 0) {
        uint32_t f = 0;
        if (use_lock)
            spin_lock_irqsave(&test_lock, &f);
        uint32_t v = shared_counter;
        for (volatile int d = 0; d < 50; d++)
            ;
        shared_counter = v + 1;
        if (use_lock)
            spin_unlock_irqrestore(&test_lock, f);
        mine++;
    }
    uint32_t f = irq_save();
    local_total += mine;
    irq_restore(f);
}

static void run_rmw(bool locked)
{
    shared_counter = 0;
    local_total = 0;
    use_lock = locked;
    struct thread *th[4];
    for (int i = 0; i < 4; i++)
        th[i] = thread_create("rmw", rmw_worker, 0);
    for (int i = 0; i < 4; i++)
        thread_join(th[i]);
    kprintf("[ktest] %s: counter=%u increments=%u\n", locked ? "locked" : "unlocked", shared_counter, local_total);
}

KTEST(spinlock_mutual_exclusion)
{
    run_rmw(true);
    KASSERT(local_total > 1000);
    KASSERT(shared_counter == local_total);
}

/* Negative control: without the lock, preemption inside the RMW window must
 * lose updates, proving the test above is actually sensitive to races. */
KTEST(race_detected_without_lock)
{
    run_rmw(false);
    KASSERT(shared_counter < local_total);
}

static struct waitqueue wq;
static volatile bool wq_flag;
static void wq_waker(void *arg)
{
    (void)arg;
    thread_sleep(5);
    wq_flag = true;
    wq_wake_all(&wq);
}

KTEST(waitqueue_sleep_wakeup)
{
    wq_flag = false;
    uint32_t t0 = ticks;
    struct thread *w = thread_create("waker", wq_waker, 0);
    wait_event(&wq, wq_flag);
    KASSERT(wq_flag);
    KASSERT(ticks - t0 >= 5);
    KASSERT(thread_join(w) == 0);
}

static void exiter(void *arg) { thread_exit((int)(uint32_t)arg); }

KTEST(thread_exit_code_and_reaping)
{
    thread_join(thread_create("warmup", exiter, 0)); /* prime slab caches */
    uint32_t before = pmm_free_count();
    for (int i = 0; i < 10; i++) {
        struct thread *th = thread_create("exiter", exiter, (void *)(uint32_t)(100 + i));
        KASSERT(thread_join(th) == 100 + i);
    }
    struct thread *d = thread_create("detached", exiter, (void *)7);
    thread_detach(d);
    thread_sleep(3); /* idle thread reaps it */
    KASSERT(pmm_free_count() == before); /* no stack frames leaked */
}

/* SPSC: the timer interrupt is the producer, a thread is the consumer. */
static struct spsc_ring tick_ring;
static uint32_t tick_ring_buf[16];
static void tick_producer(uint32_t tick) { spsc_push(&tick_ring, &tick); }

KTEST(spsc_irq_to_thread)
{
    KASSERT(spsc_init(&tick_ring, tick_ring_buf, 16, sizeof(uint32_t)));
    timer_hook = tick_producer;
    uint32_t prev = 0, got = 0;
    while (got < 30) {
        uint32_t v;
        if (spsc_pop(&tick_ring, &v)) {
            if (got)
                KASSERT(v == prev + 1); /* consecutive: nothing lost or reordered */
            prev = v;
            got++;
        } else {
            thread_sleep(1);
        }
    }
    timer_hook = 0;
}

/* MPSC: four preemptible producer threads, one consumer. */
struct kmsg {
    struct mpsc_node node;
    uint32_t producer, seq;
};
static struct mpsc_queue mq;
static void mpsc_prod(void *arg)
{
    uint32_t id = (uint32_t)arg;
    for (uint32_t i = 0; i < 300; i++) {
        struct kmsg *m = kmalloc(sizeof *m);
        m->producer = id;
        m->seq = i;
        mpsc_push(&mq, &m->node);
        if (i % 50 == 0)
            thread_yield();
    }
}

KTEST(mpsc_multi_producer)
{
    mpsc_init(&mq);
    struct thread *th[4];
    for (uint32_t i = 0; i < 4; i++)
        th[i] = thread_create("mpsc-prod", mpsc_prod, (void *)i);
    uint32_t next[4] = {0}, got = 0, bad = 0;
    while (got < 1200) {
        struct mpsc_node *n = mpsc_pop(&mq);
        if (!n) {
            thread_yield();
            continue;
        }
        struct kmsg *m = mpsc_entry(n, struct kmsg, node);
        if (m->seq != next[m->producer])
            bad++;
        next[m->producer] = m->seq + 1;
        kfree(m);
        got++;
    }
    for (int i = 0; i < 4; i++)
        thread_join(th[i]);
    KASSERT(bad == 0);
    KASSERT(mpsc_pop(&mq) == 0);
}

/* ---------------- tracing ---------------- */
KTEST(trace_kbpf_filter)
{
    /* keep only EV_USER events whose a-field is odd */
    struct kbpf_insn prog[] = {
        KBPF_I(KBPF_LDF, 0, 0, 0, 0),       KBPF_I(KBPF_JNEI, 0, 0, 3, EV_USER),
        KBPF_I(KBPF_LDF, 1, 0, 0, 1),       KBPF_I(KBPF_ANDI, 1, 0, 0, 1),
        KBPF_I(KBPF_RET, 1, 0, 0, 0),       KBPF_I(KBPF_RETI, 0, 0, 0, 0),
    };
    struct kbpf_insn bad[] = {KBPF_I(KBPF_JA, 0, 0, 5, 0), KBPF_I(KBPF_RETI, 0, 0, 0, 1)};
    KASSERT(trace_attach_filter(bad, 2) == KBPF_ERR_JUMP); /* verifier rejects */
    KASSERT(trace_attach_filter(prog, 6) == 0);
    trace_reset();
    for (uint32_t i = 0; i < 10; i++)
        trace_emit(EV_USER, i, 0);
    trace_detach_filter();
    struct trace_stats s = trace_get_stats();
    KASSERT(s.recorded == 5);
    struct trace_rec recs[8];
    uint32_t n = trace_snapshot(recs, 8);
    KASSERT(n == 5);
    for (uint32_t i = 0; i < n; i++)
        KASSERT(recs[i].event == EV_USER && recs[i].a == 2 * i + 1);
}

KTEST(trace_ring_overwrite)
{
    uint32_t f = irq_save(); /* keep other tracepoints out of the window */
    trace_reset();
    for (uint32_t i = 0; i < 300; i++)
        trace_emit(EV_USER, i, 0);
    struct trace_stats s = trace_get_stats();
    irq_restore(f);
    KASSERT(s.overwritten == 300 - 256);
    struct trace_rec r[1];
    KASSERT(trace_snapshot(r, 1) == 1);
    KASSERT(r[0].a == 299); /* newest kept */
}

/* ---------------- console ---------------- */
KTEST(vga_text_console)
{
    kprintf("vga-probe-XYZ\n");
    int row = vga_row() - 1;
    KASSERT(row >= 0);
    KASSERT((vga_cell(row, 0) & 0xff) == 'v');
    KASSERT((vga_cell(row, 12) & 0xff) == 'Z');
    KASSERT((vga_cell(row, 0) >> 8) == 0x0f);
}

KTEST(ksnprintf_formats)
{
    char b[64];
    ksnprintf(b, sizeof b, "%d|%u|%x|%08x|%s|%c|%5s|%-3s|", -42, 7u, 0xbeefu, 0x1fu, "ok", 'Z', "ab", "c");
    KASSERT(strcmp(b, "-42|7|beef|0000001f|ok|Z|   ab|c  |") == 0);
    KASSERT(ksnprintf(b, 4, "%s", "truncate") == 8 && strcmp(b, "tru") == 0);
}
