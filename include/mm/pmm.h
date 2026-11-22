#ifndef OWNKRN_MM_PMM_H
#define OWNKRN_MM_PMM_H

#include <kernel/bootinfo.h>
#include <kernel/types.h>

struct pmm_stats {
    uint64_t total_frames;
    uint64_t used_frames;
    uint64_t free_frames;
    uint64_t total_bytes;
    uint64_t free_bytes;
};

void     pmm_init(const struct boot_info *info);
uint64_t pmm_alloc_frame(void);
uint64_t pmm_alloc_frames(size_t count);
void     pmm_free_frame(uint64_t phys);
void     pmm_free_frames(uint64_t phys, size_t count);
void     pmm_get_stats(struct pmm_stats *out);

#endif /* OWNKRN_MM_PMM_H */
