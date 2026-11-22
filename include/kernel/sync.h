#ifndef OWNKRN_KERNEL_SYNC_H
#define OWNKRN_KERNEL_SYNC_H

#include <arch/io.h>
#include <stdint.h>

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT { .locked = 0 }

static inline void spinlock_init(spinlock_t *lk) {
    lk->locked = 0;
}

static inline void spin_lock(spinlock_t *lk) {
    while (__sync_lock_test_and_set(&lk->locked, 1)) {
        while (lk->locked) {
            cpu_pause();
        }
    }
}

static inline void spin_unlock(spinlock_t *lk) {
    __sync_lock_release(&lk->locked);
}

static inline uint64_t spin_lock_irqsave(spinlock_t *lk) {
    uint64_t flags = irq_save();
    spin_lock(lk);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *lk, uint64_t flags) {
    spin_unlock(lk);
    irq_restore(flags);
}

#endif /* OWNKRN_KERNEL_SYNC_H */
