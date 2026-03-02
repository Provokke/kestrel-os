/* COM1 serial driver, VGA text console and kprintf. */
#include "kernel.h"

#define COM1 0x3f8

void serial_init(void)
{
    outb(COM1 + 1, 0x00); /* no interrupts */
    outb(COM1 + 3, 0x80); /* DLAB */
    outb(COM1 + 0, 0x01); /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* 8N1 */
    outb(COM1 + 2, 0xc7); /* FIFO */
    outb(COM1 + 4, 0x03);
}

void serial_putc(char c)
{
    if (c == '\n')
        serial_putc('\r');
    while (!(inb(COM1 + 5) & 0x20))
        cpu_relax();
    outb(COM1, (uint8_t)c);
}

static volatile uint16_t *const vga = (volatile uint16_t *)0xb8000;
static int vrow, vcol;
static const uint8_t vattr = 0x0f;

void vga_init(void)
{
    for (int i = 0; i < 80 * 25; i++)
        vga[i] = (uint16_t)(vattr << 8 | ' ');
    vrow = vcol = 0;
}

void vga_putc(char c)
{
    if (c == '\n') {
        vcol = 0;
        vrow++;
    } else if (c == '\r') {
        vcol = 0;
    } else {
        vga[vrow * 80 + vcol] = (uint16_t)(vattr << 8 | (uint8_t)c);
        if (++vcol == 80) {
            vcol = 0;
            vrow++;
        }
    }
    if (vrow == 25) { /* scroll */
        for (int i = 0; i < 80 * 24; i++)
            vga[i] = vga[i + 80];
        for (int i = 80 * 24; i < 80 * 25; i++)


            vga[i] = (uint16_t)(vattr << 8 | ' ');
        vrow = 24;
    }
}

uint16_t vga_cell(int row, int col) { return vga[row * 80 + col]; }
int vga_row(void) { return vrow; }

static struct spinlock console_lock = SPINLOCK_INIT("console");

void console_write(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        serial_putc(s[i]);
        vga_putc(s[i]);
    }
}

struct outbuf {
    char *buf;
    size_t cap, len;
};

static void ob_put(struct outbuf *o, char c)
{
    if (o->len + 1 < o->cap)
        o->buf[o->len] = c;
    o->len++;
}

static void fmt_num(struct outbuf *o, uint32_t v, unsigned base, int width, char pad, bool neg)
{
    char tmp[16];
    int n = 0;
    do {
        tmp[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    if (neg)
        tmp[n++] = '-';
    while (width-- > n)
        ob_put(o, pad);
    while (n)
        ob_put(o, tmp[--n]);
}

static void vformat(struct outbuf *o, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            ob_put(o, *fmt);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0;
        bool left = false;
        if (*fmt == '-') {
            left = true;
            fmt++;
        }
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        switch (*fmt) {
        case 'd': {
            int v = va_arg(ap, int);
            fmt_num(o, v < 0 ? (uint32_t)-(int64_t)v : (uint32_t)v, 10, width, pad, v < 0);
            break;
        }
        case 'u': fmt_num(o, va_arg(ap, uint32_t), 10, width, pad, false); break;
        case 'x': fmt_num(o, va_arg(ap, uint32_t), 16, width, pad, false); break;
        case 'p':
            ob_put(o, '0');
            ob_put(o, 'x');
            fmt_num(o, (uint32_t)va_arg(ap, void *), 16, 8, '0', false);
            break;
        case 'c': ob_put(o, (char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int l = (int)strlen(s);

            while (!left && width-- > l)
                ob_put(o, ' ');
            while (*s)
                ob_put(o, *s++);
            while (left && width-- > l)
                ob_put(o, ' ');
            break;
        }
        case '%': ob_put(o, '%'); break;
        default: ob_put(o, '?'); break;
        }
    }
}

int ksnprintf(char *buf, size_t n, const char *fmt, ...)
{
    struct outbuf o = {buf, n, 0};
    va_list ap;
    va_start(ap, fmt);
    vformat(&o, fmt, ap);
    va_end(ap);
    if (n)
        buf[o.len < n ? o.len : n - 1] = 0;
    return (int)o.len;
}

void kvprintf(const char *fmt, va_list ap)
{
    char buf[512];
    struct outbuf o = {buf, sizeof buf, 0};
    vformat(&o, fmt, ap);
    size_t len = o.len < sizeof buf ? o.len : sizeof buf - 1;
    uint32_t f;
    spin_lock_irqsave(&console_lock, &f); /* whole lines never interleave */
    console_write(buf, len);
    spin_unlock_irqrestore(&console_lock, f);
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

void qemu_exit(uint8_t code)
{
    irq_disable();
    outb(0xf4, code); /* isa-debug-exit */
    for (;;)
        __asm__ volatile("hlt");
}

void panic(const char *fmt, ...)
{
    irq_disable();
    console_lock.locked = 0; /* break the lock: we may have died holding it */
    char buf[256];
    struct outbuf o = {buf, sizeof buf, 0};
    va_list ap;
    va_start(ap, fmt);
    vformat(&o, fmt, ap);
    va_end(ap);
    size_t len = o.len < sizeof buf ? o.len : sizeof buf - 1;
    console_write("\n*** KERNEL PANIC: ", 19);
    console_write(buf, len);
    console_write("\n", 1);
    trace_dump(16);
    qemu_exit(QEMU_EXIT_PANIC);
}
