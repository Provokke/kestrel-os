/* Host microbenchmarks for the portable kernel data structures (-O2). */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "bitmap_pmm.h"
#include "mpsc_queue.h"
#include "slab.h"
#include "spsc_ring.h"

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void *pa(size_t n, void *c) { (void)c; return aligned_alloc(4096, n * 4096); }
static void pf(void *p, size_t n, void *c) { (void)n; (void)c; free(p); }
static const struct page_source src = {pa, pf, NULL};

#define N 10000000u
static struct spsc_ring ring;
static uint32_t rbuf[4096];
static void *prod(void *a)
{
    (void)a;
    for (uint32_t i = 0; i < N;)
        if (spsc_push(&ring, &i))
            i++;
    return NULL;
}

struct m { struct mpsc_node n; uint32_t v; };
static struct mpsc_queue q;
#define MP 4
#define MPER 2500000u
static struct m *msgs[MP];
static void *mprod(void *a)
{
    uintptr_t id = (uintptr_t)a;
    for (uint32_t i = 0; i < MPER; i++)
        mpsc_push(&q, &msgs[id][i].n);
    return NULL;
}

int main(void)
{
    /* slab vs malloc: alloc+free pairs with a working set of 1024 */
    static struct kmalloc_heap h;
    kheap_init(&h, &src);
    static void *live[1024];
    double t = now();
    for (uint32_t i = 0; i < N; i++) {
        uint32_t k = i & 1023;
        if (live[k])
            kheap_free(&h, live[k]);
        live[k] = kheap_alloc(&h, 16 + (i % 7) * 24);
    }
    double ts = now() - t;
    for (int k = 0; k < 1024; k++) { kheap_free(&h, live[k]); live[k] = NULL; }
    t = now();
    for (uint32_t i = 0; i < N; i++) {
        uint32_t k = i & 1023;
        free(live[k]);
        live[k] = malloc(16 + (i % 7) * 24);
    }
    double tm = now() - t;
    for (int k = 0; k < 1024; k++) free(live[k]);
    printf("kmalloc/kfree (slab):   %6.1f ns/op   (glibc malloc/free: %6.1f ns/op)\n", ts / N * 1e9, tm / N * 1e9);

    static uint32_t bm[65536 / 32];
    struct pmm p;
    pmm_init(&p, bm, 65536, 0);
    pmm_mark_free(&p, 0, 65536u * 4096);
    static uintptr_t fr[4096];
    t = now();
    for (int r = 0; r < 1000; r++) {
        for (int i = 0; i < 4096; i++) fr[i] = pmm_alloc(&p);
        for (int i = 0; i < 4096; i++) pmm_free(&p, fr[i]);
    }
    printf("pmm alloc+free frame:   %6.1f ns/pair\n", (now() - t) / (1000.0 * 4096) * 1e9);

    spsc_init(&ring, rbuf, 4096, 4);
    pthread_t th;
    t = now();
    pthread_create(&th, NULL, prod, NULL);
    uint32_t v, got = 0;
    while (got < N)
        if (spsc_pop(&ring, &v)) got++;
    pthread_join(th, NULL);
    double dt = now() - t;
    printf("SPSC ring (2 threads):  %6.1f M items/s\n", N / dt / 1e6);

    mpsc_init(&q);
    pthread_t pt[MP];
    for (uintptr_t i = 0; i < MP; i++) msgs[i] = calloc(MPER, sizeof(struct m));
    t = now();
    for (uintptr_t i = 0; i < MP; i++) pthread_create(&pt[i], NULL, mprod, (void *)i);
    uint64_t g = 0;
    while (g < (uint64_t)MP * MPER)
        if (mpsc_pop(&q)) g++;
    for (int i = 0; i < MP; i++) pthread_join(pt[i], NULL);
    dt = now() - t;
    printf("MPSC queue (4P/1C):     %6.1f M items/s\n", (double)MP * MPER / dt / 1e6);
    for (int i = 0; i < MP; i++) free(msgs[i]);
    return 0;
}
