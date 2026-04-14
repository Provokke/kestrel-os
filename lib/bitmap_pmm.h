/* Bitmap physical page frame allocator (portable, host-testable). */  
#ifndef KESTREL_BITMAP_PMM_H
#define KESTREL_BITMAP_PMM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PMM_PAGE_SIZE 4096u

struct pmm {
    uint32_t *bitmap;     /* 1 bit per frame, 1 = used */
    size_t nframes;       /* frames tracked */
    uintptr_t base;       /* physical address of frame 0 */
    size_t free_frames;
    size_t hint;          /* next-fit search hint */
    size_t usable_lo, usable_hi; /* frame span ever released by pmm_mark_free */
};

/* All frames start "used"; release usable ranges with pmm_mark_free. */
void pmm_init(struct pmm *p, uint32_t *bitmap, size_t nframes, uintptr_t base);
void pmm_mark_free(struct pmm *p, uintptr_t addr, size_t len);
void pmm_mark_used(struct pmm *p, uintptr_t addr, size_t len);
/* Returns physical address or 0 on OOM. */
uintptr_t pmm_alloc(struct pmm *p);
/* n physically-contiguous frames; 0 on failure. */
uintptr_t pmm_alloc_contig(struct pmm *p, size_t n);
/* Returns false on double free / out-of-range / misaligned address. */
bool pmm_free(struct pmm *p, uintptr_t addr);
bool pmm_free_contig(struct pmm *p, uintptr_t addr, size_t n);
bool pmm_is_used(const struct pmm *p, uintptr_t addr);
static inline size_t pmm_bitmap_words(size_t nframes) { return (nframes + 31) / 32; }
#endif
