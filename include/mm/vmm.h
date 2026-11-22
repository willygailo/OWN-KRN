#ifndef OWNKRN_MM_VMM_H
#define OWNKRN_MM_VMM_H

#include <kernel/bootinfo.h>
#include <kernel/types.h>

#define PTE_PRESENT   BIT(0)
#define PTE_WRITABLE  BIT(1)
#define PTE_USER      BIT(2)
#define PTE_PWT       BIT(3)
#define PTE_PCD       BIT(4)
#define PTE_HUGE      BIT(7)
#define PTE_GLOBAL    BIT(8)
#define PTE_NX        BIT(63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

void     vmm_init(const struct boot_info *info);
uint64_t vmm_kernel_pml4(void);
uint64_t vmm_create_address_space(void);
uint64_t vmm_clone_address_space(uint64_t src_pml4_phys);
void     vmm_destroy_address_space(uint64_t pml4_phys);

bool     vmm_map_page(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);
bool     vmm_unmap_page(uint64_t pml4_phys, uint64_t virt, uint64_t *out_phys);
bool     vmm_virt_to_phys(uint64_t pml4_phys, uint64_t virt,
                          uint64_t *out_phys, uint64_t *out_flags);
void     vmm_switch(uint64_t pml4_phys);

#endif /* OWNKRN_MM_VMM_H */
