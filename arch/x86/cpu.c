/* GDT + TSS (incl. a dedicated double-fault task), IDT, 8259 PIC, PIT timer,
 * exception reporting and IRQ dispatch. */
#include "kernel.h"

struct __attribute__((packed)) gdt_entry {
    uint16_t limit_lo, base_lo;
    uint8_t base_mid, access, gran, base_hi;
};
struct __attribute__((packed)) dtr {
    uint16_t limit;
    uint32_t base;
};
struct __attribute__((packed)) tss {
    uint32_t prev, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs, ldt;
    uint16_t trap, iomap;
};
struct __attribute__((packed)) idt_entry {
    uint16_t off_lo, sel;
    uint8_t zero, flags;
    uint16_t off_hi;
};

static struct gdt_entry gdt[7];
static struct tss main_tss, df_tss;
static uint8_t df_stack[8192] __attribute__((aligned(16)));
static struct idt_entry idt[256];

extern void gdt_flush(void *), tss_flush(void), idt_flush(void *);

static void gdt_set(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
    gdt[i].limit_lo = limit & 0xffff;
    gdt[i].base_lo = base & 0xffff;
    gdt[i].base_mid = (base >> 16) & 0xff;
    gdt[i].access = access;
    gdt[i].gran = (uint8_t)(((limit >> 16) & 0x0f) | (gran & 0xf0));
    gdt[i].base_hi = (base >> 24) & 0xff;
}

static void df_task_entry(void);

void gdt_init(void)
{
    gdt_set(0, 0, 0, 0, 0);
    gdt_set(1, 0, 0xfffff, 0x9a, 0xcf); /* kernel code */
    gdt_set(2, 0, 0xfffff, 0x92, 0xcf); /* kernel data */
    gdt_set(3, 0, 0xfffff, 0xfa, 0xcf); /* user code (DPL3) */
    gdt_set(4, 0, 0xfffff, 0xf2, 0xcf); /* user data (DPL3) */
    memset(&main_tss, 0, sizeof main_tss);
    main_tss.ss0 = KERNEL_DS;
    main_tss.iomap = sizeof(struct tss);
    gdt_set(5, (uint32_t)&main_tss, sizeof main_tss - 1, 0x89, 0x00);
    /* The double-fault handler runs as a separate hardware task with its own
     * known-good stack, so even a blown kernel stack cannot escalate into a

     * triple fault (CPU reset). */
    memset(&df_tss, 0, sizeof df_tss);
    df_tss.eip = (uint32_t)df_task_entry;
    df_tss.esp = (uint32_t)(df_stack + sizeof df_stack);
    df_tss.ss0 = KERNEL_DS;
    df_tss.esp0 = df_tss.esp;
    df_tss.cs = KERNEL_CS;
    df_tss.ds = df_tss.es = df_tss.ss = df_tss.fs = df_tss.gs = KERNEL_DS;
    df_tss.eflags = 0x2;
    df_tss.iomap = sizeof(struct tss);
    gdt_set(6, (uint32_t)&df_tss, sizeof df_tss - 1, 0x89, 0x00);
    struct dtr d = {sizeof gdt - 1, (uint32_t)gdt};
    gdt_flush(&d);
    tss_flush();
}

void tss_set_kernel_stack(uint32_t esp0) { main_tss.esp0 = esp0; }

/* paging must be on before cr3 is meaningful; called from mm_init */
void df_tss_set_cr3(uint32_t cr3);
void df_tss_set_cr3(uint32_t cr3) { df_tss.cr3 = cr3; }

extern bool df_expected;
bool df_expected;

static void df_task_entry(void)
{
    /* The CPU saved the faulting context into main_tss during the task switch. */
    kprintf("\n!!! DOUBLE FAULT caught by task-gate handler (separate TSS + stack)\n");
    kprintf("    faulting context: EIP=%08x ESP=%08x EBP=%08x thread=%s\n", main_tss.eip, main_tss.esp,
            main_tss.ebp, current ? current->name : "?");
    kprintf("    triple fault averted; halting cleanly\n");
    if (df_expected) {
        kprintf("SCENARIO df: PASS\n");
        qemu_exit(QEMU_EXIT_PASS);

    }
    panic("unexpected double fault");
}

/* ---------------- IDT ---------------- */
static void idt_set(int n, uint32_t off, uint16_t sel, uint8_t flags)
{
    idt[n].off_lo = off & 0xffff;
    idt[n].sel = sel;
    idt[n].zero = 0;
    idt[n].flags = flags;
    idt[n].off_hi = (off >> 16) & 0xffff;
}

#define X(n) extern void isr##n(void);
X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)
X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)
X(32) X(33) X(34) X(35) X(36) X(37) X(38) X(39) X(40) X(41) X(42) X(43) X(44) X(45) X(46) X(47) X(128)
#undef X

static void (*const stubs[48])(void) = {
    isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7, isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23, isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
    isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39, isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47,
};

void idt_init(void)
{

    for (int i = 0; i < 48; i++)
        idt_set(i, (uint32_t)stubs[i], KERNEL_CS, 0x8e); /* present, DPL0, 32-bit interrupt gate */
    idt_set(8, 0, DF_TSS_SEL, 0x85);                     /* task gate -> double-fault TSS */
    idt_set(0x80, (uint32_t)isr128, KERNEL_CS, 0xee);    /* DPL3: callable from user mode */
    struct dtr d = {sizeof idt - 1, (uint32_t)idt};
    idt_flush(&d);
}

/* ---------------- PIC / PIT ---------------- */
void pic_init(void)
{
    outb(0x20, 0x11);
    outb(0xa0, 0x11);
    outb(0x21, 0x20); /* master vectors 32..39 */
    outb(0xa1, 0x28); /* slave vectors 40..47 */
    outb(0x21, 0x04);
    outb(0xa1, 0x02);
    outb(0x21, 0x01);
    outb(0xa1, 0x01);
    outb(0x21, 0xfe); /* unmask IRQ0 only */
    outb(0xa1, 0xff);
}

volatile uint32_t ticks;
void (*timer_hook)(uint32_t tick);

static void timer_irq(struct regs *r)
{
    (void)r;
    ticks++;
    if (timer_hook)
        timer_hook(ticks);
    sched_tick();
}

void pit_init(uint32_t hz)
{
    uint32_t div = 1193182 / hz;
    outb(0x43, 0x36);
    outb(0x40, div & 0xff);
    outb(0x40, (div >> 8) & 0xff);
    irq_register(0, timer_irq);
}

static irq_handler_t irq_handlers[16];
void irq_register(int irq, irq_handler_t h) { irq_handlers[irq] = h; }

static const char *const exc_names[32] = {
    "#DE Divide Error", "#DB Debug", "NMI", "#BP Breakpoint", "#OF Overflow", "#BR Bound Range",
    "#UD Invalid Opcode", "#NM Device Not Available", "#DF Double Fault", "Coprocessor Overrun",
    "#TS Invalid TSS", "#NP Segment Not Present", "#SS Stack Fault", "#GP General Protection",
    "#PF Page Fault", "reserved", "#MF x87 FP", "#AC Alignment Check", "#MC Machine Check",
    "#XM SIMD FP", "#VE Virtualization", "#CP Control Protection",
};

void dump_regs(const struct regs *r)
{
    kprintf("  EAX=%08x EBX=%08x ECX=%08x EDX=%08x\n", r->eax, r->ebx, r->ecx, r->edx);
    kprintf("  ESI=%08x EDI=%08x EBP=%08x ESP=%08x\n", r->esi, r->edi, r->ebp,
            (r->cs & 3) ? r->useresp : (uint32_t)&r->useresp);
    kprintf("  EIP=%08x CS=%04x DS=%04x EFLAGS=%08x CR2=%08x CR3=%08x\n", r->eip, r->cs, r->ds, r->eflags,
            read_cr2(), read_cr3());
}

void isr_dispatch(struct regs *r);
void isr_dispatch(struct regs *r)
{
    uint32_t n = r->int_no;
    if (n == 0x80) {
        syscall_dispatch(r);
    } else if (n >= 32 && n < 48) {
        uint32_t irq = n - 32;
        if (irq != 0)
            trace_emit(EV_IRQ, irq, 0);
        if (irq >= 8)
            outb(0xa0, 0x20);
        outb(0x20, 0x20); /* EOI before a possible context switch */
        if (irq_handlers[irq])
            irq_handlers[irq](r);
    } else {
        if (n == 14 && page_fault_handler(r))
            return;
        const char *name = n < 22 ? exc_names[n] : "reserved";
        if (r->cs & 3) {
            /* Fault in ring 3: the kernel survives; only the task dies. */
            kprintf("[user] %s in ring 3 at EIP=%08x (err=%x) -> killing thread %u '%s'\n", name, r->eip,
                    r->err_code, current->tid, current->name);
            dump_regs(r);
            irq_enable();
            thread_exit(128 + (int)n);
        }
        kprintf("\n!!! EXCEPTION %u (%s) err=%08x in kernel mode, thread=%s\n", n, name, r->err_code,
                current ? current->name : "?");
        dump_regs(r);
        panic("unhandled exception %u", n);
    }
    if (need_resched) {
        need_resched = false;
        schedule();
    }
}
