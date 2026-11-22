#include <arch/io.h>
#include <kernel/panic.h>
#include <kernel/printk.h>
#include <kernel/sync.h>
#include <lib/string.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#define PML4_IDX(va) (((va) >> 39) & 0x1FFULL)
#define PDPT_IDX(va) (((va) >> 30) & 0x1FFULL)
#define PD_IDX(va)   (((va) >> 21) & 0x1FFULL)
#define PT_IDX(va)   (((va) >> 12) & 0x1FFULL)

static uint64_t   g_kernel_pml4_phys = 0;
static spinlock_t g_vmm_lock = SPINLOCK_INIT;

static inline uint64_t *pt_virt(uint64_t phys) {
    return (uint64_t *)PHYS_TO_VIRT(phys & PTE_ADDR_MASK);
}

static uint64_t *get_or_alloc_table(uint64_t *parent, size_t idx, bool user) {
    uint64_t entry = parent[idx];
    if (entry & PTE_PRESENT) {
        if (user && !(entry & PTE_USER)) {
            parent[idx] |= PTE_USER | PTE_WRITABLE;
        }
        return pt_virt(entry);
    }

    uint64_t frame = pmm_alloc_frame();
    if (!frame) {
        return NULL;
    }
    uint64_t flags = PTE_PRESENT | PTE_WRITABLE | (user ? PTE_USER : 0);
    parent[idx] = frame | flags;
    return pt_virt(frame);
}

static bool map_2m_page(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pml4 = pt_virt(pml4_phys);
    uint64_t *pdpt = get_or_alloc_table(pml4, PML4_IDX(virt), false);
    if (!pdpt) return false;
    uint64_t *pd = get_or_alloc_table(pdpt, PDPT_IDX(virt), false);
    if (!pd) return false;

    pd[PD_IDX(virt)] = (phys & ~0x1FFFFFULL) | flags | PTE_HUGE;
    return true;
}

static bool map_4k_unlocked(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    bool user = (flags & PTE_USER) != 0;
    uint64_t *pml4 = pt_virt(pml4_phys);
    uint64_t *pdpt = get_or_alloc_table(pml4, PML4_IDX(virt), user);
    if (!pdpt) return false;
    uint64_t *pd = get_or_alloc_table(pdpt, PDPT_IDX(virt), user);
    if (!pd) return false;
    uint64_t *pt = get_or_alloc_table(pd, PD_IDX(virt), user);
    if (!pt) return false;

    pt[PT_IDX(virt)] = (phys & PTE_ADDR_MASK) | flags;
    if (read_cr3() == pml4_phys) {
        invlpg(virt);
    }
    return true;
}

bool vmm_map_page(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t irq = spin_lock_irqsave(&g_vmm_lock);
    bool ok = map_4k_unlocked(pml4_phys, virt, phys, flags);
    spin_unlock_irqrestore(&g_vmm_lock, irq);
    return ok;
}

bool vmm_unmap_page(uint64_t pml4_phys, uint64_t virt, uint64_t *out_phys) {
    uint64_t irq = spin_lock_irqsave(&g_vmm_lock);
    uint64_t *pml4 = pt_virt(pml4_phys);
    uint64_t e4 = pml4[PML4_IDX(virt)];
    if (!(e4 & PTE_PRESENT)) {
        spin_unlock_irqrestore(&g_vmm_lock, irq);
        return false;
    }
    uint64_t *pdpt = pt_virt(e4);
    uint64_t e3 = pdpt[PDPT_IDX(virt)];
    if (!(e3 & PTE_PRESENT) || (e3 & PTE_HUGE)) {
        spin_unlock_irqrestore(&g_vmm_lock, irq);
        return false;
    }
    uint64_t *pd = pt_virt(e3);
    uint64_t e2 = pd[PD_IDX(virt)];
    if (!(e2 & PTE_PRESENT) || (e2 & PTE_HUGE)) {
        spin_unlock_irqrestore(&g_vmm_lock, irq);
        return false;
    }
    uint64_t *pt = pt_virt(e2);
    uint64_t e1 = pt[PT_IDX(virt)];
    if (!(e1 & PTE_PRESENT)) {
        spin_unlock_irqrestore(&g_vmm_lock, irq);
        return false;
    }
    if (out_phys) {
        *out_phys = e1 & PTE_ADDR_MASK;
    }
    pt[PT_IDX(virt)] = 0;
    if (read_cr3() == pml4_phys) {
        invlpg(virt);
    }
    spin_unlock_irqrestore(&g_vmm_lock, irq);
    return true;
}

bool vmm_virt_to_phys(uint64_t pml4_phys, uint64_t virt,
                      uint64_t *out_phys, uint64_t *out_flags) {
    uint64_t irq = spin_lock_irqsave(&g_vmm_lock);
    uint64_t *pml4 = pt_virt(pml4_phys);
    uint64_t e4 = pml4[PML4_IDX(virt)];
    if (!(e4 & PTE_PRESENT)) goto not_found;

    uint64_t *pdpt = pt_virt(e4);
    uint64_t e3 = pdpt[PDPT_IDX(virt)];
    if (!(e3 & PTE_PRESENT)) goto not_found;
    if (e3 & PTE_HUGE) {
        if (out_phys) *out_phys = (e3 & 0x000FFFFFC0000000ULL) | (virt & 0x3FFFFFFFULL);
        if (out_flags) *out_flags = e3 & ~PTE_ADDR_MASK;
        spin_unlock_irqrestore(&g_vmm_lock, irq);
        return true;
    }

    uint64_t *pd = pt_virt(e3);
    uint64_t e2 = pd[PD_IDX(virt)];
    if (!(e2 & PTE_PRESENT)) goto not_found;
    if (e2 & PTE_HUGE) {
        if (out_phys) *out_phys = (e2 & 0x000FFFFFFFE00000ULL) | (virt & 0x1FFFFFULL);
        if (out_flags) *out_flags = e2 & ~PTE_ADDR_MASK;
        spin_unlock_irqrestore(&g_vmm_lock, irq);
        return true;
    }

    uint64_t *pt = pt_virt(e2);
    uint64_t e1 = pt[PT_IDX(virt)];
    if (!(e1 & PTE_PRESENT)) goto not_found;

    if (out_phys) *out_phys = (e1 & PTE_ADDR_MASK) | (virt & 0xFFFULL);
    if (out_flags) *out_flags = e1 & ~PTE_ADDR_MASK;
    spin_unlock_irqrestore(&g_vmm_lock, irq);
    return true;

not_found:
    spin_unlock_irqrestore(&g_vmm_lock, irq);
    return false;
}

void vmm_init(const struct boot_info *info) {
    g_kernel_pml4_phys = pmm_alloc_frame();
    KASSERT(g_kernel_pml4_phys != 0);

    /* 1. Map HHDM (0xFFFF800000000000+) covering at least 4 GiB with 2 MiB NX pages */
    uint64_t hhdm_limit = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    if (info->max_phys_addr > hhdm_limit) {
        hhdm_limit = ALIGN_UP(info->max_phys_addr, 2ULL * 1024ULL * 1024ULL);
    }
    for (uint64_t pa = 0; pa < hhdm_limit; pa += 2ULL * 1024ULL * 1024ULL) {
        KASSERT(map_2m_page(g_kernel_pml4_phys, HHDM_BASE + pa, pa,
                            PTE_PRESENT | PTE_WRITABLE | PTE_NX));
    }

    /* 2. Map higher-half kernel sections with strict 4 KiB W^X permissions */
    uint64_t text_s   = (uint64_t)(uintptr_t)__text_start;
    uint64_t text_e   = (uint64_t)(uintptr_t)__text_end;
    uint64_t rodata_s = (uint64_t)(uintptr_t)__rodata_start;
    uint64_t rodata_e = (uint64_t)(uintptr_t)__rodata_end;
    uint64_t data_s   = (uint64_t)(uintptr_t)__data_start;
    uint64_t kern_e   = (uint64_t)(uintptr_t)__kernel_end;

    for (uint64_t va = text_s; va < text_e; va += PAGE_SIZE) {
        KASSERT(map_4k_unlocked(g_kernel_pml4_phys, va, KVIRT_TO_PHYS(va),
                                PTE_PRESENT));
    }
    for (uint64_t va = rodata_s; va < rodata_e; va += PAGE_SIZE) {
        KASSERT(map_4k_unlocked(g_kernel_pml4_phys, va, KVIRT_TO_PHYS(va),
                                PTE_PRESENT | PTE_NX));
    }
    for (uint64_t va = data_s; va < kern_e; va += PAGE_SIZE) {
        KASSERT(map_4k_unlocked(g_kernel_pml4_phys, va, KVIRT_TO_PHYS(va),
                                PTE_PRESENT | PTE_WRITABLE | PTE_NX));
    }

    /* 3. Pre-allocate kernel heap PML4 slot (PML4[384]) so all future user
     *    address spaces share dynamic kernel heap mappings automatically. */
    uint64_t *kpml4 = pt_virt(g_kernel_pml4_phys);
    KASSERT(get_or_alloc_table(kpml4, PML4_IDX(KHEAP_VIRT_BASE), false) != NULL);

    /* 4. Switch CR3 to clean kernel page tables (unmaps low identity PML4[0]) */
    write_cr3(g_kernel_pml4_phys);

    pr_ok("VMM initialized: CR3=0x%llx (W^X enforced: .text RX, .rodata RO+NX, .data/.bss RW+NX)\n",
          (unsigned long long)g_kernel_pml4_phys);
}

uint64_t vmm_kernel_pml4(void) {
    return g_kernel_pml4_phys;
}

uint64_t vmm_create_address_space(void) {
    uint64_t pml4_phys = pmm_alloc_frame();
    if (!pml4_phys) {
        return 0;
    }
    uint64_t *dst = pt_virt(pml4_phys);
    uint64_t *src = pt_virt(g_kernel_pml4_phys);
    /* Zero user half [0..255], share kernel half [256..511] */
    memset(&dst[0], 0, 256 * sizeof(uint64_t));
    memcpy(&dst[256], &src[256], 256 * sizeof(uint64_t));
    return pml4_phys;
}

uint64_t vmm_clone_address_space(uint64_t src_pml4_phys) {
    uint64_t dst_pml4_phys = vmm_create_address_space();
    if (!dst_pml4_phys) {
        return 0;
    }

    uint64_t irq = spin_lock_irqsave(&g_vmm_lock);
    uint64_t *src_pml4 = pt_virt(src_pml4_phys);

    for (size_t i4 = 0; i4 < 256; i4++) {
        if (!(src_pml4[i4] & PTE_PRESENT)) continue;
        uint64_t *src_pdpt = pt_virt(src_pml4[i4]);

        for (size_t i3 = 0; i3 < 512; i3++) {
            if (!(src_pdpt[i3] & PTE_PRESENT) || (src_pdpt[i3] & PTE_HUGE)) continue;
            uint64_t *src_pd = pt_virt(src_pdpt[i3]);

            for (size_t i2 = 0; i2 < 512; i2++) {
                if (!(src_pd[i2] & PTE_PRESENT) || (src_pd[i2] & PTE_HUGE)) continue;
                uint64_t *src_pt = pt_virt(src_pd[i2]);

                for (size_t i1 = 0; i1 < 512; i1++) {
                    uint64_t pte = src_pt[i1];
                    if (!(pte & PTE_PRESENT) || !(pte & PTE_USER)) continue;

                    uint64_t new_phys = pmm_alloc_frame();
                    if (!new_phys) {
                        spin_unlock_irqrestore(&g_vmm_lock, irq);
                        vmm_destroy_address_space(dst_pml4_phys);
                        return 0;
                    }
                    memcpy(PHYS_TO_VIRT(new_phys),
                           PHYS_TO_VIRT(pte & PTE_ADDR_MASK), PAGE_SIZE);

                    uint64_t va = ((uint64_t)i4 << 39) | ((uint64_t)i3 << 30) |
                                  ((uint64_t)i2 << 21) | ((uint64_t)i1 << 12);
                    uint64_t flags = pte & ~PTE_ADDR_MASK;
                    if (!map_4k_unlocked(dst_pml4_phys, va, new_phys, flags)) {
                        pmm_free_frame(new_phys);
                        spin_unlock_irqrestore(&g_vmm_lock, irq);
                        vmm_destroy_address_space(dst_pml4_phys);
                        return 0;
                    }
                }
            }
        }
    }

    spin_unlock_irqrestore(&g_vmm_lock, irq);
    return dst_pml4_phys;
}

void vmm_destroy_address_space(uint64_t pml4_phys) {
    if (!pml4_phys || pml4_phys == g_kernel_pml4_phys) {
        return;
    }
    if (read_cr3() == pml4_phys) {
        write_cr3(g_kernel_pml4_phys);
    }

    uint64_t irq = spin_lock_irqsave(&g_vmm_lock);
    uint64_t *pml4 = pt_virt(pml4_phys);

    for (size_t i4 = 0; i4 < 256; i4++) {
        if (!(pml4[i4] & PTE_PRESENT)) continue;
        uint64_t pdpt_phys = pml4[i4] & PTE_ADDR_MASK;
        uint64_t *pdpt = pt_virt(pdpt_phys);

        for (size_t i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PTE_PRESENT) || (pdpt[i3] & PTE_HUGE)) continue;
            uint64_t pd_phys = pdpt[i3] & PTE_ADDR_MASK;
            uint64_t *pd = pt_virt(pd_phys);

            for (size_t i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PTE_PRESENT) || (pd[i2] & PTE_HUGE)) continue;
                uint64_t pt_phys = pd[i2] & PTE_ADDR_MASK;
                uint64_t *pt = pt_virt(pt_phys);

                for (size_t i1 = 0; i1 < 512; i1++) {
                    if ((pt[i1] & PTE_PRESENT) && (pt[i1] & PTE_USER)) {
                        pmm_free_frame(pt[i1] & PTE_ADDR_MASK);
                    }
                }
                pmm_free_frame(pt_phys);
            }
            pmm_free_frame(pd_phys);
        }
        pmm_free_frame(pdpt_phys);
    }
    pmm_free_frame(pml4_phys);
    spin_unlock_irqrestore(&g_vmm_lock, irq);
}

void vmm_switch(uint64_t pml4_phys) {
    if (read_cr3() != pml4_phys) {
        write_cr3(pml4_phys);
    }
}
