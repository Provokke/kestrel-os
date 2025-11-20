#!/usr/bin/env python3
"""Headless QEMU boot tests for Kestrel.

Each case boots the kernel with `-append test=<scenario>`, captures COM1 via
-serial stdio, enforces a timeout, and checks (a) the isa-debug-exit status and
(b) ordered regex assertions over the serial log, plus optional custom checks.
`-no-reboot` turns an accidental triple fault into a QEMU exit with status 0,
which never matches an expected status, so resets cannot masquerade as passes.
"""
import argparse
import re
import subprocess
import sys
import time

PASS, FAIL, PANIC = 33, 35, 37


def check_sched(log):
    seq = re.findall(r"\[sched\] thread ([ABC]) iteration (\d)", log)
    if len(seq) != 18:
        return f"expected 18 iteration lines, got {len(seq)}"
    letters = [s[0] for s in seq]
    if set(letters[:3]) != {"A", "B", "C"}:
        return f"first three iterations not interleaved: {letters[:3]}"
    switches = sum(1 for a, b in zip(letters, letters[1:]) if a != b)
    if switches < 10:
        return f"only {switches} thread changes in output order"
    for t in "ABC":
        its = [int(i) for l, i in seq if l == t]
        if its != list(range(6)):
            return f"thread {t} iterations out of order: {its}"
    return None


def check_timer(log):
    ticks = [int(t) for t in re.findall(r"\[timer\] tick (\d+)", log)]
    if len(ticks) != 5:
        return f"expected 5 tick lines, got {len(ticks)}"
    deltas = [b - a for a, b in zip(ticks, ticks[1:])]
    if any(d < 9 or d > 12 for d in deltas):
        return f"tick deltas out of range: {deltas}"
    return None


def check_ktest(log):
    names = re.findall(r"^KTEST (\S+)\s+PASS", log, re.M)
    fails = re.findall(r"^KTEST (\S+)\s+FAIL", log, re.M)
    m = re.search(r"\[ktest\] running (\d+) self-tests", log)
    if not m or int(m.group(1)) != len(names) or fails:
        return f"ktest mismatch: declared={m and m.group(1)} passed={len(names)} failed={fails}"
    return None


CASES = [
    ("boot", "boot", PASS, [r"Kestrel OS booting", r"\[cpu\] GDT\+TSS loaded", r"\[mm\] paging enabled",
                            r"interrupts on; boot complete", r"SCENARIO boot: PASS"], None),
    ("ktest", "ktest", PASS, [r"\[ktest\] running \d+ self-tests", r"KTEST page_fault_demand_paging\s+PASS",

                              r"KTEST spsc_irq_to_thread\s+PASS", r"KTEST mpsc_multi_producer\s+PASS",
                              r"summary: \d+ passed, 0 failed", r"SCENARIO ktest: PASS"], check_ktest),
    ("timer", "timer", PASS, [r"\[timer\] tick", r"SCENARIO timer: PASS"], check_timer),
    ("sched", "sched", PASS, [r"\[sched\] context switches during run: \d+", r"SCENARIO sched: PASS"], check_sched),
    ("page_fault", "pf", PANIC, [r"\[pf\] demand-paged fault at 50010000", r"\[pf\] demand-paged fault at 50011000",
                                  r"demand faults handled: 2", r"PAGE FAULT at 0badb000: not-present read",
                                  r"EXCEPTION 14 \(#PF Page Fault\)", r"CR2=0badb000", r"KERNEL PANIC"], None),
    ("exception_dump", "exc", PANIC, [r"EXCEPTION 6 \(#UD Invalid Opcode\)",
                                      r"EAX=11111111 EBX=22222222 ECX=33333333 EDX=44444444",
                                      r"EIP=[0-9a-f]{8} CS=0008", r"KERNEL PANIC: unhandled exception 6",
                                      r"\[trace\] dump"], None),
    ("user_syscalls", "user", PASS, [r"user: hello from ring 3!", r"user: getpid\(\) = \d+",
                                     r"user: write\(kernel ptr\) = -14", r"user: write\(unmapped ptr\) = -14",
                                     r"user: write\(wrapping ptr\) = -14", r"user: bad syscall = -38",
                                     r"user: all checks passed", r"exited with code 42",
                                     r"#GP General Protection in ring 3", r"exited with code 141",
                                     r"#PF Page Fault in ring 3", r"exited with code 142",
                                     r"SCENARIO user: PASS"], None),
    ("triple_fault_protection", "df", PASS, [r"recursing until the kernel stack hits its guard page",
                                             r"DOUBLE FAULT caught by task-gate handler",
                                             r"thread=overflow", r"triple fault averted",
                                             r"SCENARIO df: PASS"], None),
    # Control case: an unprotected triple fault resets the CPU; with -no-reboot QEMU
    # exits with status 0 and no SCENARIO line. Proves resets are detected.
    ("triple_fault_control", "tf", 0, [r"\[tf\] loading an empty IDT"], lambda log: "SCENARIO" in log and "unexpected SCENARIO line" or None),
    ("tracing", "trace", PASS, [r"kbpf filter attached \(verifier=0", r"\[trace\] dump: last \d+ events",
                                r"filtered=[1-9]", r"\[trace\] t=\s*\d+ .* syscall", r"SCENARIO trace: PASS"], None),
    ("all", "all", PASS, [r"summary: \d+ passed, 0 failed", r"\[timer\] tick", r"\[sched\] context switches",
                          r"user: all checks passed", r"\[mm\] free frames", r"SCENARIO all: PASS"], None),
]


def run_case(qemu, kernel, scenario, timeout):
    cmd = [qemu, "-kernel", kernel, "-m", "128M", "-serial", "stdio", "-display", "none", "-no-reboot",
           "-monitor", "none", "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04", "-append", f"test={scenario}"]
    t0 = time.time()
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
        return p.returncode, p.stdout.decode(errors="replace").replace("\r", ""), time.time() - t0
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode(errors="replace").replace("\r", "")
        return None, out, time.time() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--qemu", default="qemu-system-i386")
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("--only")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    passed = failed = 0
    for name, scenario, want, patterns, extra in CASES:
        if a.only and a.only != name:
            continue
        code, log, dt = run_case(a.qemu, a.kernel, scenario, a.timeout)
        problems = []
        if code is None:
            problems.append(f"timeout after {a.timeout}s")
        elif code != want:
            problems.append(f"exit status {code}, expected {want}" + (" (triple fault/reset?)" if code == 0 else ""))
        pos = 0
        for pat in patterns:  # ordered assertions
            m = re.compile(pat, re.M).search(log, pos)
            if not m:
                problems.append(f"missing (in order): /{pat}/")
                break
            pos = m.end()
        if extra and not problems:
            err = extra(log)
            if err:
                problems.append(err)
        status = "PASS" if not problems else "FAIL"
        print(f"[{status}] qemu:{name:<24} exit={code} {dt:5.1f}s")
        for pr in problems:
            print(f"        - {pr}")
        if problems or a.verbose:
            print("        --- serial log tail ---")
            for line in log.strip().splitlines()[-25:]:
                print("        | " + line)
        passed += not problems
        failed += bool(problems)
    print(f"qemu boot tests: {passed} passed, {failed} failed")
    return 1 if failed else 0

if __name__ == "__main__":
    sys.exit(main())
