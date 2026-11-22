#ifndef OWNKRN_ARCH_IO_H
#define OWNKRN_ARCH_IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)(val & 0xFFFFFFFFULL);
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline uint64_t read_cr0(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void write_cr3(uint64_t v) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}

static inline void invlpg(uint64_t addr) {
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

static inline uint64_t read_rflags(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    return flags;
}

static inline void irq_disable(void) {
    __asm__ volatile("cli" : : : "memory");
}

static inline void irq_enable(void) {
    __asm__ volatile("sti" : : : "memory");
}

static inline uint64_t irq_save(void) {
    uint64_t flags = read_rflags();
    irq_disable();
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    if (flags & (1ULL << 9)) {
        irq_enable();
    }
}

static inline void cpu_halt(void) {
    __asm__ volatile("hlt");
}

static inline void cpu_pause(void) {
    __asm__ volatile("pause");
}

/* QEMU / Bochs ACPI poweroff helper */
static inline void arch_poweroff(void) {
    outw(0x604, 0x2000);   /* QEMU pc/q35 ACPI PM1a_CNT */
    outw(0xB004, 0x2000);  /* Bochs / older QEMU */
    outw(0x4004, 0x3400);  /* VirtualBox */
    for (;;) {
        irq_disable();
        cpu_halt();
    }
}

/* 8042 PS/2 keyboard controller CPU reset */
static inline void arch_reboot(void) {
    irq_disable();
    for (int i = 0; i < 10000; i++) {
        if ((inb(0x64) & 0x02) == 0) {
            break;
        }
    }
    outb(0x64, 0xFE);
    arch_poweroff();
}

#endif /* OWNKRN_ARCH_IO_H */
