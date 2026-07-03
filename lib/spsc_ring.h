/* Lock-free single-producer / single-consumer ring buffer (portable).
 * Free-running 32-bit indices; capacity must be a power of two. The producer
 * publishes with a release store of `tail`; the consumer frees slots with a
 * release store of `head`. Each side only reads the other's index with acquire. */
#ifndef KESTREL_SPSC_RING_H
#define KESTREL_SPSC_RING_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct spsc_ring {
    uint32_t mask;
    uint32_t elem_size;
    unsigned char *buf;
    _Alignas(64) uint32_t head; /* written by consumer only */
    _Alignas(64) uint32_t tail; /* written by producer only */
    _Alignas(64) uint32_t cached_head; /* producer-private copy */
    _Alignas(64) uint32_t cached_tail; /* consumer-private copy */
};

/* capacity: power of two >= 2. buf must hold capacity*elem_size bytes. */
bool spsc_init(struct spsc_ring *r, void *buf, uint32_t capacity, uint32_t elem_size);
bool spsc_push(struct spsc_ring *r, const void *elem);
bool spsc_pop(struct spsc_ring *r, void *elem);
uint32_t spsc_count(const struct spsc_ring *r);
static inline uint32_t spsc_capacity(const struct spsc_ring *r) { return r->mask + 1; }
#endif
