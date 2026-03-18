/* Preemptive round-robin scheduler, kernel threads, sleep, wait queues and
 * IRQ-safe spinlocks (uniprocessor: the lock word still uses an atomic xchg). */
#include "kernel.h"

#define STACK_PAGES 3 /* 1 guard page (unmapped) + 2 stack pages */
#define TIMESLICE_TICKS 2

struct thread *current;
static struct thread boot_thread, *idle_thread;
static struct thread *rq_head, *rq_tail;
static struct thread *sleepers;
static struct thread *zombies; /* detached threads awaiting reaping */
static uint32_t next_tid;
static uint32_t slice_left = TIMESLICE_TICKS;
volatile bool need_resched;
uint32_t context_switches;

/* ---------------- spinlocks ---------------- */
void spin_lock_irqsave(struct spinlock *l, uint32_t *flags)
{
    *flags = irq_save();
    if (l->locked && l->owner == current && current)
        panic("spinlock '%s': recursive acquisition by %s", l->name, current->name);
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
        cpu_relax();
    l->owner = current;
}

void spin_unlock_irqrestore(struct spinlock *l, uint32_t flags)
{
    l->owner = 0;
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
    irq_restore(flags);
}

/* ---------------- run queue ---------------- */
static void rq_push(struct thread *t)
{
    t->state = T_READY;
    t->next = 0;
    if (rq_tail)
        rq_tail->next = t;
    else
        rq_head = t;
    rq_tail = t;
}

static struct thread *rq_pop(void)
{
    struct thread *t = rq_head;
    if (t) {
        rq_head = t->next;
        if (!rq_head)
            rq_tail = 0;
        t->next = 0;
    }
    return t;
}

void schedule(void)
{
    struct thread *prev = current;
    if (prev->state == T_RUNNING && prev != idle_thread)
        rq_push(prev);
    struct thread *next = rq_pop();
    if (!next)
        next = (prev->state == T_READY || prev->state == T_RUNNING) ? prev : idle_thread;
    slice_left = TIMESLICE_TICKS;
    if (next == prev) {
        prev->state = T_RUNNING;
        return;
    }
    next->state = T_RUNNING;
    next->switches++;
    context_switches++;
    trace_emit(EV_SCHED_SWITCH, prev->tid, next->tid);
    current = next;
    tss_set_kernel_stack(next->stack_top);
    switch_context(&prev->esp, next->esp);
}

void sched_tick(void)
{
    /* wake sleepers whose deadline passed */
    struct thread **pp = &sleepers;
    while (*pp) {
        struct thread *t = *pp;
        if ((int32_t)(ticks - t->wake_tick) >= 0) {
            *pp = t->next;
            rq_push(t);
        } else {
            pp = &t->next;
        }
    }
    if (current == idle_thread) {
        if (rq_head)
            need_resched = true;
    } else if (--slice_left == 0) {
        need_resched = true;
    }
}

void thread_yield(void)
{
    uint32_t f = irq_save();
    schedule();
    irq_restore(f);
}

void thread_sleep(uint32_t nticks)
{
    uint32_t f = irq_save();
    current->wake_tick = ticks + nticks;
    current->state = T_SLEEPING;
    current->next = sleepers;
    sleepers = current;
    schedule();
    irq_restore(f);
}

void wq_sleep(struct waitqueue *wq)
{
    current->state = T_BLOCKED;
    current->next = wq->head;
    wq->head = current;
    schedule();
}

void wq_wake_all(struct waitqueue *wq)
{
    uint32_t f = irq_save();
    while (wq->head) {
        struct thread *t = wq->head;
        wq->head = t->next;
        rq_push(t);
    }
    irq_restore(f);
}

/* ---------------- threads ---------------- */
void thread_start_c(void);
void thread_start_c(void)
{
    irq_enable(); /* we arrive here from schedule() with interrupts off */
    current->fn(current->arg);
    thread_exit(0);
}

extern void thread_trampoline(void);

struct thread *thread_create(const char *name, void (*fn)(void *), void *arg)
{
    struct thread *t = kmalloc(sizeof *t);
    if (!t)
        return 0;
    memset(t, 0, sizeof *t);
    uint32_t base = pmm_alloc_frames(STACK_PAGES);
    if (!base) {
        kfree(t);
        return 0;
    }
    vmm_unmap(base); /* guard page: a stack overflow faults instead of corrupting memory */
    t->stack_base = base;
    t->stack_top = base + STACK_PAGES * PAGE_SIZE;
    uint32_t *sp = (uint32_t *)t->stack_top;
    *--sp = 0;                          /* fake return address for the trampoline */
    *--sp = (uint32_t)thread_trampoline; /* switch_context's ret target */
    *--sp = 0;                          /* ebp */
    *--sp = 0;                          /* ebx */
    *--sp = 0;                          /* esi */
    *--sp = 0;                          /* edi */
    t->esp = (uint32_t)sp;
    t->fn = fn;
    t->arg = arg;
    uint32_t f = irq_save();
    t->tid = ++next_tid;
    irq_restore(f);
    size_t n = strlen(name);
    if (n > sizeof t->name - 1)
        n = sizeof t->name - 1;
    memcpy(t->name, name, n);
    trace_emit(EV_THREAD_CREATE, t->tid, 0);
    f = irq_save();
    rq_push(t);
    irq_restore(f);
    return t;
}

static void thread_free(struct thread *t)
{
    vmm_map(t->stack_base, t->stack_base, PTE_W); /* restore identity mapping of guard */
    pmm_free_frames(t->stack_base, STACK_PAGES);
    kfree(t);
}

void thread_exit(int code)
{
    irq_disable();
    struct thread *t = current;
    t->exit_code = code;
    t->state = T_DEAD;
    trace_emit(EV_THREAD_EXIT, t->tid, (uint32_t)code);
    if (t->detached) {
        t->next = zombies;
        zombies = t;
    }
    wq_wake_all(&t->exit_wq);

    schedule();
    panic("dead thread %s rescheduled", t->name);
}

int thread_join(struct thread *t)
{
    wait_event(&t->exit_wq, t->state == T_DEAD);
    int code = t->exit_code;
    thread_free(t);
    return code;
}

void thread_detach(struct thread *t)
{
    uint32_t f = irq_save();
    t->detached = true;
    if (t->state == T_DEAD) { /* already exited: reap via idle */
        t->next = zombies;
        zombies = t;
    }
    irq_restore(f);
}

static void idle_fn(void *arg)
{
    (void)arg;
    for (;;) {
        irq_disable();
        while (zombies) {
            struct thread *z = zombies;
            zombies = z->next;
            irq_enable();
            thread_free(z);
            irq_disable();
        }
        if (rq_head) {
            schedule();
            irq_enable();
            continue;
        }
        __asm__ volatile("sti; hlt" ::: "memory"); /* atomic enable+halt: no lost wakeup */
    }
}

void sched_init(void)
{
    memcpy(boot_thread.name, "kmain", 6);
    boot_thread.state = T_RUNNING;
    extern char boot_stack_top[];
    boot_thread.stack_top = (uint32_t)boot_stack_top;
    current = &boot_thread;
    idle_thread = thread_create("idle", idle_fn, 0);
    /* idle is only run when nothing else is runnable: take it off the queue */
    uint32_t f = irq_save();
    rq_head = rq_tail = 0;
    idle_thread->state = T_READY;
    irq_restore(f);
}
