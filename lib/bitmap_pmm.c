#include "bitmap_pmm.h"

static inline bool test_bit(const struct pmm *p, size_t i) { return (p->bitmap[i / 32] >> (i % 32)) & 1u; }
static inline void set_bit(struct pmm *p, size_t i) { p->bitmap[i / 32] |= 1u << (i % 32); }
static inline void clr_bit(struct pmm *p, size_t i) { p->bitmap[i / 32] &= ~(1u << (i % 32)); }

void pmm_init(struct pmm *p, uint32_t *bitmap, size_t nframes, uintptr_t base)
{
    p->bitmap = bitmap;
    p->nframes = nframes;
    p->base = base;
    p->free_frames = 0;
    p->hint = 0;
    p->usable_lo = (size_t)-1;
    p->usable_hi = 0;
    for (size_t i = 0; i < pmm_bitmap_words(nframes); i++)
        bitmap[i] = 0xffffffffu;
}

static bool frame_range(const struct pmm *p, uintptr_t addr, size_t len, size_t *first, size_t *last)
{
    if (addr < p->base)
        return false;
    uintptr_t start = (addr - p->base + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uintptr_t end = (addr - p->base + len) / PMM_PAGE_SIZE; /* exclusive */
    if (end > p->nframes)
        end = p->nframes;
    if (start >= end)
        return false;
    *first = start;
    *last = end;
    return true;
}

void pmm_mark_free(struct pmm *p, uintptr_t addr, size_t len)
{
    size_t a, b;
    if (!frame_range(p, addr, len, &a, &b))
        return;
    if (a < p->usable_lo)
        p->usable_lo = a;
    if (b > p->usable_hi)
        p->usable_hi = b;
    for (size_t i = a; i < b; i++)
        if (test_bit(p, i)) {
            clr_bit(p, i);
            p->free_frames++;
        }
}

void pmm_mark_used(struct pmm *p, uintptr_t addr, size_t len)
{
    if (addr < p->base)
        return;
    /* round outward: any partially-covered frame becomes used */
    size_t a = (addr - p->base) / PMM_PAGE_SIZE;
    size_t b = (addr - p->base + len + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    if (b > p->nframes)
        b = p->nframes;
    for (size_t i = a; i < b; i++)
        if (!test_bit(p, i)) {
            set_bit(p, i);
            p->free_frames--;
        }
}

uintptr_t pmm_alloc(struct pmm *p)
{
    if (p->free_frames == 0)
        return 0;
    size_t words = pmm_bitmap_words(p->nframes);
    size_t w0 = p->hint / 32;
    for (size_t k = 0; k < words; k++) {
        size_t w = (w0 + k) % words;
        if (p->bitmap[w] == 0xffffffffu)
            continue;
        uint32_t inv = ~p->bitmap[w];
        size_t bit = (size_t)__builtin_ctz(inv);
        size_t i = w * 32 + bit;
        if (i >= p->nframes)
            continue;
        set_bit(p, i);
        p->free_frames--;
        p->hint = i + 1 < p->nframes ? i + 1 : 0;
        return p->base + i * PMM_PAGE_SIZE;
    }
    return 0;
}

uintptr_t pmm_alloc_contig(struct pmm *p, size_t n)
{
    if (n == 0 || n > p->free_frames)
        return 0;
    if (n == 1)
        return pmm_alloc(p);
    size_t run = 0;
    for (size_t i = 0; i < p->nframes; i++) {
        if (test_bit(p, i)) {
            run = 0;
            continue;
        }
        if (++run == n) {
            size_t first = i + 1 - n;
            for (size_t j = first; j <= i; j++)
                set_bit(p, j);
            p->free_frames -= n;
            return p->base + first * PMM_PAGE_SIZE;
        }
    }
    return 0;
}

bool pmm_free(struct pmm *p, uintptr_t addr)
{
    if (addr < p->base || (addr - p->base) % PMM_PAGE_SIZE)
        return false;
    size_t i = (addr - p->base) / PMM_PAGE_SIZE;
    if (i >= p->nframes || i < p->usable_lo || i >= p->usable_hi || !test_bit(p, i))
        return false; /* out of range, reserved (never usable) frame, or double free */
    clr_bit(p, i);
    p->free_frames++;
    return true;
}

bool pmm_free_contig(struct pmm *p, uintptr_t addr, size_t n)
{
    bool ok = true;
    for (size_t k = 0; k < n; k++)
        ok &= pmm_free(p, addr + k * PMM_PAGE_SIZE);
    return ok;
}

bool pmm_is_used(const struct pmm *p, uintptr_t addr)
{
    if (addr < p->base)
        return true;
    size_t i = (addr - p->base) / PMM_PAGE_SIZE;
    return i >= p->nframes || test_bit(p, i);
}
