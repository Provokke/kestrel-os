/* Host-side unit + stress tests for the portable kernel libraries.
 * Built with -fsanitize=address,undefined (and separately with TSan for the
 * lock-free stress tests). */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bitmap_pmm.h"
#include "kbpf.h"
#include "mpsc_queue.h"
#include "slab.h"
#include "spsc_ring.h"

static int g_pass, g_fail;
static int t_fail;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) {                                                              \
            fprintf(stderr, "  CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); \
            t_fail = 1;                                                          \
            return;                                                              \
        }                                                                        \
    } while (0)
#define RUN(fn)                                       \
    do {                                              \
        t_fail = 0;                                   \
        fn();                                         \
        printf("[%s] %s\n", t_fail ? "FAIL" : " OK ", #fn); \
        if (t_fail) g_fail++; else g_pass++;          \
    } while (0)

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)rng_state;
}

/* ---------------- pmm ---------------- */
static void test_pmm_basic(void)
{
    uint32_t bm[4];
    struct pmm p;
    pmm_init(&p, bm, 100, 0x100000);
    CHECK(p.free_frames == 0);
    CHECK(pmm_alloc(&p) == 0);
    pmm_mark_free(&p, 0x100000, 100 * 4096);
    CHECK(p.free_frames == 100);
    pmm_mark_used(&p, 0x100000, 1); /* partial frame -> used */
    CHECK(p.free_frames == 99);
    uintptr_t a = pmm_alloc(&p);
    CHECK(a == 0x101000);
    CHECK(pmm_is_used(&p, a));
    CHECK(pmm_free(&p, a));
    CHECK(!pmm_free(&p, a));          /* double free detected */
    CHECK(!pmm_free(&p, a + 12));     /* misaligned */
    CHECK(!pmm_free(&p, 0x100000 + 200 * 4096)); /* out of range */
    CHECK(!pmm_free(&p, 0x1000));     /* below base */
    uint32_t bm2[4];
    struct pmm q;
    pmm_init(&q, bm2, 100, 0);
    pmm_mark_free(&q, 10 * 4096, 20 * 4096);
    CHECK(!pmm_free(&q, 5 * 4096));   /* reserved frame was never usable */
    CHECK(!pmm_free(&q, 30 * 4096));
    CHECK(p.free_frames == 99);
}

static void test_pmm_exhaust(void)
{
    uint32_t bm[pmm_bitmap_words(77)];
    struct pmm p;
    pmm_init(&p, bm, 77, 0);
    pmm_mark_free(&p, 0, 77 * 4096);
    uintptr_t seen[77];
    for (int i = 0; i < 77; i++) {
        seen[i] = pmm_alloc(&p);
        CHECK(seen[i] != 0 || i == 0); /* frame 0 at address 0 is legal but ambiguous */
    }
    CHECK(p.free_frames == 0);
    CHECK(pmm_alloc(&p) == 0);
    for (int i = 0; i < 77; i++)
        for (int j = i + 1; j < 77; j++)
            CHECK(seen[i] != seen[j]);
}

static void test_pmm_contig_random_model(void)
{
    enum { N = 1000 };
    uint32_t bm[pmm_bitmap_words(N)];
    unsigned char model[N] = {0};
    struct pmm p;
    pmm_init(&p, bm, N, 0x400000);
    pmm_mark_free(&p, 0x400000, N * 4096);
    struct { uintptr_t a; size_t n; } live[512];
    int nlive = 0;
    for (int it = 0; it < 20000; it++) {
        if (nlive < 512 && (rnd() % 3 != 0 || nlive == 0)) {
            size_t n = 1 + rnd() % 8;
            uintptr_t a = pmm_alloc_contig(&p, n);
            if (!a)
                continue;
            size_t f = (a - 0x400000) / 4096;
            for (size_t k = 0; k < n; k++) {
                CHECK(model[f + k] == 0); /* no overlap with live allocations */
                model[f + k] = 1;
            }
            live[nlive].a = a;
            live[nlive].n = n;
            nlive++;
        } else {
            int i = (int)(rnd() % (uint32_t)nlive);
            CHECK(pmm_free_contig(&p, live[i].a, live[i].n));
            size_t f = (live[i].a - 0x400000) / 4096;
            for (size_t k = 0; k < live[i].n; k++)
                model[f + k] = 0;
            live[i] = live[--nlive];
        }
        size_t used = 0;
        for (int k = 0; k < N; k++)
            used += model[k];
        CHECK(p.free_frames == N - used);
    }
}

/* ---------------- slab ---------------- */
static size_t pages_out;
static void *host_alloc_pages(size_t n, void *ctx)
{
    (void)ctx;
    void *p = aligned_alloc(4096, n * 4096);
    if (p)
        pages_out += n;
    return p;
}
static void host_free_pages(void *p, size_t n, void *ctx)
{
    (void)ctx;
    pages_out -= n;
    free(p);
}
static const struct page_source host_src = {host_alloc_pages, host_free_pages, NULL};

static int ctor_calls;
static void ctor(void *o) { ctor_calls++; memset(o, 0xab, 4); }

static void test_slab_basic(void)
{
    struct kmem_cache c;
    CHECK(kmem_cache_init(&c, "test-40", 40, ctor, &host_src) == 0);
    CHECK(c.obj_size == 40);
    void *a = kmem_cache_alloc(&c), *b = kmem_cache_alloc(&c);
    CHECK(a && b && a != b);
    CHECK(((uintptr_t)a & 7) == 0);
    CHECK(ctor_calls == 2);
    CHECK(*(unsigned char *)a == 0xab);
    CHECK(c.active_objs == 2 && c.nr_slabs == 1);
    CHECK(kmem_cache_free(&c, a));
    CHECK(!kmem_cache_free(&c, a));            /* double free detected */
    CHECK(!kmem_cache_free(&c, (char *)b + 1)); /* interior pointer */
    CHECK(c.bad_frees == 2);
    CHECK(((unsigned char *)a)[sizeof(void *)] == SLAB_POISON_FREE); /* poisoned */
    CHECK(kmem_cache_free(&c, b));
    CHECK(c.active_objs == 0);
    kmem_cache_destroy(&c);
    CHECK(pages_out == 0);
    CHECK(kmem_cache_init(&c, "too-big", 5000, NULL, &host_src) == -1);
}

static void test_slab_random_model(void)
{
    struct kmem_cache c;
    kmem_cache_init(&c, "rand-24", 24, NULL, &host_src);
    enum { MAXLIVE = 3000 };
    static unsigned char *live[MAXLIVE];
    static unsigned char tag[MAXLIVE];
    int n = 0;
    for (int it = 0; it < 200000; it++) {
        if (n < MAXLIVE && (rnd() & 1)) {
            unsigned char *p = kmem_cache_alloc(&c);
            CHECK(p);
            tag[n] = (unsigned char)rnd();
            memset(p, tag[n], 24);
            live[n++] = p;
        } else if (n > 0) {
            int i = (int)(rnd() % (uint32_t)n);
            for (int k = 0; k < 24; k++)
                CHECK(live[i][k] == tag[i]); /* nobody scribbled on it */
            CHECK(kmem_cache_free(&c, live[i]));
            live[i] = live[--n];
            tag[i] = tag[n];
        }
    }
    CHECK(c.active_objs == (size_t)n);
    while (n)
        CHECK(kmem_cache_free(&c, live[--n]));
    CHECK(c.nr_slabs <= 2); /* only the empty reserve is kept */
    CHECK(kmem_cache_shrink(&c) == c.nr_empty || c.nr_slabs == 0);
    CHECK(c.nr_slabs == 0);
    CHECK(pages_out == 0);
}

static void test_kheap_classes_and_large(void)
{
    static struct kmalloc_heap h;
    kheap_init(&h, &host_src);
    CHECK(kheap_alloc(&h, 0) == NULL);
    size_t sizes[] = {1, 7, 8, 9, 31, 64, 100, 500, 1024, 1025, 4000, 4096, 12345, 100000};
    void *ptrs[sizeof sizes / sizeof sizes[0]];
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        ptrs[i] = kheap_alloc(&h, sizes[i]);
        CHECK(ptrs[i]);
        CHECK(kheap_usable_size(ptrs[i]) >= sizes[i]);
        memset(ptrs[i], 0x5a, sizes[i]);
    }
    CHECK(h.large_pages > 0);
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
        CHECK(kheap_free(&h, ptrs[i]));
    CHECK(h.large_pages == 0);
    CHECK(kheap_free(&h, NULL));
    for (int i = 0; i < KMALLOC_NCLASSES; i++)
        kmem_cache_destroy(&h.classes[i]);
    CHECK(pages_out == 0);
}

struct mt_slab_arg { struct kmalloc_heap *h; pthread_mutex_t *mu; int id; int errors; };
static void *mt_slab_worker(void *argp)
{
    struct mt_slab_arg *a = argp;
    void *mine[256] = {0};
    unsigned seed = (unsigned)a->id * 7919u;
    for (int it = 0; it < 100000; it++) {
        int i = (int)(rand_r(&seed) % 256);
        pthread_mutex_lock(a->mu);
        if (mine[i]) {
            if (*(int *)mine[i] != a->id * 1000 + i)
                a->errors++;
            if (!kheap_free(a->h, mine[i]))
                a->errors++;
            mine[i] = NULL;
        } else {
            mine[i] = kheap_alloc(a->h, 4 + rand_r(&seed) % 900);
            if (!mine[i])
                a->errors++;
            else
                *(int *)mine[i] = a->id * 1000 + i;
        }
        pthread_mutex_unlock(a->mu);
    }
    pthread_mutex_lock(a->mu);
    for (int i = 0; i < 256; i++)
        if (mine[i])
            kheap_free(a->h, mine[i]);
    pthread_mutex_unlock(a->mu);
    return NULL;
}

static void test_kheap_multithreaded(void)
{
    static struct kmalloc_heap h;
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    kheap_init(&h, &host_src);
    pthread_t th[4];
    struct mt_slab_arg args[4];
    for (int i = 0; i < 4; i++) {
        args[i] = (struct mt_slab_arg){&h, &mu, i + 1, 0};
        pthread_create(&th[i], NULL, mt_slab_worker, &args[i]);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
        CHECK(args[i].errors == 0);
    }
    for (int i = 0; i < KMALLOC_NCLASSES; i++) {
        CHECK(h.classes[i].active_objs == 0);
        kmem_cache_destroy(&h.classes[i]);
    }
    CHECK(pages_out == 0);
}

/* ---------------- SPSC ---------------- */
static void test_spsc_single_thread(void)
{
    struct spsc_ring r;
    uint32_t buf[8];
    CHECK(!spsc_init(&r, buf, 6, 4)); /* not a power of two */
    CHECK(spsc_init(&r, buf, 8, 4));
    uint32_t v;
    CHECK(!spsc_pop(&r, &v));
    for (uint32_t round = 0; round < 1000; round++) { /* exercises index wrap */
        uint32_t k = 1 + round % 8;
        for (uint32_t i = 0; i < k; i++)
            CHECK(spsc_push(&r, &(uint32_t){round * 10 + i}));
        if (k == 8)
            CHECK(!spsc_push(&r, &v)); /* full */
        CHECK(spsc_count(&r) == k);
        for (uint32_t i = 0; i < k; i++) {
            CHECK(spsc_pop(&r, &v));
            CHECK(v == round * 10 + i);
        }
        CHECK(!spsc_pop(&r, &v));
    }
    /* free-running 32-bit index overflow */
    r.head = r.tail = r.cached_head = r.cached_tail = 0xfffffffcu;
    for (uint32_t i = 0; i < 8; i++)
        CHECK(spsc_push(&r, &i));
    for (uint32_t i = 0; i < 8; i++) {
        CHECK(spsc_pop(&r, &v));
        CHECK(v == i);
    }
}

#define SPSC_ITEMS 5000000u
static struct spsc_ring g_spsc;
static uint64_t g_spsc_buf[1024];
static void *spsc_producer(void *arg)
{
    (void)arg;
    for (uint64_t i = 0; i < SPSC_ITEMS;) {
        uint64_t v = i * 2654435761u;
        if (spsc_push(&g_spsc, &v))
            i++;
    }
    return NULL;
}

static void test_spsc_stress_2threads(void)
{
    spsc_init(&g_spsc, g_spsc_buf, 1024, sizeof(uint64_t));
    pthread_t th;
    pthread_create(&th, NULL, spsc_producer, NULL);
    uint64_t expect = 0, v;
    int bad = 0;
    while (expect < SPSC_ITEMS) {
        if (spsc_pop(&g_spsc, &v)) {
            if (v != expect * 2654435761u)
                bad++;
            expect++;
        }
    }
    pthread_join(th, NULL);
    CHECK(bad == 0);
    CHECK(!spsc_pop(&g_spsc, &v));
}

/* ---------------- MPSC ---------------- */
struct msg { struct mpsc_node node; uint32_t producer; uint32_t seq; };
#define MPSC_PRODUCERS 4
#define MPSC_PER 300000u
static struct mpsc_queue g_q;
static struct msg *g_msgs[MPSC_PRODUCERS];

static void *mpsc_producer(void *arg)
{
    uint32_t id = (uint32_t)(uintptr_t)arg;
    for (uint32_t i = 0; i < MPSC_PER; i++) {
        g_msgs[id][i].producer = id;
        g_msgs[id][i].seq = i;
        mpsc_push(&g_q, &g_msgs[id][i].node);
    }
    return NULL;
}

static void test_mpsc_single_thread(void)
{
    struct mpsc_queue q;
    struct msg m[5];
    mpsc_init(&q);
    CHECK(mpsc_pop(&q) == NULL);
    CHECK(!mpsc_maybe_nonempty(&q));
    for (int round = 0; round < 3; round++) {
        for (uint32_t i = 0; i < 5; i++) {
            m[i].seq = i;
            mpsc_push(&q, &m[i].node);
        }
        CHECK(mpsc_maybe_nonempty(&q));
        for (uint32_t i = 0; i < 5; i++) {
            struct mpsc_node *n = mpsc_pop(&q);
            CHECK(n);
            CHECK(mpsc_entry(n, struct msg, node)->seq == i);
        }
        CHECK(mpsc_pop(&q) == NULL);
        CHECK(!mpsc_maybe_nonempty(&q));
    }
}

static void test_mpsc_stress_4producers(void)
{
    mpsc_init(&g_q);
    pthread_t th[MPSC_PRODUCERS];
    for (uint32_t i = 0; i < MPSC_PRODUCERS; i++) {
        g_msgs[i] = calloc(MPSC_PER, sizeof(struct msg));
        pthread_create(&th[i], NULL, mpsc_producer, (void *)(uintptr_t)i);
    }
    uint32_t next[MPSC_PRODUCERS] = {0};
    uint64_t got = 0;
    int bad = 0;
    while (got < (uint64_t)MPSC_PRODUCERS * MPSC_PER) {
        struct mpsc_node *n = mpsc_pop(&g_q);
        if (!n)
            continue;
        struct msg *m = mpsc_entry(n, struct msg, node);
        if (m->seq != next[m->producer])
            bad++; /* per-producer FIFO order must hold */
        next[m->producer] = m->seq + 1;
        got++;
    }
    for (uint32_t i = 0; i < MPSC_PRODUCERS; i++)
        pthread_join(th[i], NULL);
    CHECK(mpsc_pop(&g_q) == NULL);
    for (uint32_t i = 0; i < MPSC_PRODUCERS; i++) {
        CHECK(next[i] == MPSC_PER);
        free(g_msgs[i]);
    }
    CHECK(bad == 0);
}

/* ---------------- kbpf ---------------- */
static void test_kbpf_verifier(void)
{
    struct kbpf_insn ok[] = {KBPF_I(KBPF_LDF, 0, 0, 0, 1), KBPF_I(KBPF_JEQI, 0, 0, 1, 7),
                             KBPF_I(KBPF_RETI, 0, 0, 0, 0), KBPF_I(KBPF_RETI, 0, 0, 0, 1)};
    CHECK(kbpf_verify(ok, 4, 4) == KBPF_OK);
    CHECK(kbpf_verify(ok, 0, 4) == KBPF_ERR_LEN);
    CHECK(kbpf_verify(ok, 4, 1) == KBPF_ERR_FIELD);
    CHECK(kbpf_verify(ok, 2, 4) == KBPF_ERR_JUMP); /* jump past end */
    struct kbpf_insn noret[] = {KBPF_I(KBPF_MOVI, 0, 0, 0, 1)};
    CHECK(kbpf_verify(noret, 1, 4) == KBPF_ERR_NORET);
    struct kbpf_insn badreg[] = {KBPF_I(KBPF_RET, 9, 0, 0, 0)};
    CHECK(kbpf_verify(badreg, 1, 4) == KBPF_ERR_REG);
    struct kbpf_insn badop[] = {KBPF_I(200, 0, 0, 0, 0)};
    CHECK(kbpf_verify(badop, 1, 4) == KBPF_ERR_OP);
    struct kbpf_insn badsh[] = {KBPF_I(KBPF_RSHI, 0, 0, 0, 32), KBPF_I(KBPF_RET, 0, 0, 0, 0)};
    CHECK(kbpf_verify(badsh, 2, 4) == KBPF_ERR_SHIFT);
    struct kbpf_insn big[KBPF_MAX_INSNS + 1];
    for (int i = 0; i <= KBPF_MAX_INSNS; i++)
        big[i] = KBPF_I(KBPF_RETI, 0, 0, 0, 0);
    CHECK(kbpf_verify(big, KBPF_MAX_INSNS + 1, 4) == KBPF_ERR_LEN);
}
static void test_kbpf_run(void)
{
    /* accept if (ctx[0] == 3 && (ctx[1] >> 4) & 1) else ctx[2] + 5 */
    struct kbpf_insn p[] = {
        KBPF_I(KBPF_LDF, 0, 0, 0, 0), KBPF_I(KBPF_JNEI, 0, 0, 4, 3),
        KBPF_I(KBPF_LDF, 1, 0, 0, 1), KBPF_I(KBPF_RSHI, 1, 0, 0, 4),
        KBPF_I(KBPF_ANDI, 1, 0, 0, 1), KBPF_I(KBPF_RET, 1, 0, 0, 0),
        KBPF_I(KBPF_LDF, 2, 0, 0, 2), KBPF_I(KBPF_ADDI, 2, 0, 0, 5),
        KBPF_I(KBPF_MOV, 3, 2, 0, 0), KBPF_I(KBPF_RET, 3, 0, 0, 0),
    };
    CHECK(kbpf_verify(p, 10, 3) == KBPF_OK);
    CHECK(kbpf_run(p, 10, (uint32_t[]){3, 0x10, 0}) == 1);
    CHECK(kbpf_run(p, 10, (uint32_t[]){3, 0x20, 0}) == 0);
    CHECK(kbpf_run(p, 10, (uint32_t[]){4, 0, 37}) == 42);
}

/* Fuzz: random programs never crash the verifier, and every verified program
 * terminates within len steps without reading out-of-bounds (ASan watches). */
static void test_kbpf_fuzz(void)
{
    int accepted = 0;
    for (int it = 0; it < 200000; it++) {
        struct kbpf_insn p[16];
        uint32_t len = 1 + rnd() % 16;
        for (uint32_t i = 0; i < len; i++)
            p[i] = KBPF_I(rnd() % (KBPF_NOPS + 2), rnd() % 5, rnd() % 5, rnd() % 6, rnd() % 40);
        if (kbpf_verify(p, len, 4) != KBPF_OK)
            continue;
        accepted++;
        uint32_t *ctx = malloc(4 * sizeof(uint32_t)); /* exact size: ASan catches OOB */
        for (int k = 0; k < 4; k++)
            ctx[k] = rnd();
        (void)kbpf_run(p, len, ctx);
        free(ctx);
    }
    CHECK(accepted > 100);
}

int main(void)
{
    RUN(test_pmm_basic);
    RUN(test_pmm_exhaust);
    RUN(test_pmm_contig_random_model);
    RUN(test_slab_basic);
    RUN(test_slab_random_model);
    RUN(test_kheap_classes_and_large);
    RUN(test_kheap_multithreaded);
    RUN(test_spsc_single_thread);
    RUN(test_spsc_stress_2threads);
    RUN(test_mpsc_single_thread);
    RUN(test_mpsc_stress_4producers);
    RUN(test_kbpf_verifier);
    RUN(test_kbpf_run);
    RUN(test_kbpf_fuzz);
    printf("host tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
