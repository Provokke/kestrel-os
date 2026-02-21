/* Kestrel kernel entry point and boot-time test scenarios.
 * The scenario is chosen from the multiboot command line: test=<name>. */
#include "kernel.h"
#include "kbpf.h"

#define MB_INFO_MAGIC 0x2badb002u

struct multiboot_info {
    uint32_t flags, mem_lower, mem_upper, boot_device, cmdline;
};

static char cmdline[256];
extern char kernel_end[];
extern bool df_expected;

static const char *cmdline_get(const char *key)
{
    static char val[64];
    size_t kl = strlen(key);
    for (const char *p = cmdline; *p; p++) {
        if ((p == cmdline || p[-1] == ' ') && !strncmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1;
            size_t n = 0;
            while (*p && *p != ' ' && n < sizeof val - 1)
                val[n++] = *p++;
            val[n] = 0;
            return val;
        }
    }
    return 0;
}

static __attribute__((noreturn)) void finish(const char *name, bool ok)
{
    kprintf("SCENARIO %s: %s\n", name, ok ? "PASS" : "FAIL");
    qemu_exit(ok ? QEMU_EXIT_PASS : QEMU_EXIT_FAIL);
}

/* ---- scenario: timer ---- */
static bool scenario_timer(void)
{
    uint32_t start = ticks;
    uint32_t last = start;
    for (int i = 1; i <= 5; i++) {
        while (ticks < start + (uint32_t)i * 10)
            __asm__ volatile("hlt");
        kprintf("[timer] tick %u (+%u)\n", ticks, ticks - last);
        last = ticks;
    }
    return ticks - start >= 50;
}

/* ---- scenario: sched ---- */
static void busy_worker(void *arg)
{
    char id = (char)(uint32_t)arg;
    for (int i = 0; i < 6; i++) {
        uint32_t until = ticks + 3;
        while ((int32_t)(ticks - until) < 0)  
            ; /* busy: only preemption lets others run */
        kprintf("[sched] thread %c iteration %d tick=%u\n", id, i, ticks);
    }
}

static bool scenario_sched(void)
{
    struct thread *t[3];
    for (int i = 0; i < 3; i++)
        t[i] = thread_create("busy", busy_worker, (void *)(uint32_t)('A' + i));
    uint32_t sw0 = context_switches;
    for (int i = 0; i < 3; i++)
        thread_join(t[i]);
    kprintf("[sched] context switches during run: %u\n", context_switches - sw0);
    return context_switches - sw0 > 6;
}

/* ---- scenario: pf ---- */
static bool scenario_pf(void)
{
    volatile uint32_t *p = (volatile uint32_t *)(DEMAND_BASE + 0x10000);
    uint32_t before = demand_faults;
    p[0] = 0xdeadbeef;
    p[1024] = 0xcafef00d; /* next page: second fault */
    kprintf("[pf] read back %08x %08x, demand faults handled: %u\n", p[0], p[1024], demand_faults - before);
    if (demand_faults - before != 2 || p[0] != 0xdeadbeef)
        return false;
    kprintf("[pf] now touching an unmapped kernel address (expect a fatal fault dump)\n");
    (void)*(volatile uint32_t *)0x0badb000; /* outside every mapping -> panic */
    return false;
}

/* ---- scenario: user ---- */
static bool scenario_user(void)
{
    int c0 = run_user_program(0);
    int c1 = run_user_program(1);
    int c2 = run_user_program(2);
    kprintf("[user] exit codes: normal=%d privileged=%d kernel-read=%d, syscalls=%u\n", c0, c1, c2, syscall_count);
    return c0 == 42 && c1 == 128 + 13 && c2 == 128 + 14;
}

/* ---- scenario: df ---- */
static volatile int recurse_forever = 1;
static uint32_t recurse(uint32_t n)
{
    if (!recurse_forever)
        return n;
    volatile char pad[256];
    pad[0] = (char)n;
    return recurse(n + 1) + pad[0];
}
static void overflow_thread(void *arg)
{
    (void)arg;
    kprintf("[df] recursing until the kernel stack hits its guard page...\n");
    recurse(0);
}
static bool scenario_df(void)
{
    df_expected = true;
    thread_join(thread_create("overflow", overflow_thread, 0));
    return false; /* unreachable: the double-fault task ends the run */
}

/* ---- scenario: exc ---- */
static bool scenario_exc(void)
{
    kprintf("[exc] executing ud2 with marker registers\n");
    __asm__ volatile("mov $0x11111111, %%eax\n mov $0x22222222, %%ebx\n mov $0x33333333, %%ecx\n"
                     "mov $0x44444444, %%edx\n ud2" ::: "eax", "ebx", "ecx", "edx");
    return false;
}

/* ---- scenario: trace ---- */
static bool scenario_trace(void)
{
    /* kbpf filter: drop kmalloc events and write() syscalls, keep the rest */
    static const struct kbpf_insn prog[] = {
        KBPF_I(KBPF_LDF, 0, 0, 0, 0),         KBPF_I(KBPF_JEQI, 0, 0, 4, EV_KMALLOC),
        KBPF_I(KBPF_JNEI, 0, 0, 2, EV_SYSCALL), KBPF_I(KBPF_LDF, 1, 0, 0, 1),
        KBPF_I(KBPF_JEQI, 1, 0, 1, SYS_WRITE), KBPF_I(KBPF_RETI, 0, 0, 0, 1),
        KBPF_I(KBPF_RETI, 0, 0, 0, 0),
    };
    int v = trace_attach_filter(prog, sizeof prog / sizeof prog[0]);
    kprintf("[trace] kbpf filter attached (verifier=%d, %u insns)\n", v, (uint32_t)(sizeof prog / sizeof prog[0]));
    trace_reset();
    struct thread *a = thread_create("tr-a", busy_worker, (void *)'T');
    thread_join(a);
    run_user_program(0);
    struct trace_stats s = trace_get_stats();
    trace_detach_filter();
    trace_dump(40);
    return v == 0 && s.recorded > 0 && s.filtered > 0;
}

void kmain(uint32_t magic, struct multiboot_info *mbi);
void kmain(uint32_t magic, struct multiboot_info *mbi)
{
    serial_init();
    vga_init();
    if (magic != MB_INFO_MAGIC)
        panic("bad multiboot magic %08x", magic);
    if ((mbi->flags & 4) && mbi->cmdline) {
        const char *c = (const char *)mbi->cmdline;
        size_t n = strlen(c);
        memcpy(cmdline, c, n < sizeof cmdline - 1 ? n : sizeof cmdline - 1);
    }
    kprintf("Kestrel OS booting (i686, multiboot) cmdline='%s'\n", cmdline);
    gdt_init();
    idt_init();
    kprintf("[cpu] GDT+TSS loaded, IDT installed (double fault -> task gate)\n");
    mm_init(mbi->mem_upper, (uint32_t)kernel_end);
    sched_init();
    pic_init();
    pit_init(HZ);
    irq_enable();
    kprintf("[boot] PIT at %u Hz, interrupts on; boot complete\n", HZ);

    const char *test = cmdline_get("test");
    if (!test)
        test = "all";
    if (!strcmp(test, "boot"))
        finish("boot", true);
    if (!strcmp(test, "ktest"))
        finish("ktest", ktest_run_all() == 0);
    if (!strcmp(test, "timer"))
        finish("timer", scenario_timer());
    if (!strcmp(test, "sched"))
        finish("sched", scenario_sched());

    if (!strcmp(test, "pf"))
        finish("pf", scenario_pf());
    if (!strcmp(test, "user"))
        finish("user", scenario_user());
    if (!strcmp(test, "df"))
        finish("df", scenario_df());
    if (!strcmp(test, "exc"))
        finish("exc", scenario_exc());
    if (!strcmp(test, "tf")) {
        /* Harness control: deliberately triple-fault (empty IDT + int3) to
         * prove the test runner detects a CPU reset instead of a clean exit. */
        kprintf("[tf] loading an empty IDT and raising int3 -> triple fault\n");
        static const struct __attribute__((packed)) { uint16_t l; uint32_t b; } null_idt = {0, 0};
        irq_disable();
        __asm__ volatile("lidt %0; int3" ::"m"(null_idt));
    }
    if (!strcmp(test, "trace"))
        finish("trace", scenario_trace());
    if (!strcmp(test, "all")) {
        bool ok = ktest_run_all() == 0;
        ok &= scenario_timer();
        ok &= scenario_sched();
        ok &= scenario_user();
        mm_dump_stats();
        trace_dump(12);
        finish("all", ok);
    }
    kprintf("unknown test '%s'\n", test);
    finish(test, false);
}
