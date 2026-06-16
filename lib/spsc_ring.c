#include "spsc_ring.h"

static void copy_bytes(void *d, const void *s, size_t n)
{
    unsigned char *dp = d;
    const unsigned char *sp = s;
    while (n--)
        *dp++ = *sp++;
}

bool spsc_init(struct spsc_ring *r, void *buf, uint32_t capacity, uint32_t elem_size)
{
    if (capacity < 2 || (capacity & (capacity - 1)) || elem_size == 0)
        return false;
    r->mask = capacity - 1;
    r->elem_size = elem_size;
    r->buf = buf;
    r->head = r->tail = 0;
    r->cached_head = r->cached_tail = 0;
    return true;
}

bool spsc_push(struct spsc_ring *r, const void *elem)
{
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    if (tail - r->cached_head > r->mask) {
        r->cached_head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
        if (tail - r->cached_head > r->mask)
            return false; /* full */
    }
    copy_bytes(r->buf + (size_t)(tail & r->mask) * r->elem_size, elem, r->elem_size);
    __atomic_store_n(&r->tail, tail + 1, __ATOMIC_RELEASE);
    return true;
}

bool spsc_pop(struct spsc_ring *r, void *elem)
{
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    if (head == r->cached_tail) {
        r->cached_tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
        if (head == r->cached_tail)
            return false; /* empty */
    }
    copy_bytes(elem, r->buf + (size_t)(head & r->mask) * r->elem_size, r->elem_size);
    __atomic_store_n(&r->head, head + 1, __ATOMIC_RELEASE);
    return true;
}

uint32_t spsc_count(const struct spsc_ring *r)
{
    return __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE) - __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
}
