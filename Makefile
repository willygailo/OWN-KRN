# OwnKrn x86_64 Kernel & Userspace Build System

CC      := gcc
LD      := ld
AS      := nasm
OBJCOPY := objcopy
QEMU    := qemu-system-x86_64

GCC_INC := $(shell $(CC) -print-file-name=include)

BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj
USER_OBJ  := $(BUILD_DIR)/user_obj
USER_BIN  := $(BUILD_DIR)/user_bin
KERNEL    := $(BUILD_DIR)/kernel.elf
INITRD    := $(BUILD_DIR)/initrd.tar
ISO       := $(BUILD_DIR)/kernel.iso

# Freestanding Higher-Half Kernel Flags
CFLAGS := \
	-std=gnu11 \
	-ffreestanding \
	-nostdlib \
	-nostdinc \
	-isystem $(GCC_INC) \
	-Iinclude \
	-fno-builtin \
	-fno-stack-protector \
	-fno-pic \
	-fno-pie \
	-fcf-protection=none \
	-fno-asynchronous-unwind-tables \
	-fno-unwind-tables \
	-fno-omit-frame-pointer \
	-mno-red-zone \
	-mno-mmx \
	-mno-sse \
	-mno-sse2 \
	-mcmodel=kernel \
	-Wall -Wextra -Werror \
	-g -O2 \
	-MMD -MP

# Freestanding Ring 3 Userspace Flags
USER_CFLAGS := \
	-std=gnu11 \
	-ffreestanding \
	-nostdlib \
	-nostdinc \
	-isystem $(GCC_INC) \
	-Iinclude \
	-Iuser/libc/include \
	-fno-builtin \
	-fno-stack-protector \
	-fno-pic \
	-fno-pie \
	-fcf-protection=none \
	-fno-asynchronous-unwind-tables \
	-fno-unwind-tables \
	-mno-mmx \
	-mno-sse \
	-mno-sse2 \
	-Wall -Wextra -Werror \
	-g -O2 \
	-MMD -MP

ASFLAGS      := -f elf64 -g -F dwarf
LDFLAGS      := -nostdlib -static -z max-page-size=0x1000 -T linker/kernel.ld
USER_LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -T user/user.ld

# Kernel Sources
ASM_SRCS := \
	arch/x86_64/boot.asm \
	arch/x86_64/gdt.asm \
	arch/x86_64/isr.asm \
	arch/x86_64/switch.asm \
	arch/x86_64/syscall.asm

C_SRCS := \
	arch/x86_64/gdt.c \
	arch/x86_64/idt.c \
	arch/x86_64/pic.c \
	arch/x86_64/pit.c \
	kernel/main.c \
	kernel/panic.c \
	kernel/printk.c \
	kernel/bootinfo.c \
	kernel/sched.c \
	kernel/elf.c \
	kernel/syscall.c \
	mm/pmm.c \
	mm/vmm.c \
	mm/kmalloc.c \
	fs/vfs.c \
	fs/tmpfs.c \
	fs/pipe.c \
	fs/initrd.c \
	fs/devfs.c \
	fs/procfs.c \
	drivers/serial.c \
	drivers/console.c \
	drivers/keyboard.c \
	drivers/rtc.c \
	drivers/tty.c \
	lib/string.c \
	lib/vsnprintf.c

OBJS := \
	$(patsubst %.asm,$(OBJ_DIR)/%.asm.o,$(ASM_SRCS)) \
	$(patsubst %.c,$(OBJ_DIR)/%.c.o,$(C_SRCS))

# Userspace Runtime & Programs
ULIBC_OBJS := \
	$(USER_OBJ)/libc/crt0.asm.o \
	$(USER_OBJ)/libc/syscall.asm.o \
	$(USER_OBJ)/libc/ulibc.c.o

USER_PROGS := \
	$(USER_BIN)/init \
	$(USER_BIN)/sh \
	$(USER_BIN)/fetch \
	$(USER_BIN)/top \
	$(USER_BIN)/edit \
	$(USER_BIN)/snake \
	$(USER_BIN)/hello \
	$(USER_BIN)/grep \
	$(USER_BIN)/wc \
	$(USER_BIN)/hexdump \
	$(USER_BIN)/date \
	$(USER_BIN)/sleep \
	$(USER_BIN)/selftest \
	$(USER_BIN)/faulttest \
	$(USER_BIN)/child_helper

DEPS := $(OBJS:.o=.d) $(USER_OBJ)/libc/ulibc.c.d

QEMU_COMMON := \
	-machine pc \
	-cpu qemu64,+nx,+smep \
	-m 256M \
	-no-reboot \
	-d guest_errors,cpu_reset \
	-D $(BUILD_DIR)/qemu.log

.PHONY: all iso run run-gui debug test clean check-tools

all: $(KERNEL) $(INITRD)

check-tools:
	@for t in $(CC) $(LD) $(AS) $(QEMU) gdb objdump readelf tar; do \
		command -v $$t >/dev/null 2>&1 || { echo "MISSING REQUIRED TOOL: $$t"; exit 1; }; \
	done
	@echo "Toolchain OK (CC=$$( $(CC) --version | head -n1 ), QEMU=$$( $(QEMU) --version | head -n1 ))"
	@if command -v xorriso >/dev/null 2>&1; then \
		echo "ISO builder OK (grub-mkrescue + xorriso available)"; \
	else \
		echo "Note: xorriso not installed; using direct QEMU PVH ELF64 -kernel boot (install xorriso for 'make iso')"; \
	fi

# Kernel Build Rules
$(KERNEL): $(OBJS) linker/kernel.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)
	@echo "Built $@"

$(OBJ_DIR)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# Userspace Build Rules
$(USER_OBJ)/libc/%.asm.o: user/libc/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(USER_OBJ)/libc/%.c.o: user/libc/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_OBJ)/bin/%.c.o: user/bin/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_OBJ)/init/init.c.o: user/init/init.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_OBJ)/sh/sh.c.o: user/sh/sh.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(USER_BIN)/init: $(USER_OBJ)/init/init.c.o $(ULIBC_OBJS) user/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(ULIBC_OBJS) $<

$(USER_BIN)/sh: $(USER_OBJ)/sh/sh.c.o $(ULIBC_OBJS) user/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(ULIBC_OBJS) $<

$(USER_BIN)/%: $(USER_OBJ)/bin/%.c.o $(ULIBC_OBJS) user/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(USER_LDFLAGS) -o $@ $(ULIBC_OBJS) $<

# Initramfs Archive (POSIX ustar)
$(INITRD): $(USER_PROGS) $(wildcard initrd_files/etc/*)
	@rm -rf $(BUILD_DIR)/initrd_root
	@mkdir -p $(BUILD_DIR)/initrd_root/bin $(BUILD_DIR)/initrd_root/etc
	@cp -r initrd_files/* $(BUILD_DIR)/initrd_root/
	@cp $(USER_PROGS) $(BUILD_DIR)/initrd_root/bin/
	@tar --format=ustar -cf $(INITRD) -C $(BUILD_DIR)/initrd_root .
	@echo "Built $(INITRD)"

iso: $(KERNEL) $(INITRD)
	@command -v grub-mkrescue >/dev/null 2>&1 || { echo "grub-mkrescue missing"; exit 1; }
	@command -v xorriso >/dev/null 2>&1 || { echo "xorriso missing (sudo apt install xorriso mtools)"; exit 1; }
	@rm -rf $(BUILD_DIR)/isodir
	@mkdir -p $(BUILD_DIR)/isodir/boot/grub
	@cp $(KERNEL) $(BUILD_DIR)/isodir/boot/kernel.elf
	@cp $(INITRD) $(BUILD_DIR)/isodir/boot/initrd.tar
	@cp boot/grub/grub.cfg $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(BUILD_DIR)/isodir
	@echo "Built $(ISO)"

run: $(KERNEL) $(INITRD)
	$(QEMU) $(QEMU_COMMON) -kernel $(KERNEL) -initrd $(INITRD) -display none -chardev stdio,id=ser0,mux=on,signal=off -mon chardev=ser0 -serial chardev:ser0

run-gui: $(KERNEL) $(INITRD)
	$(QEMU) $(QEMU_COMMON) -kernel $(KERNEL) -initrd $(INITRD) -chardev stdio,id=ser0,mux=on,signal=off -mon chardev=ser0 -serial chardev:ser0

debug: $(KERNEL) $(INITRD)
	@echo "Starting QEMU paused on gdbstub tcp::1234"
	@echo "Attach with: gdb -ex 'file $(KERNEL)' -ex 'target remote :1234'"
	$(QEMU) $(QEMU_COMMON) -kernel $(KERNEL) -initrd $(INITRD) -display none -serial stdio -s -S

test: $(KERNEL) $(INITRD)
	@bash tests/run_tests.sh

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
