#include <kernel/panic.h>
#include <kernel/printk.h>
#include <kernel/sync.h>
#include <lib/string.h>
#include <mm/pmm.h>

/* Supports up to 4 GiB physical RAM (1,048,576 4 KiB frames -> 128 KiB bitmap) */
#define PMM_MAX_FRAMES (1024ULL * 1024ULL)
#define PMM_BITMAP_LEN (PMM_MAX_FRAMES / 64ULL)

static uint64_t   g_bitmap[PMM_BITMAP_LEN];
static uint64_t   g_max_frame = 0;
static uint64_t   g_total_frames = 0;
static uint64_t   g_used_frames = 0;
static uint64_t   g_scan_hint = 256; /* Skip first 1 MiB */
static spinlock_t g_pmm_lock = SPINLOCK_INIT;

static inline bool frame_is_used(uint64_t idx) {
    return (g_bitmap[idx >> 6] & (1ULL << (idx & 63))) != 0;
}

static inline void frame_mark_used(uint64_t idx) {
    if (idx < g_max_frame && !frame_is_used(idx)) {
        g_bitmap[idx >> 6] |= (1ULL << (idx & 63));
        g_used_frames++;
    }
}

static inline void frame_mark_free(uint64_t idx) {
    if (idx < g_max_frame && frame_is_used(idx)) {
        g_bitmap[idx >> 6] &= ~(1ULL << (idx & 63));
        if (g_used_frames > 0) {
            g_used_frames--;
        }
    }
}

static void reserve_range(uint64_t start, uint64_t end) {
    if (end <= start) {
        return;
    }
    uint64_t first = ALIGN_DOWN(start, PAGE_SIZE) >> PAGE_SHIFT;
    uint64_t last  = ALIGN_UP(end, PAGE_SIZE) >> PAGE_SHIFT;
    if (last > g_max_frame) {
        last = g_max_frame;
    }
    for (uint64_t f = first; f < last; f++) {
        frame_mark_used(f);
    }
}

static void release_range(uint64_t start, uint64_t end) {
    uint64_t aligned_start = ALIGN_UP(start, PAGE_SIZE);
    uint64_t aligned_end   = ALIGN_DOWN(end, PAGE_SIZE);
    if (aligned_end <= aligned_start) {
        return;
    }
    uint64_t first = aligned_start >> PAGE_SHIFT;
    uint64_t last  = aligned_end >> PAGE_SHIFT;
    if (last > g_max_frame) {
        last = g_max_frame;
    }
    for (uint64_t f = first; f < last; f++) {
        frame_mark_free(f);
    }
}

void pmm_init(const struct boot_info *info) {
    memset(g_bitmap, 0xFF, sizeof(g_bitmap));

    uint64_t max_phys = info->max_phys_addr;
    if (max_phys > PMM_MAX_FRAMES * PAGE_SIZE) {
        max_phys = PMM_MAX_FRAMES * PAGE_SIZE;
    }
    g_max_frame = ALIGN_UP(max_phys, PAGE_SIZE) >> PAGE_SHIFT;
    g_total_frames = g_max_frame;
    g_used_frames = g_max_frame;

    /* Free all usable RAM regions reported by the bootloader */
    for (size_t i = 0; i < info->mmap_count; i++) {
        const struct boot_mmap_entry *e = &info->mmap[i];
        if (e->type == MMAP_TYPE_USABLE) {
            release_range(e->base, e->base + e->length);
        }
    }

    /* Re-reserve critical physical regions */
    reserve_range(0, 0x00100000ULL); /* Low 1 MiB (BIOS/VGA/EBDA) */
    reserve_range((uint64_t)(uintptr_t)__kernel_phys_start,
                  (uint64_t)(uintptr_t)__kernel_phys_end);

    if (info->raw_info_phys && info->raw_info_size) {
        reserve_range(info->raw_info_phys,
                      info->raw_info_phys + info->raw_info_size);
    }
    if (info->initrd_phys && info->initrd_size) {
        reserve_range(info->initrd_phys,
                      info->initrd_phys + info->initrd_size);
    }

    /* Count usable frames properly: track free_frames relative to usable RAM */
    g_total_frames = 0;
    for (size_t i = 0; i < info->mmap_count; i++) {
        if (info->mmap[i].type == MMAP_TYPE_USABLE) {
            uint64_t s = ALIGN_UP(info->mmap[i].base, PAGE_SIZE);
            uint64_t e = ALIGN_DOWN(info->mmap[i].base + info->mmap[i].length, PAGE_SIZE);
            if (e > PMM_MAX_FRAMES * PAGE_SIZE) {
                e = PMM_MAX_FRAMES * PAGE_SIZE;
            }
            if (e > s) {
                g_total_frames += (e - s) >> PAGE_SHIFT;
            }
        }
    }

    uint64_t actual_free = 0;
    for (uint64_t f = 0; f < g_max_frame; f++) {
        if (!frame_is_used(f)) {
            actual_free++;
        }
    }
    g_used_frames = (g_total_frames >= actual_free) ? (g_total_frames - actual_free) : 0;
    g_scan_hint = 256;

    pr_ok("PMM initialized: %llu MiB free / %llu MiB total (%llu frames)\n",
          (unsigned long long)((actual_free * PAGE_SIZE) / (1024ULL * 1024ULL)),
          (unsigned long long)((g_total_frames * PAGE_SIZE) / (1024ULL * 1024ULL)),
          (unsigned long long)actual_free);
}

uint64_t pmm_alloc_frame(void) {
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);

    for (int pass = 0; pass < 2; pass++) {
        uint64_t start = (pass == 0) ? g_scan_hint : 256;
        uint64_t end   = (pass == 0) ? g_max_frame : g_scan_hint;

        uint64_t w_start = start >> 6;
        uint64_t w_end   = (end + 63) >> 6;
        for (uint64_t w = w_start; w < w_end; w++) {
            if (g_bitmap[w] == ~0ULL) {
                continue;
            }
            uint64_t inv = ~g_bitmap[w];
            uint64_t bit = (uint64_t)__builtin_ctzll(inv);
            uint64_t idx = (w << 6) + bit;
            if (idx < start || idx >= end) {
                continue;
            }
            g_bitmap[w] |= (1ULL << bit);
            g_used_frames++;
            g_scan_hint = idx + 1;
            spin_unlock_irqrestore(&g_pmm_lock, flags);

            uint64_t phys = idx << PAGE_SHIFT;
            memset(PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
            return phys;
        }
    }

    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return 0;
}

uint64_t pmm_alloc_frames(size_t count) {
    if (count == 0) {
        return 0;
    }
    if (count == 1) {
        return pmm_alloc_frame();
    }

    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    uint64_t run = 0;
    uint64_t run_start = 256;

    for (uint64_t f = 256; f < g_max_frame; f++) {
        if (!frame_is_used(f)) {
            if (run == 0) {
                run_start = f;
            }
            run++;
            if (run == count) {
                for (uint64_t k = 0; k < count; k++) {
                    uint64_t idx = run_start + k;
                    g_bitmap[idx >> 6] |= (1ULL << (idx & 63));
                    g_used_frames++;
                }
                spin_unlock_irqrestore(&g_pmm_lock, flags);

                uint64_t phys = run_start << PAGE_SHIFT;
                memset(PHYS_TO_VIRT(phys), 0, count * PAGE_SIZE);
                return phys;
            }
        } else {
            run = 0;
        }
    }

    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return 0;
}

void pmm_free_frame(uint64_t phys) {
    if (phys == 0 || (phys & (PAGE_SIZE - 1)) != 0) {
        return;
    }
    uint64_t idx = phys >> PAGE_SHIFT;
    if (idx < 256 || idx >= g_max_frame) {
        return;
    }

    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    if (!frame_is_used(idx)) {
        spin_unlock_irqrestore(&g_pmm_lock, flags);
        kpanic("PMM double free of frame 0x%llx", (unsigned long long)phys);
    }
    g_bitmap[idx >> 6] &= ~(1ULL << (idx & 63));
    if (g_used_frames > 0) {
        g_used_frames--;
    }
    if (idx < g_scan_hint) {
        g_scan_hint = idx;
    }
    spin_unlock_irqrestore(&g_pmm_lock, flags);
}

void pmm_free_frames(uint64_t phys, size_t count) {
    for (size_t i = 0; i < count; i++) {
        pmm_free_frame(phys + i * PAGE_SIZE);
    }
}

void pmm_get_stats(struct pmm_stats *out) {
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    out->total_frames = g_total_frames;
    out->used_frames  = g_used_frames;
    out->free_frames  = (g_total_frames >= g_used_frames)
                        ? (g_total_frames - g_used_frames) : 0;
    out->total_bytes  = out->total_frames * PAGE_SIZE;
    out->free_bytes   = out->free_frames * PAGE_SIZE;
    spin_unlock_irqrestore(&g_pmm_lock, flags);
}
