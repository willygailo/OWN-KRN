#ifndef OWNKRN_ARCH_IDT_H
#define OWNKRN_ARCH_IDT_H

#include <stdint.h>

#define IRQ_BASE 32

struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*irq_handler_t)(struct interrupt_frame *frame);

void idt_init(void);
void irq_register_handler(uint8_t irq, irq_handler_t handler);
void idt_register_vector(uint8_t vector, irq_handler_t handler);

uint64_t idt_get_irq_count(uint8_t irq);
uint64_t idt_get_pf_count(void);
uint64_t idt_get_syscall_count(void);
void     idt_inc_syscall_count(void);

#endif /* OWNKRN_ARCH_IDT_H */
