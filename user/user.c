/* Tiny freestanding ring-3 program embedded in the kernel image.
 * Mode (passed in EAX at entry) selects the behaviour:
 *   0: exercise syscalls, including deliberately bad pointers -> exit(42)
 *   1: execute a privileged instruction (cli) -> killed by #GP
 *   2: dereference a kernel address -> killed by #PF */
#include <stdint.h>

static inline int sc(int n, int a, int b, int c)
{
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}

static int slen(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static void puts_u(const char *s) { sc(1, 1, (int)(uintptr_t)s, slen(s)); }

static void put_int(const char *label, int v)
{
    char buf[48];
    int i = 0;
    while (label[i]) {
        buf[i] = label[i];
        i++;
    }
    char tmp[12];
    int n = 0;
    unsigned u = v < 0 ? (unsigned)-v : (unsigned)v;
    do {
        tmp[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (v < 0)
        buf[i++] = '-';
    while (n)
        buf[i++] = tmp[--n];
    buf[i++] = '\n';
    sc(1, 1, (int)(uintptr_t)buf, i);
}

static volatile int counter_bss;

int umain(int mode);
int umain(int mode)
{
    if (mode == 1) {
        puts_u("user: about to execute cli in ring 3\n");
        __asm__ volatile("cli");
        puts_u("user: ERROR still alive\n");
        return 1;
    }
    if (mode == 2) {
        puts_u("user: about to read kernel memory at 0x00100000\n");
        volatile int x = *(volatile int *)0x00100000;
        (void)x;
        puts_u("user: ERROR still alive\n");

        return 1;
    }
    puts_u("user: hello from ring 3!\n");
    put_int("user: getpid() = ", sc(2, 0, 0, 0));
    int r = sc(1, 1, 0x00100000, 16); /* kernel pointer */
    put_int("user: write(kernel ptr) = ", r);
    int r2 = sc(1, 1, 0x40800000, 16); /* unmapped user-range pointer */
    put_int("user: write(unmapped ptr) = ", r2);
    int r3 = sc(1, 1, (int)0xfffffff0, 64); /* wraps around 4 GiB */
    put_int("user: write(wrapping ptr) = ", r3);
    put_int("user: bad syscall = ", sc(99, 0, 0, 0));
    int t0 = sc(5, 0, 0, 0);
    sc(4, 5, 0, 0); /* sleep 5 ticks */
    int t1 = sc(5, 0, 0, 0);
    put_int("user: slept ticks = ", t1 - t0);
    for (int i = 0; i < 3; i++) {
        counter_bss++;
        sc(3, 0, 0, 0); /* yield */
    }
    put_int("user: bss counter = ", counter_bss);
    if (r == -14 && r2 == -14 && r3 == -14 && t1 - t0 >= 5 && counter_bss == 3)
        puts_u("user: all checks passed\n");
    return 42;
}

__attribute__((naked, section(".text.start"))) void _start(void)
{
    __asm__ volatile("push %eax\n"
                     "call umain\n"
                     "mov %eax, %ebx\n"
                     "mov $0, %eax\n"
                     "int $0x80\n"
                     "ud2\n");
}
