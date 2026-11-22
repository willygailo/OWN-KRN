#ifndef OWNKRN_KERNEL_ELF_H
#define OWNKRN_KERNEL_ELF_H

#include <kernel/types.h>

#define USER_STACK_TOP   0x00007FFFFFFFE000ULL
#define USER_STACK_PAGES 16ULL /* 64 KiB user stack */

struct elf_load_result {
    uint64_t entry;
    uint64_t user_rsp;
    uint64_t brk_start;
    uint64_t cr3;
};

int   elf_load_into_aspace(const uint8_t *image, size_t size,
                           const char *const *argv, int argc,
                           struct elf_load_result *out);
pid_t elf_spawn_user_process(const char *path, const char *const *argv, int argc);

#endif /* OWNKRN_KERNEL_ELF_H */
