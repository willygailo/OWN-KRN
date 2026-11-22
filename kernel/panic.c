#include <arch/io.h>
#include <kernel/panic.h>
#include <kernel/printk.h>
#include <stdarg.h>

static void print_backtrace(uint64_t rbp) {
    printk("Stack backtrace:\n");
    for (int depth = 0; depth < 16; depth++) {
        if (rbp < KERNEL_VIRT_BASE && rbp < HHDM_BASE) {
            break;
        }
        if ((rbp & 0x7) != 0) {
            break;
        }
        const uint64_t *frame = (const uint64_t *)(uintptr_t)rbp;
        uint64_t next_rbp = frame[0];
        uint64_t ret_rip  = frame[1];
        if (ret_rip == 0) {
            break;
        }
        printk("  #%02d  RIP=0x%016llx  RBP=0x%016llx\n",
               depth, (unsigned long long)ret_rip, (unsigned long long)rbp);
        if (next_rbp <= rbp) {
            break;
        }
        rbp = next_rbp;
    }
}

NORETURN void kpanic(const char *fmt, ...) {
    irq_disable();
    printk("\n!!! KERNEL PANIC: ");
    va_list ap;
    va_start(ap, fmt);
    vprintk(fmt, ap);
    va_end(ap);
    printk("\n");

    uint64_t rbp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    print_backtrace(rbp);

    printk("!!! System halted.\n");
    for (;;) {
        irq_disable();
        cpu_halt();
    }
}

NORETURN void kpanic_frame(const struct interrupt_frame *f, const char *reason) {
    irq_disable();
    printk("\n================ KERNEL EXCEPTION PANIC ================\n");
    printk("Reason : %s (vec=%llu, err=0x%llx)\n",
           reason, (unsigned long long)f->int_no, (unsigned long long)f->err_code);

    if (f->int_no == 14) {
        uint64_t cr2 = read_cr2();
        printk("CR2    : 0x%016llx  [%s %s %s%s%s]\n",
               (unsigned long long)cr2,
               (f->err_code & 1) ? "PROT" : "NOT-PRESENT",
               (f->err_code & 2) ? "WRITE" : "READ",
               (f->err_code & 4) ? "USER" : "KERNEL",
               (f->err_code & 8) ? " RSVD" : "",
               (f->err_code & 16) ? " EXEC" : "");
    }

    printk("RIP    : 0x%016llx  CS : 0x%04llx  RFLAGS: 0x%016llx\n",
           (unsigned long long)f->rip, (unsigned long long)f->cs,
           (unsigned long long)f->rflags);
    printk("RSP    : 0x%016llx  SS : 0x%04llx  CR3   : 0x%016llx\n",
           (unsigned long long)f->rsp, (unsigned long long)f->ss,
           (unsigned long long)read_cr3());
    printk("RAX=%016llx RBX=%016llx RCX=%016llx RDX=%016llx\n",
           (unsigned long long)f->rax, (unsigned long long)f->rbx,
           (unsigned long long)f->rcx, (unsigned long long)f->rdx);
    printk("RSI=%016llx RDI=%016llx RBP=%016llx R8 =%016llx\n",
           (unsigned long long)f->rsi, (unsigned long long)f->rdi,
           (unsigned long long)f->rbp, (unsigned long long)f->r8);
    printk("R9 =%016llx R10=%016llx R11=%016llx R12=%016llx\n",
           (unsigned long long)f->r9,  (unsigned long long)f->r10,
           (unsigned long long)f->r11, (unsigned long long)f->r12);
    printk("R13=%016llx R14=%016llx R15=%016llx\n",
           (unsigned long long)f->r13, (unsigned long long)f->r14,
           (unsigned long long)f->r15);

    print_backtrace(f->rbp);
    printk("========================================================\n");

    for (;;) {
        irq_disable();
        cpu_halt();
    }
}
