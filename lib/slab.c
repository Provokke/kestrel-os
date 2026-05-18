#include "slab.h"

#define HDR_SIZE ((sizeof(struct slab) + 15u) & ~15u)

static void *mem_set(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}

static void list_push(struct slab_list *l, struct slab *s)
{
    s->prev = NULL;
    s->next = l->head;
    if (l->head)
        l->head->prev = s;
    l->head = s;
}

static void list_del(struct slab_list *l, struct slab *s)
{
    if (s->prev)
        s->prev->next = s->next;
    else
        l->head = s->next;
    if (s->next)
        s->next->prev = s->prev;
    s->next = s->prev = NULL;
}

int kmem_cache_init(struct kmem_cache *c, const char *name, size_t size,
                    void (*ctor)(void *), const struct page_source *src)
{
    if (size == 0 || size > SLAB_PAGE_SIZE - HDR_SIZE)
        return -1;
    mem_set(c, 0, sizeof(*c));
    c->name = name;
    c->obj_size = (size + 7) & ~(size_t)7;
    if (c->obj_size < sizeof(void *))
        c->obj_size = sizeof(void *);
    c->objs_per_slab = (SLAB_PAGE_SIZE - HDR_SIZE) / c->obj_size;
    c->src = src;
    c->ctor = ctor;
    return 0;
}

static struct slab *slab_new(struct kmem_cache *c)
{
    unsigned char *page = c->src->alloc_pages(1, c->src->ctx);
    if (!page)
        return NULL;
    struct slab *s = (struct slab *)page;
    s->magic = SLAB_MAGIC;
    s->inuse = 0;
    s->cache = c;
    s->next = s->prev = NULL;
    s->freelist = NULL;
    /* Build the freelist in address order (last object pushed first). */
    for (size_t i = c->objs_per_slab; i-- > 0;) {
        void **obj = (void **)(page + HDR_SIZE + i * c->obj_size);
        mem_set(obj, SLAB_POISON_FREE, c->obj_size);
        *obj = s->freelist;
        s->freelist = obj;
    }
    c->nr_slabs++;
    return s;
}

void *kmem_cache_alloc(struct kmem_cache *c)
{
    struct slab *s = c->partial.head;
    if (!s) {
        s = c->empty.head;
        if (s) {
            list_del(&c->empty, s);
            c->nr_empty--;
        } else {
            s = slab_new(c);
            if (!s)
                return NULL;
        }

        list_push(&c->partial, s);
    }
    void **obj = s->freelist;
    s->freelist = *obj;
    s->inuse++;
    if (s->inuse == c->objs_per_slab) {
        list_del(&c->partial, s);
        list_push(&c->full, s);
    }
    c->active_objs++;
    c->total_allocs++;
    mem_set(obj, 0, c->obj_size);
    if (c->ctor)
        c->ctor(obj);
    return obj;
}

static bool on_freelist(struct slab *s, void *obj)
{
    for (void **p = s->freelist; p; p = *p)
        if (p == obj)
            return true;
    return false;
}

bool kmem_cache_free(struct kmem_cache *c, void *obj)
{
    if (!obj)
        return true;
    struct slab *s = (struct slab *)((uintptr_t)obj & ~(uintptr_t)(SLAB_PAGE_SIZE - 1));
    uintptr_t off = (uintptr_t)obj - (uintptr_t)s;
    if (s->magic != SLAB_MAGIC || s->cache != c || off < HDR_SIZE ||
        (off - HDR_SIZE) % c->obj_size != 0 || (off - HDR_SIZE) / c->obj_size >= c->objs_per_slab ||
        s->inuse == 0 || on_freelist(s, obj)) {
        c->bad_frees++;
        return false;
    }
    bool was_full = s->inuse == c->objs_per_slab;
    mem_set(obj, SLAB_POISON_FREE, c->obj_size);
    *(void **)obj = s->freelist;
    s->freelist = obj;
    s->inuse--;
    c->active_objs--;
    c->total_frees++;
    if (was_full) {
        list_del(&c->full, s);
        list_push(&c->partial, s);
    }
    if (s->inuse == 0) {
        list_del(&c->partial, s);
        if (c->nr_empty >= 2) { /* keep a small reserve, give the rest back */
            s->magic = 0;
            c->src->free_pages(s, 1, c->src->ctx);
            c->nr_slabs--;
        } else {
            list_push(&c->empty, s);
            c->nr_empty++;
        }
    }
    return true;
}

size_t kmem_cache_shrink(struct kmem_cache *c)
{
    size_t n = 0;
    while (c->empty.head) {
        struct slab *s = c->empty.head;
        list_del(&c->empty, s);
        s->magic = 0;
        c->src->free_pages(s, 1, c->src->ctx);
        c->nr_slabs--;
        n++;
    }
    c->nr_empty = 0;
    return n;
}

static void free_list(struct kmem_cache *c, struct slab_list *l)
{
    while (l->head) {
        struct slab *s = l->head;
        list_del(l, s);
        s->magic = 0;
        c->src->free_pages(s, 1, c->src->ctx);
        c->nr_slabs--;
    }
}

void kmem_cache_destroy(struct kmem_cache *c)
{
    free_list(c, &c->partial);
    free_list(c, &c->full);
    free_list(c, &c->empty);
    c->nr_empty = 0;
}

/* ---------------- kmalloc size classes ---------------- */

static const char *const class_names[KMALLOC_NCLASSES] = {
    "kmalloc-8", "kmalloc-16", "kmalloc-32", "kmalloc-64",
    "kmalloc-128", "kmalloc-256", "kmalloc-512", "kmalloc-1024",
};

struct large_hdr {
    uint32_t magic;
    uint32_t npages;
    uint32_t size;
    uint32_t pad;
};

void kheap_init(struct kmalloc_heap *h, const struct page_source *src)
{
    h->src = src;
    h->large_pages = 0;
    for (int i = 0; i < KMALLOC_NCLASSES; i++)
        kmem_cache_init(&h->classes[i], class_names[i], (size_t)8 << i, NULL, src);
}

static int size_class(size_t size)
{
    int i = 0;
    size_t s = 8;
    while (s < size) {
        s <<= 1;
        i++;
    }
    return i;
}

void *kheap_alloc(struct kmalloc_heap *h, size_t size)
{
    if (size == 0)
        return NULL;
    if (size <= KMALLOC_MAX_SLAB)
        return kmem_cache_alloc(&h->classes[size_class(size)]);
    if (size > (size_t)1 << 30)
        return NULL;
    size_t n = (size + sizeof(struct large_hdr) + SLAB_PAGE_SIZE - 1) / SLAB_PAGE_SIZE;
    struct large_hdr *hdr = h->src->alloc_pages(n, h->src->ctx);
    if (!hdr)
        return NULL;
    hdr->magic = LARGE_MAGIC;
    hdr->npages = (uint32_t)n;
    hdr->size = (uint32_t)size;
    h->large_pages += n;
    return hdr + 1;
}

bool kheap_free(struct kmalloc_heap *h, void *ptr)
{
    if (!ptr)
        return true;
    void *page = (void *)((uintptr_t)ptr & ~(uintptr_t)(SLAB_PAGE_SIZE - 1));
    uint32_t magic = *(uint32_t *)page;
    if (magic == SLAB_MAGIC) {
        struct slab *s = page;
        for (int i = 0; i < KMALLOC_NCLASSES; i++)
            if (s->cache == &h->classes[i])
                return kmem_cache_free(s->cache, ptr);
        return false;
    }
    if (magic == LARGE_MAGIC && ptr == (struct large_hdr *)page + 1) {
        struct large_hdr *hdr = page;
        size_t n = hdr->npages;
        hdr->magic = 0;
        h->large_pages -= n;
        h->src->free_pages(page, n, h->src->ctx);
        return true;
    }
    return false;
}

size_t kheap_usable_size(void *ptr)
{
    void *page = (void *)((uintptr_t)ptr & ~(uintptr_t)(SLAB_PAGE_SIZE - 1));
    uint32_t magic = *(uint32_t *)page;
    if (magic == SLAB_MAGIC)
        return ((struct slab *)page)->cache->obj_size;
    if (magic == LARGE_MAGIC)
        return ((struct large_hdr *)page)->size;
    return 0;
}
