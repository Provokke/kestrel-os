/* int 0x80 system calls, user-pointer validation and copy helpers, and the
 * loader for the embedded ring-3 program. */
#include "kernel.h"

uint32_t syscall_count;

/* A user range is acceptable only if it lies entirely inside the user window
 * (no wrap-around) and every page is present with the U bit (and W if writing). */
int access_ok_user(uint32_t uaddr, uint32_t len, bool write)
{
    if (len == 0)
        return 0;
    if (uaddr < USER_BASE || uaddr >= USER_END || len > USER_END - uaddr)
        return -EFAULT;
    for (uint32_t pg = uaddr & ~0xfffu; pg < uaddr + len; pg += PAGE_SIZE) {
        uint32_t fl;
        if (!vmm_translate(pg, &fl) || !(fl & PTE_U) || (write && !(fl & PTE_W)))
            return -EFAULT;
    }
    return 0;
}

int copy_from_user(void *dst, uint32_t usrc, uint32_t len)
{
    int e = access_ok_user(usrc, len, false);
    if (e)
        return e;
    memcpy(dst, (const void *)usrc, len);

    return 0;
}

int copy_to_user(uint32_t udst, const void *src, uint32_t len)
{
    int e = access_ok_user(udst, len, true);
    if (e)
        return e;
    memcpy((void *)udst, src, len);
    return 0;
}

static int sys_write(uint32_t fd, uint32_t ubuf, uint32_t len)
{
    if (fd != 1 && fd != 2)
        return -EINVAL;
    char kbuf[129];
    uint32_t done = 0;
    while (done < len) {
        uint32_t chunk = len - done > 128 ? 128 : len - done;
        int e = copy_from_user(kbuf, ubuf + done, chunk);
        if (e)
            return e;
        kbuf[chunk] = 0;
        kprintf("%s", kbuf); /* goes through the console lock */
        done += chunk;
    }
    return (int)done;
}

void syscall_dispatch(struct regs *r)
{
    uint32_t nr = r->eax;
    int ret;
    syscall_count++;
    switch (nr) {
    case SYS_EXIT:
        trace_emit(EV_SYSCALL, nr, r->ebx);

        irq_enable();
        thread_exit((int)r->ebx);
    case SYS_WRITE: ret = sys_write(r->ebx, r->ecx, r->edx); break;
    case SYS_GETPID: ret = (int)current->tid; break;
    case SYS_YIELD: thread_yield(); ret = 0; break;
    case SYS_SLEEP: thread_sleep(r->ebx); ret = 0; break;
    case SYS_UPTIME: ret = (int)ticks; break;
    default: ret = -ENOSYS; break;
    }
    trace_emit(EV_SYSCALL, nr, (uint32_t)ret);
    r->eax = (uint32_t)ret;
}

extern const char user_bin_start[], user_bin_end[];

static uint32_t user_frames[(USER_END - USER_BASE) / PAGE_SIZE];

static void user_thread(void *arg)
{
    uint32_t mode = (uint32_t)arg;
    kprintf("[user] entering ring 3: entry=%08x stack=%08x mode=%u\n", USER_BASE, USER_STACK_TOP, mode);
    enter_usermode(USER_BASE, USER_STACK_TOP - 16, mode);
}

int run_user_program(uint32_t mode)
{
    uint32_t size = (uint32_t)(user_bin_end - user_bin_start);
    uint32_t code_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE + 1; /* +1 for .bss slack */
    uint32_t idx = 0;
    for (uint32_t i = 0; i < code_pages; i++) {
        uint32_t pa = pmm_alloc_frame();
        memset((void *)pa, 0, PAGE_SIZE);
        vmm_map(USER_BASE + i * PAGE_SIZE, pa, PTE_U | PTE_W);
        user_frames[idx++] = pa;
    }
    for (uint32_t i = 1; i <= USER_STACK_PAGES; i++) {
        uint32_t pa = pmm_alloc_frame();
        memset((void *)pa, 0, PAGE_SIZE);
        vmm_map(USER_STACK_TOP - i * PAGE_SIZE, pa, PTE_U | PTE_W);
        user_frames[idx++] = pa;
    }
    memcpy((void *)USER_BASE, user_bin_start, size);
    struct thread *t = thread_create("user", user_thread, (void *)mode);
    int code = thread_join(t);
    /* tear down the address-space window */
    for (uint32_t i = 0; i < code_pages; i++)
        vmm_unmap(USER_BASE + i * PAGE_SIZE);
    for (uint32_t i = 1; i <= USER_STACK_PAGES; i++)
        vmm_unmap(USER_STACK_TOP - i * PAGE_SIZE);
    for (uint32_t i = 0; i < idx; i++)
        pmm_free_frame(user_frames[i]);
    kprintf("[user] program (mode %u) exited with code %d\n", mode, code);
    return code;
}
