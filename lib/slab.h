/* SLUB-flavoured object cache allocator + kmalloc size classes (portable). */
#ifndef KESTREL_SLAB_H
#define KESTREL_SLAB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SLAB_PAGE_SIZE 4096u
#define SLAB_MAGIC 0x51ab51abu
#define LARGE_MAGIC 0x1a26e000u
#define SLAB_POISON_FREE 0x6b

struct page_source {
    void *(*alloc_pages)(size_t n, void *ctx); /* page-aligned, contiguous */
    void (*free_pages)(void *p, size_t n, void *ctx);
    void *ctx;
};

struct slab {
    uint32_t magic;
    uint32_t inuse;
    struct kmem_cache *cache;
    struct slab *next, *prev;
    void *freelist;
};

struct slab_list { struct slab *head; };

struct kmem_cache {
    const char *name;
    size_t obj_size;       /* rounded object size */
    size_t objs_per_slab;
    struct slab_list partial, full, empty;
    size_t nr_empty;
    const struct page_source *src;
    void (*ctor)(void *obj);
    /* statistics */
    size_t nr_slabs, active_objs, total_allocs, total_frees, bad_frees;
};

int kmem_cache_init(struct kmem_cache *c, const char *name, size_t size,
                    void (*ctor)(void *), const struct page_source *src);
void *kmem_cache_alloc(struct kmem_cache *c);
/* Returns false (and does nothing) on a detected invalid/double free. */
bool kmem_cache_free(struct kmem_cache *c, void *obj);
/* Release cached empty slabs back to the page source. */
size_t kmem_cache_shrink(struct kmem_cache *c);
void kmem_cache_destroy(struct kmem_cache *c);

#define KMALLOC_NCLASSES 8 /* 8,16,32,64,128,256,512,1024 */
#define KMALLOC_MAX_SLAB 1024u

struct kmalloc_heap {
    struct kmem_cache classes[KMALLOC_NCLASSES];
    const struct page_source *src;
    size_t large_pages; /* pages currently held by large allocations */
};

void kheap_init(struct kmalloc_heap *h, const struct page_source *src);
void *kheap_alloc(struct kmalloc_heap *h, size_t size);
bool kheap_free(struct kmalloc_heap *h, void *ptr);
/* Usable size of an allocation (0 if pointer is not recognised). */
size_t kheap_usable_size(void *ptr);
#endif
