/* Kestrel kernel-wide declarations (i686, uniprocessor). */
#ifndef KESTREL_KERNEL_H
#define KESTREL_KERNEL_H
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_CS 0x1b
#define USER_DS 0x23
#define TSS_SEL 0x28
#define DF_TSS_SEL 0x30

#define PAGE_SIZE 4096u
#define IDENTITY_MAP_END (64u * 1024 * 1024)
#define USER_BASE 0x40000000u
#define USER_END 0x40400000u
#define USER_STACK_TOP USER_END
#define USER_STACK_PAGES 4
#define DEMAND_BASE 0x50000000u
#define DEMAND_END 0x50400000u

#define HZ 100
#define EFAULT 14
#define EINVAL 22
#define ENOSYS 38

/* QEMU isa-debug-exit codes: process exit status = (code << 1) | 1 */
#define QEMU_EXIT_PASS 0x10  /* -> 33 */

#define QEMU_EXIT_FAIL 0x11  /* -> 35 */
#define QEMU_EXIT_PANIC 0x12 /* -> 37 */

/* ---- port I/O and CPU helpers ---- */
static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0, %1" ::"a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void cpu_relax(void) { __asm__ volatile("pause"); }
static inline void irq_enable(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void irq_disable(void) { __asm__ volatile("cli" ::: "memory"); }
static inline uint32_t read_eflags(void) { uint32_t f; __asm__ volatile("pushfl; popl %0" : "=r"(f)::"memory"); return f; }
static inline uint32_t irq_save(void) { uint32_t f = read_eflags(); irq_disable(); return f; }
static inline void irq_restore(uint32_t f) { if (f & 0x200) irq_enable(); }
static inline uint32_t read_cr2(void) { uint32_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline uint32_t read_cr3(void) { uint32_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline uint64_t rdtsc(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }
static inline void invlpg(uint32_t va) { __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory"); }

/* ---- interrupt frame (layout matches arch/x86/isr.asm) ---- */
struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss; /* useresp/ss valid only from ring 3 */
};

/* ---- klib ---- */
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strstr_k(const char *h, const char *n);

/* ---- console ---- */
void serial_init(void);
void serial_putc(char c);
void vga_init(void);
void vga_putc(char c);
uint16_t vga_cell(int row, int col);
int vga_row(void);
void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t n, const char *fmt, ...);
void console_write(const char *s, size_t n);
__attribute__((noreturn)) void panic(const char *fmt, ...);
__attribute__((noreturn)) void qemu_exit(uint8_t code);

/* ---- arch ---- */
void gdt_init(void);
void tss_set_kernel_stack(uint32_t esp0);
void idt_init(void);
void pic_init(void);
void pit_init(uint32_t hz);
typedef void (*irq_handler_t)(struct regs *r);
void irq_register(int irq, irq_handler_t h);
void dump_regs(const struct regs *r);
extern volatile uint32_t ticks;
extern void (*timer_hook)(uint32_t tick);
void switch_context(uint32_t *old_esp, uint32_t new_esp);
__attribute__((noreturn)) void enter_usermode(uint32_t eip, uint32_t esp, uint32_t arg);

/* ---- memory ---- */
void mm_init(uint32_t mem_upper_kb, uint32_t kernel_end);
uint32_t pmm_alloc_frame(void);
void pmm_free_frame(uint32_t pa);
uint32_t pmm_free_count(void);
uint32_t pmm_alloc_frames(size_t n);
void pmm_free_frames(uint32_t pa, size_t n);
bool pmm_double_free_check(uint32_t pa); /* true if free was rejected */
#define PTE_P 1u
#define PTE_W 2u
#define PTE_U 4u
int vmm_map(uint32_t va, uint32_t pa, uint32_t flags);
void vmm_unmap(uint32_t va);
uint32_t vmm_translate(uint32_t va, uint32_t *flags_out); /* 0 if unmapped */
bool page_fault_handler(struct regs *r); /* true if handled */
extern uint32_t demand_faults;
void *kmalloc(size_t n);
void kfree(void *p);
struct kmem_cache;
struct kmalloc_heap;
struct kmalloc_heap *kernel_heap(void);
extern const struct page_source kernel_page_source;
void mm_dump_stats(void);

/* ---- sync ---- */
struct thread;
struct spinlock {
    volatile uint32_t locked;
    struct thread *owner;
    const char *name;
};
#define SPINLOCK_INIT(n) {0, 0, n}
void spin_lock_irqsave(struct spinlock *l, uint32_t *flags);
void spin_unlock_irqrestore(struct spinlock *l, uint32_t flags);

struct waitqueue {
    struct thread *head;
};

/* ---- scheduler ---- */
enum thread_state { T_READY, T_RUNNING, T_SLEEPING, T_BLOCKED, T_DEAD };
struct thread {
    uint32_t esp;
    uint32_t tid;
    char name[16];
    enum thread_state state;
    uint32_t stack_base; /* lowest address of allocation (guard page) */
    uint32_t stack_top;
    uint32_t wake_tick;
    int exit_code;
    bool detached;
    void (*fn)(void *);
    void *arg;
    struct thread *next;      /* run queue / wait queue / sleep list link */
    struct waitqueue exit_wq; /* joiners */
    uint32_t switches;
};
extern struct thread *current;
extern uint32_t context_switches;
void sched_init(void);
struct thread *thread_create(const char *name, void (*fn)(void *), void *arg);
int thread_join(struct thread *t);
void thread_detach(struct thread *t);
__attribute__((noreturn)) void thread_exit(int code);
void thread_yield(void);
void thread_sleep(uint32_t nticks);
void schedule(void); /* call with interrupts disabled */
void sched_tick(void);
extern volatile bool need_resched;
void wq_sleep(struct waitqueue *wq); /* call with irqs disabled; returns with irqs disabled */
void wq_wake_all(struct waitqueue *wq);
#define wait_event(wq, cond)                         \
    do {                                             \
        uint32_t __f = irq_save();                   \
        while (!(cond))                              \
            wq_sleep(wq);                            \
        irq_restore(__f);                            \
    } while (0)

/* ---- syscalls / user ---- */
enum { SYS_EXIT = 0, SYS_WRITE = 1, SYS_GETPID = 2, SYS_YIELD = 3, SYS_SLEEP = 4, SYS_UPTIME = 5, NR_SYSCALLS };
void syscall_dispatch(struct regs *r);
int access_ok_user(uint32_t uaddr, uint32_t len, bool write);
int copy_from_user(void *dst, uint32_t usrc, uint32_t len);
int copy_to_user(uint32_t udst, const void *src, uint32_t len);
int run_user_program(uint32_t mode); /* returns exit code */
extern uint32_t syscall_count;

/* ---- tracing ---- */
enum trace_event { EV_SCHED_SWITCH = 1, EV_IRQ, EV_SYSCALL, EV_PAGE_FAULT, EV_THREAD_CREATE, EV_THREAD_EXIT, EV_KMALLOC, EV_USER, EV_MAX };
struct trace_rec {
    uint32_t tick;
    uint32_t tsc_lo;
    uint16_t event;
    uint16_t tid;
    uint32_t a, b;
};
struct kbpf_insn;
void trace_emit(uint32_t event, uint32_t a, uint32_t b);
int trace_attach_filter(const struct kbpf_insn *prog, uint32_t len);
void trace_detach_filter(void);
void trace_reset(void);
void trace_set_enabled(bool on);
uint32_t trace_snapshot(struct trace_rec *out, uint32_t max);
void trace_dump(uint32_t max);
struct trace_stats { uint32_t emitted, recorded, filtered, overwritten; };
struct trace_stats trace_get_stats(void);

/* ---- self tests ---- */
struct ktest_ctx {
    int failed;
    const char *file;
    int line;
    const char *expr;
};
struct ktest {
    const char *name;
    void (*fn)(struct ktest_ctx *t);
};
#define KTEST(n)                                                                      \
    static void ktest_##n(struct ktest_ctx *t);                                       \
    static const struct ktest ktest_desc_##n __attribute__((used, section("ktests"), \
                                                            aligned(4))) = {#n, ktest_##n}; \
    static void ktest_##n(struct ktest_ctx *t)
#define KASSERT(c)                     \
    do {                               \
        if (!(c)) {                    \
            t->failed = 1;             \
            t->file = __FILE__;        \
            t->line = __LINE__;        \
            t->expr = #c;              \
            return;                    \
        }                              \
    } while (0)
int ktest_run_all(void); /* returns number of failures */
#endif
