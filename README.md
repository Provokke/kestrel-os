<div align="center">

# Kestrel OS

A small, original **i686** hobby kernel in C and NASM that demonstrates Linux-kernel-style systems concepts — and, above all, *proves* they work with three layers of automated tests.

![License](https://img.shields.io/badge/LICENSE-MIT-blue?style=for-the-badge)  ![Language](https://img.shields.io/badge/C11-555555?style=for-the-badge)

Refer to [LICENSE](LICENSE) for the legal bits.

</div>

## 🧭 What it does

| Area | What is implemented |
|---|---|
| Boot / CPU | Multiboot v1 entry, GDT, TSS, IDT, 32 exception stubs with full register dumps |
| Interrupts | 8259 PIC remap, PIT timer at 100 Hz, IRQ dispatch with EOI-before-switch |
| Console | COM1 serial driver (polled), VGA text console with scrolling, `kprintf`/`ksnprintf` |
| Memory | Bitmap page-frame allocator (reserved/double-free detection), 4 KiB paging, identity-mapped kernel, demand-paged region, NULL page unmapped, CR0.WP |
| Heap | SLUB-flavoured object caches (partial/full/empty lists, ctor, poisoning, double-free detection) + kmalloc size classes 8..1024 and page-backed large allocations |
| Scheduling | Preemptive round-robin kernel threads, context switch in asm, idle thread with `sti; hlt`, sleep, join/detach/reaping, wait queues |
| Sync | IRQ-save spinlocks (recursion detection), lock-free SPSC ring, atomic MPSC queue (one `xchg` per push) |
| User mode | Ring 3 via `iret`, `int 0x80` syscalls (exit, write, getpid, yield, sleep, uptime), `copy_from_user`/`copy_to_user` with range + page-table validation; faults in ring 3 kill only the task |
| Robustness | Guard page below every kernel stack; **double fault handled by a hardware task gate** with its own TSS and stack, so a kernel stack overflow cannot triple-fault |
| Tracing | Tracepoints (sched_switch, irq, syscall, page_fault, thread create/exit, kmalloc) into an overwriting ring buffer dumped over serial; **kbpf**, an eBPF-inspired verified filter VM attached to tracepoints |

## 🏗️ Architecture

```
                +--------------------- ring 3 ----------------------+
                |  user/user.c  (embedded flat binary @ 0x40000000) |
                +-------------------------|--int 0x80---------------+
                                          v
 +---------------------------------- ring 0 ------------------------------------+
 | kernel/syscall.c  dispatch, access_ok_user, copy_{from,to}_user, loader      |
 | kernel/sched.c    threads, run queue, sleep list, wait queues, spinlocks     |
 | kernel/mm.c       PMM wrapper, page tables, #PF handler, kmalloc/kfree       |
 | kernel/trace.c    tracepoint ring  <---- kbpf filter (lib/kbpf.c, verified)  |
 | kernel/ktest.c    KTEST self-tests (linker section "ktests")                 |
 | kernel/console.c  COM1 + VGA + kprintf      kernel/main.c  scenarios         |
 |------------------------------------------------------------------------------|
 | arch/x86/cpu.c    GDT/TSS/DF-task, IDT, PIC, PIT, exception dumps            |
 | arch/x86/isr.asm  entry stubs, switch_context, enter_usermode               |
 | arch/x86/boot.asm multiboot header, boot stack                              |
 |------------------------------------------------------------------------------|
 | lib/  (portable, compiled BOTH into the kernel and into host tests)          |
 |   bitmap_pmm.c  slab.c  spsc_ring.c  mpsc_queue.c  kbpf.c                    |
 +------------------------------------------------------------------------------+

 Physical/virtual layout (identity mapped 0-64 MiB, supervisor only)
   0x00000000  unmapped NULL page
   0x00100000  kernel image (.text .rodata .data[ktests] .bss)  -> kernel_end
   kernel_end  free frames managed by the bitmap PMM (page tables, slabs, stacks)
   0x40000000  user window (code + 4-page stack, U bit)     4 MiB
   0x50000000  demand-paged kernel region (faults map zeroed frames)
```

Thread stacks are 3 contiguous frames whose lowest frame is unmapped (guard).
Overflowing it raises #PF, which cannot be delivered on the same stack, so the
CPU raises #DF; IDT vector 8 is a *task gate* to a second TSS with a private
stack, so the handler always runs, reports the faulting EIP/ESP from the saved
TSS and exits cleanly instead of resetting the machine.

## 🚀 Quick start

Requirements: `clang`, `ld.lld`, `llvm-objcopy`, `nasm`, `python3`,
`qemu-system-i386` (`apt-get install -y qemu-system-x86`).

```sh
make            # build/kestrel.elf
make run        # boot with test=all on the serial console
make test       # host tests (ASan+UBSan), host tests (TSan), QEMU boot tests
make bench      # host microbenchmarks
```

Pick a boot scenario with the multiboot command line, e.g.
`qemu-system-i386 -kernel build/kestrel.elf -append test=user -serial stdio -display none -device isa-debug-exit,iobase=0xf4,iosize=0x04 -no-reboot`.
Scenarios: `boot ktest timer sched pf exc user df tf trace all`.

## 🧪 Testing

`make test` runs everything and fails on any failure:

1. **Host unit + stress tests** (`tests/host/test_lib.c`): the exact `lib/`
   sources the kernel uses, compiled for Linux with
   `-fsanitize=address,undefined`, then again with `-fsanitize=thread`.
   14 tests: PMM model-checked against a shadow array over 20k random
   alloc/free ops; slab random model (200k ops, scribble detection),
   double/interior free detection, poisoning; kmalloc classes + large;
   4-thread kmalloc; SPSC wrap/overflow of 32-bit indices + 2-thread 5M-item
   stress; MPSC 4 producers x 300k with per-producer FIFO check; kbpf verifier
   rejection cases, semantics, and a 200k-program fuzz loop.
2. **In-kernel KTEST suite** (`kernel/ktest.c`, 20 tests) run at boot.
   Includes a negative control: the same unlocked read-modify-write that the
   spinlock test protects *must* lose updates under preemption.
3. **QEMU boot tests** (`tests/qemu/run_qemu_tests.py`, 11 cases): headless,
   `-serial stdio -display none -no-reboot`, `isa-debug-exit` exit codes
   (33 pass, 35 fail, 37 panic), per-case timeout, ordered regex assertions
   plus structural checks (thread interleaving, tick spacing, KTEST counts).
   `triple_fault_control` deliberately triple-faults and asserts the harness
   sees QEMU's reset exit (status 0), proving resets cannot pass as success.

Real output of `make test` (QEMU part):

```
[PASS] qemu:boot                     exit=33   0.1s
[PASS] qemu:ktest                    exit=33   1.2s
[PASS] qemu:timer                    exit=33   0.6s
[PASS] qemu:sched                    exit=33   0.5s
[PASS] qemu:page_fault               exit=37   0.1s
[PASS] qemu:exception_dump           exit=37   0.1s
[PASS] qemu:user_syscalls            exit=33   0.2s
[PASS] qemu:triple_fault_protection  exit=33   0.1s
[PASS] qemu:triple_fault_control     exit=0   0.1s
[PASS] qemu:tracing                  exit=33   0.3s
[PASS] qemu:all                      exit=33   2.2s
qemu boot tests: 11 passed, 0 failed
```

KTEST output (test=ktest):

```
KTEST pmm_alloc_free_unique        PASS (0 ticks)
KTEST pmm_double_free_rejected     PASS (0 ticks)
KTEST pmm_contiguous               PASS (0 ticks)
KTEST kmalloc_size_classes         PASS (0 ticks)
KTEST slab_cache_ctor_reuse        PASS (0 ticks)
KTEST paging_map_translate_unmap   PASS (0 ticks)
KTEST page_fault_demand_paging     PASS (0 ticks)
KTEST user_copy_validation         PASS (0 ticks)
KTEST preemption_round_robin       PASS (22 ticks)
KTEST sleep_duration               PASS (10 ticks)
KTEST spinlock_mutual_exclusion    PASS (21 ticks)
KTEST race_detected_without_lock   PASS (21 ticks)

KTEST waitqueue_sleep_wakeup       PASS (5 ticks)
KTEST thread_exit_code_and_reaping PASS (3 ticks)
KTEST spsc_irq_to_thread           PASS (30 ticks)
KTEST mpsc_multi_producer          PASS (0 ticks)
KTEST trace_kbpf_filter            PASS (0 ticks)
KTEST trace_ring_overwrite         PASS (0 ticks)
KTEST vga_text_console             PASS (0 ticks)
KTEST ksnprintf_formats            PASS (0 ticks)
```

Sample serial output (excerpts):

```
[sched] thread A iteration 0 tick=127
[sched] thread B iteration 0 tick=129
[sched] thread C iteration 0 tick=131
[sched] thread A iteration 1 tick=133
...
user: hello from ring 3!
user: write(kernel ptr) = -14
user: write(unmapped ptr) = -14
user: write(wrapping ptr) = -14
user: bad syscall = -38
user: slept ticks = 5
user: all checks passed
[user] program (mode 0) exited with code 42
[user] #GP General Protection in ring 3 at EIP=4000008a (err=0) -> killing thread 29 'user'
  EAX=00000025 EBX=00000001 ECX=40000963 EDX=00000025
  ESI=00000001 EDI=00000000 EBP=403fffe4 ESP=403fff88
  EIP=4000008a CS=001b DS=0023 EFLAGS=00000246 CR2=50002004 CR3=00117000
...
[df] recursing until the kernel stack hits its guard page...
!!! DOUBLE FAULT caught by task-gate handler (separate TSS + stack)
    faulting context: EIP=001034d9 ESP=0012d000 EBP=0012d008 thread=overflow
    triple fault averted; halting cleanly
...
[trace] kbpf filter attached (verifier=0, 7 insns)
[trace] dump: last 19 events (emitted=33 recorded=19 filtered=14 overwritten=0)
[trace] t=   19 tsc=218e0d56 tid= 3 syscall        a=00000002 b=00000003
```

## ⏱️ Benchmarks

`make bench` on the shared 4-core build VM (host, -O2, numbers vary run to run):

```
kmalloc/kfree (slab):     35.7 ns/op   (glibc malloc/free:    6.6 ns/op)
pmm alloc+free frame:      9.1 ns/pair
SPSC ring (2 threads):    52.5 M items/s
MPSC queue (4P/1C):       17.3 M items/s
```

The slab path is slower than glibc because every `kfree` walks the slab's
freelist to detect double frees (a debugging feature, O(objects per slab)).

## ⚠️ Honest limits

- Uniprocessor only; spinlocks are correct but only ever contend via IRQs.
  PIC + PIT, no APIC/HPET.
- One shared address space: a single user window used by one user task at a
  time (no per-process page directories, no ELF loader, no fork/exec).
- `copy_from_user` validates by walking page tables up front (no exception
  fixup table), which is race-free only because there is one CPU and the user
  window is not unmapped concurrently.
- PMM only manages the identity-mapped first 64 MiB and uses `mem_upper`
  rather than the full multiboot memory map.
- kbpf has 4 registers, forward-only jumps and no maps/helpers.
- Serial output is polled; no input driver.
- Slab double-free detection costs a freelist walk per free (see benchmarks).

## Licence

MIT — see [LICENSE](LICENSE).
