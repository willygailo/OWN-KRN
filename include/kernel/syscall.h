#ifndef OWNKRN_KERNEL_SYSCALL_H
#define OWNKRN_KERNEL_SYSCALL_H

#include <arch/idt.h>
#include <kernel/types.h>

void    syscall_init(void);
int64_t syscall_dispatch(struct interrupt_frame *frame);

int     copy_from_user(void *kdst, uint64_t usrc, size_t len);
int     copy_to_user(uint64_t udst, const void *ksrc, size_t len);
int     copy_str_from_user(char *kdst, uint64_t usrc, size_t max_len);

#endif /* OWNKRN_KERNEL_SYSCALL_H */
