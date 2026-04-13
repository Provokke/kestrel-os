/* Kernel tracing: tracepoints record fixed-size events into an overwriting
 * ring buffer; an optional verified kbpf program filters events in-line. */
#include "kernel.h"
#include "kbpf.h"

#define TRACE_SLOTS 256 /* power of two */

static struct trace_rec ring[TRACE_SLOTS];
static uint32_t widx; /* total records written */
static struct trace_stats stats;
static bool enabled = true;
static struct kbpf_insn filter[KBPF_MAX_INSNS];
static uint32_t filter_len;

static const char *const ev_names[EV_MAX] = {

    "?", "sched_switch", "irq", "syscall", "page_fault", "thread_create", "thread_exit", "kmalloc", "user",
};

void trace_emit(uint32_t event, uint32_t a, uint32_t b)
{

    if (!enabled)
        return;
    uint32_t f = irq_save();
    stats.emitted++;
    uint32_t tid = current ? current->tid : 0;
    if (filter_len) {
        uint32_t ctx[4] = {event, a, b, tid};
        if (!kbpf_run(filter, filter_len, ctx)) {
            stats.filtered++;
            irq_restore(f);
            return;
        }
    }
    if (widx >= TRACE_SLOTS)
        stats.overwritten++;
    struct trace_rec *r = &ring[widx++ & (TRACE_SLOTS - 1)];
    r->tick = ticks;
    r->tsc_lo = (uint32_t)rdtsc();
    r->event = (uint16_t)event;
    r->tid = (uint16_t)tid;
    r->a = a;
    r->b = b;
    stats.recorded++;
    irq_restore(f);
}

int trace_attach_filter(const struct kbpf_insn *prog, uint32_t len)
{
    int v = kbpf_verify(prog, len, 4);
    if (v != KBPF_OK)
        return v;
    uint32_t f = irq_save();
    memcpy(filter, prog, len * sizeof *prog);
    filter_len = len;
    irq_restore(f);
    return 0;
}

void trace_detach_filter(void)
{
    uint32_t f = irq_save();
    filter_len = 0;
    irq_restore(f);
}

void trace_reset(void)
{
    uint32_t f = irq_save();
    widx = 0;
    memset(&stats, 0, sizeof stats);
    irq_restore(f);
}

void trace_set_enabled(bool on) { enabled = on; }
struct trace_stats trace_get_stats(void) { return stats; }

uint32_t trace_snapshot(struct trace_rec *out, uint32_t max)
{
    uint32_t f = irq_save();
    uint32_t n = widx < TRACE_SLOTS ? widx : TRACE_SLOTS;
    if (n > max)
        n = max;
    for (uint32_t i = 0; i < n; i++)
        out[i] = ring[(widx - n + i) & (TRACE_SLOTS - 1)];
    irq_restore(f);
    return n;
}

void trace_dump(uint32_t max)
{
    static struct trace_rec snap[TRACE_SLOTS];
    bool was = enabled;
    enabled = false;
    uint32_t n = trace_snapshot(snap, max);
    kprintf("[trace] dump: last %u events (emitted=%u recorded=%u filtered=%u overwritten=%u)\n", n, stats.emitted,
            stats.recorded, stats.filtered, stats.overwritten);
    for (uint32_t i = 0; i < n; i++) {
        struct trace_rec *r = &snap[i];
        kprintf("[trace] t=%5u tsc=%08x tid=%2u %-14s a=%08x b=%08x\n", r->tick, r->tsc_lo, r->tid,
                r->event < EV_MAX ? ev_names[r->event] : "?", r->a, r->b);
    }
    enabled = was;
}
