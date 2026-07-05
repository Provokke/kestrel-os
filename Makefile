# Kestrel OS build. `make` builds the kernel, `make test` runs every suite.
CC      := clang
LD      := ld.lld
NASM    := nasm
OBJCOPY := llvm-objcopy
QEMU    ?= qemu-system-i386
B       := build

KCFLAGS := --target=i686-unknown-none-elf -std=gnu11 -ffreestanding -fno-builtin -nostdlib \
           -fno-pic -fno-stack-protector -mno-sse -mno-mmx -mno-80387 -fno-omit-frame-pointer \
           -O2 -g -Wall -Wextra -Werror -Iinclude -Ilib
UCFLAGS := --target=i686-unknown-none-elf -std=gnu11 -ffreestanding -fno-builtin -nostdlib \
           -fno-pic -fno-stack-protector -mno-sse -mno-mmx -mno-80387 -O2 -Wall -Wextra -Werror
HOSTCFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Ilib

KSRCS := $(wildcard kernel/*.c) arch/x86/cpu.c $(wildcard lib/*.c)
KOBJS := $(patsubst %.c,$(B)/k/%.o,$(KSRCS)) $(B)/k/boot.o $(B)/k/isr.o
LIBSRCS := $(wildcard lib/*.c)

.PHONY: all kernel test test-host test-host-tsan test-qemu bench run clean
all: kernel
kernel: $(B)/kestrel.elf

$(B)/k/%.o: %.c include/kernel.h $(wildcard lib/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -c $< -o $@

$(B)/k/boot.o: arch/x86/boot.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 $< -o $@

$(B)/user.elf: user/user.c user/user.ld
	@mkdir -p $(B)
	$(CC) $(UCFLAGS) -c user/user.c -o $(B)/user.o
	$(LD) -m elf_i386 -T user/user.ld $(B)/user.o -o $@

$(B)/user.bin: $(B)/user.elf
	$(OBJCOPY) -O binary $< $@

$(B)/k/isr.o: arch/x86/isr.asm $(B)/user.bin
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 -DUSER_BIN='"$(B)/user.bin"' $< -o $@

$(B)/kestrel.elf: $(KOBJS) arch/x86/linker.ld
	$(LD) -m elf_i386 -T arch/x86/linker.ld $(KOBJS) -o $@

# ---- host-side tests (ASan+UBSan, and TSan for the lock-free stress tests) ----
$(B)/host_tests: $(LIBSRCS) tests/host/test_lib.c $(wildcard lib/*.h)
	@mkdir -p $(B)
	$(CC) $(HOSTCFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all $(LIBSRCS) tests/host/test_lib.c -o $@ -lpthread

$(B)/host_tests_tsan: $(LIBSRCS) tests/host/test_lib.c $(wildcard lib/*.h)
	@mkdir -p $(B)
	$(CC) $(HOSTCFLAGS) -fsanitize=thread $(LIBSRCS) tests/host/test_lib.c -o $@ -lpthread

$(B)/bench: $(LIBSRCS) tests/host/bench.c $(wildcard lib/*.h)
	@mkdir -p $(B)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -Ilib $(LIBSRCS) tests/host/bench.c -o $@ -lpthread

test-host: $(B)/host_tests
	./$(B)/host_tests

test-host-tsan: $(B)/host_tests_tsan
	./$(B)/host_tests_tsan

test-qemu: $(B)/kestrel.elf
	python3 tests/qemu/run_qemu_tests.py --kernel $(B)/kestrel.elf --qemu $(QEMU)

test: test-host test-host-tsan test-qemu

bench: $(B)/bench
	./$(B)/bench

run: $(B)/kestrel.elf
	$(QEMU) -kernel $< -m 128M -serial stdio -display none -no-reboot \
	  -device isa-debug-exit,iobase=0xf4,iosize=0x04 -append "test=all"

clean:
	rm -rf $(B)
