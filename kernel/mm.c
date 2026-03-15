/* Physical memory (bitmap PMM), paging (identity-mapped kernel, user window,
 * demand-paged region), page fault handling and the kmalloc heap. */
#include "kernel.h"
#include "bitmap_pmm.h"
#include "slab.h"

#define MAX_FRAMES (IDENTITY_MAP_END / PAGE_SIZE)

static uint32_t pmm_bitmap[MAX_FRAMES / 32];
static struct pmm pmm;
static struct spinlock pmm_lock = SPINLOCK_INIT("pmm");
static uint32_t *page_dir;
uint32_t demand_faults;

uint32_t pmm_alloc_frame(void)
{
    uint32_t f;
    spin_lock_irqsave(&pmm_lock, &f);
    uint32_t pa = (uint32_t)pmm_alloc(&pmm);
    spin_unlock_irqrestore(&pmm_lock, f);
    return pa;
}

uint32_t pmm_alloc_frames(size_t n)
{
    uint32_t f;
    spin_lock_irqsave(&pmm_lock, &f);
    uint32_t pa = (uint32_t)pmm_alloc_contig(&pmm, n);
    spin_unlock_irqrestore(&pmm_lock, f);
    return pa;
}

void pmm_free_frame(uint32_t pa)
{
    if (pmm_double_free_check(pa))
        panic("pmm: bad/double free of frame %08x", pa);
}

bool pmm_double_free_check(uint32_t pa)
{
    uint32_t f;
    spin_lock_irqsave(&pmm_lock, &f);
    bool ok = pmm_free(&pmm, pa);
    spin_unlock_irqrestore(&pmm_lock, f);
    return !ok;
}

void pmm_free_frames(uint32_t pa, size_t n)
{
    for (size_t i = 0; i < n; i++)
        pmm_free_frame(pa + i * PAGE_SIZE);
}

uint32_t pmm_free_count(void) { return (uint32_t)pmm.free_frames; }

/* ---------------- paging ---------------- */
static uint32_t *get_pte(uint32_t va, bool create, uint32_t pde_flags)
{
    uint32_t pdi = va >> 22;
    if (!(page_dir[pdi] & PTE_P)) {
        if (!create)
            return 0;
        uint32_t pt = pmm_alloc_frame();
        if (!pt)
            return 0;
        memset((void *)pt, 0, PAGE_SIZE);
        page_dir[pdi] = pt | PTE_P | PTE_W | pde_flags;
    }
    uint32_t *pt = (uint32_t *)(page_dir[pdi] & ~0xfffu);
    return &pt[(va >> 12) & 0x3ff];
}

int vmm_map(uint32_t va, uint32_t pa, uint32_t flags)
{
    uint32_t *pte = get_pte(va, true, flags & PTE_U);
    if (!pte)
        return -1;
    *pte = (pa & ~0xfffu) | flags | PTE_P;
    invlpg(va);
    return 0;
}

void vmm_unmap(uint32_t va)
{
    uint32_t *pte = get_pte(va, false, 0);
    if (pte) {
        *pte = 0;
        invlpg(va);
    }
}

uint32_t vmm_translate(uint32_t va, uint32_t *flags_out)
{
    uint32_t *pte = get_pte(va, false, 0);
    if (!pte || !(*pte & PTE_P))
        return 0;
    if (flags_out)
        *flags_out = *pte & 0xfff & (page_dir[va >> 22] | ~(uint32_t)PTE_U);
    return (*pte & ~0xfffu) | (va & 0xfff);
}

bool page_fault_handler(struct regs *r)
{
    uint32_t addr = read_cr2();
    trace_emit(EV_PAGE_FAULT, addr, r->err_code);
    bool present = r->err_code & 1, user = r->err_code & 4;
    if (!present && !user && addr >= DEMAND_BASE && addr < DEMAND_END) {
        uint32_t pa = pmm_alloc_frame();
        if (!pa)
            return false;
        memset((void *)pa, 0, PAGE_SIZE);
        vmm_map(addr & ~0xfffu, pa, PTE_W);
        demand_faults++;
        kprintf("[pf] demand-paged fault at %08x (err=%x, eip=%08x) -> mapped frame %08x\n", addr, r->err_code,
                r->eip, pa);
        return true;
    }
    if (!user) {
        kprintf("\n!!! PAGE FAULT at %08x: %s %s in kernel mode\n", addr, present ? "protection" : "not-present",
                (r->err_code & 2) ? "write" : "read");
    }
    return false;
}

/* ---------------- kmalloc ---------------- */
static void *kps_alloc(size_t n, void *ctx)
{
    (void)ctx;
    return (void *)pmm_alloc_frames(n);
}
static void kps_free(void *p, size_t n, void *ctx)
{
    (void)ctx;
    pmm_free_frames((uint32_t)p, n);
}
const struct page_source kernel_page_source = {kps_alloc, kps_free, 0};
static struct kmalloc_heap heap;
static struct spinlock heap_lock = SPINLOCK_INIT("kheap");

struct kmalloc_heap *kernel_heap(void) { return &heap; }

void *kmalloc(size_t n)
{
    uint32_t f;
    spin_lock_irqsave(&heap_lock, &f);
    void *p = kheap_alloc(&heap, n);
    spin_unlock_irqrestore(&heap_lock, f);
    trace_emit(EV_KMALLOC, n, (uint32_t)p);
    return p;
}

void kfree(void *p)
{

    uint32_t f;
    spin_lock_irqsave(&heap_lock, &f);
    bool ok = kheap_free(&heap, p);
    spin_unlock_irqrestore(&heap_lock, f);
    if (!ok)
        panic("kfree: invalid or double free of %p", p);
}

void mm_dump_stats(void)
{
    kprintf("[mm] free frames: %u (%u KiB)\n", pmm_free_count(), pmm_free_count() * 4);
    for (int i = 0; i < KMALLOC_NCLASSES; i++) {
        struct kmem_cache *c = &heap.classes[i];
        if (c->total_allocs)
            kprintf("[mm]   %12s: obj=%4u slabs=%u active=%u allocs=%u frees=%u\n", c->name, (uint32_t)c->obj_size,
                    (uint32_t)c->nr_slabs, (uint32_t)c->active_objs, (uint32_t)c->total_allocs,
                    (uint32_t)c->total_frees);
    }
}

void df_tss_set_cr3(uint32_t cr3);

void mm_init(uint32_t mem_upper_kb, uint32_t kernel_end)
{
    uint32_t top = 0x100000 + mem_upper_kb * 1024;
    if (top > IDENTITY_MAP_END)
        top = IDENTITY_MAP_END;
    pmm_init(&pmm, pmm_bitmap, MAX_FRAMES, 0);
    uint32_t start = (kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    pmm_mark_free(&pmm, start, top - start);
    kprintf("[mm] memory: %u KiB upper, usable frames %08x-%08x: %u free\n", mem_upper_kb, start, top,
            pmm_free_count());

    page_dir = (uint32_t *)pmm_alloc_frame();
    memset(page_dir, 0, PAGE_SIZE);
    for (uint32_t va = 0; va < IDENTITY_MAP_END; va += PAGE_SIZE)
        vmm_map(va, va, PTE_W);
    vmm_unmap(0); /* null page stays unmapped to catch NULL dereferences */
    __asm__ volatile("mov %0, %%cr3" ::"r"(page_dir));
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80010000u; /* PG | WP (supervisor writes honour read-only) */
    __asm__ volatile("mov %0, %%cr0" ::"r"(cr0));
    df_tss_set_cr3((uint32_t)page_dir);
    kprintf("[mm] paging enabled: identity map 0-%uMiB, page dir at %p\n", IDENTITY_MAP_END >> 20, page_dir);
    kheap_init(&heap, &kernel_page_source);
}
