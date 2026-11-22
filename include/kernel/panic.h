#ifndef OWNKRN_KERNEL_PANIC_H
#define OWNKRN_KERNEL_PANIC_H

#include <arch/idt.h>
#include <kernel/types.h>

NORETURN void kpanic(const char *fmt, ...);
NORETURN void kpanic_frame(const struct interrupt_frame *frame, const char *reason);

#define KASSERT(cond)                                                          \
    do {                                                                       \
        if (!(cond)) {                                                         \
            kpanic("Assertion failed: %s (%s:%d)", #cond, __FILE__, __LINE__); \
        }                                                                      \
    } while (0)

#endif /* OWNKRN_KERNEL_PANIC_H */
