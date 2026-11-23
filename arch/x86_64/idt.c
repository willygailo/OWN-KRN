#include <arch/gdt.h>
#include <arch/idt.h>
#include <arch/io.h>
#include <arch/pic.h>
#include <kernel/panic.h>
#include <kernel/printk.h>
#include <kernel/types.h>
#include <lib/string.h>

#define IDT_ENTRIES 256
#define NUM_STUBS   49

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} PACKED;

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} PACKED;

extern uint64_t isr_stub_table[NUM_STUBS];

static struct idt_entry  g_idt[IDT_ENTRIES] ALIGNED(16);
static irq_handler_t     g_handlers[IDT_ENTRIES];
static volatile uint64_t g_irq_counts[16];
static volatile uint64_t g_pf_count = 0;
static volatile uint64_t g_syscall_count = 0;

uint64_t idt_get_irq_count(uint8_t irq) {
    return (irq < 16) ? g_irq_counts[irq] : 0;
}

uint64_t idt_get_pf_count(void) {
    return g_pf_count;
}

uint64_t idt_get_syscall_count(void) {
    return g_syscall_count;
}

void idt_inc_syscall_count(void) {
    g_syscall_count++;
}

static const char *const k_exception_names[32] = {
    "Divide-by-Zero (#DE)",
    "Debug (#DB)",
    "Non-Maskable Interrupt (NMI)",
    "Breakpoint (#BP)",
    "Overflow (#OF)",
    "Bound Range Exceeded (#BR)",
    "Invalid Opcode (#UD)",
    "Device Not Available (#NM)",
    "Double Fault (#DF)",
    "Coprocessor Segment Overrun",
    "Invalid TSS (#TS)",
    "Segment Not Present (#NP)",
    "Stack-Segment Fault (#SS)",
    "General Protection Fault (#GP)",
    "Page Fault (#PF)",
    "Reserved (15)",
    "x87 Floating-Point (#MF)",
    "Alignment Check (#AC)",
    "Machine Check (#MC)",
    "SIMD Floating-Point (#XM)",
    "Virtualization (#VE)",
    "Control Protection (#CP)",
    "Reserved (22)", "Reserved (23)", "Reserved (24)", "Reserved (25)",
    "Reserved (26)", "Reserved (27)", "Reserved (28)",
    "VMM Communication (#VC)", "Security (#SX)", "Reserved (31)"
};

static void idt_set_gate(uint8_t vec, uint64_t handler, uint8_t ist, uint8_t flags) {
    struct idt_entry *e = &g_idt[vec];
    e->offset_low  = (uint16_t)(handler & 0xFFFF);
    e->selector    = GDT_KERNEL_CODE;
    e->ist         = ist & 0x07;
    e->type_attr   = flags;
    e->offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    e->offset_high = (uint32_t)(handler >> 32);
    e->zero        = 0;
}

void idt_init(void) {
    memset(g_idt, 0, sizeof(g_idt));
    memset(g_handlers, 0, sizeof(g_handlers));

    for (uint8_t i = 0; i < NUM_STUBS; i++) {
        uint8_t ist = (i == 8) ? 1 : 0; /* Isolate #DF onto IST1 */
        idt_set_gate(i, isr_stub_table[i], ist, 0x8E);
    }

    struct idt_ptr idtr = {
        .limit = sizeof(g_idt) - 1,
        .base  = (uint64_t)(uintptr_t)&g_idt,
    };
    __asm__ volatile("lidt %0" : : "m"(idtr));
}

void irq_register_handler(uint8_t irq, irq_handler_t handler) {
    if (irq < 16) {
        g_handlers[IRQ_BASE + irq] = handler;
        pic_unmask(irq);
    }
}

void idt_register_vector(uint8_t vector, irq_handler_t handler) {
    g_handlers[vector] = handler;
}

/* Optional hook invoked when a Ring 3 process faults (set by scheduler in Phase 6) */
typedef bool (*user_fault_hook_t)(struct interrupt_frame *frame);
static user_fault_hook_t g_user_fault_hook = NULL;

void idt_set_user_fault_hook(user_fault_hook_t hook) {
    g_user_fault_hook = hook;
}

void interrupt_dispatch(struct interrupt_frame *frame) {
    uint64_t vec = frame->int_no;

    /* Hardware IRQ (32..47) */
    if (vec >= IRQ_BASE && vec < IRQ_BASE + 16) {
        uint8_t irq = (uint8_t)(vec - IRQ_BASE);
        /* Ignore spurious IRQ7 / IRQ15 */
        if (irq == 7 && !(pic_get_isr() & (1U << 7))) {
            return;
        }
        if (irq == 15 && !(pic_get_isr() & (1U << 15))) {
            pic_eoi(0);
            return;
        }
        /* Send EOI before handler if handler may context switch (e.g. IRQ0 timer) */
        g_irq_counts[irq]++;
        pic_eoi(irq);
        if (g_handlers[vec]) {
            g_handlers[vec](frame);
        }
        return;
    }

    /* Custom registered handler (e.g., breakpoint or software test vector) */
    if (vec < IDT_ENTRIES && g_handlers[vec]) {
        g_handlers[vec](frame);
        return;
    }

    /* CPU Exception (0..31) */
    if (vec < 32) {
        if (vec == 14) {
            g_pf_count++;
        }
        if ((frame->cs & 3) == 3 && g_user_fault_hook && g_user_fault_hook(frame)) {
            return;
        }
        kpanic_frame(frame, k_exception_names[vec]);
    }

    kpanic_frame(frame, "Unhandled interrupt vector");
}
