#include <kernel/panic.h>
#include <kernel/printk.h>
#include <kernel/sync.h>
#include <kernel/types.h>
#include <lib/string.h>
#include <mm/kmalloc.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#define KHEAP_MAGIC_USED  0x55AA1337C0DEBEEFULL
#define KHEAP_MAGIC_FREE  0x55AA1337F00DFEE0ULL
#define KHEAP_TAIL_CANARY 0xDEADC0DEDEADBEEFULL
#define KHEAP_INIT_PAGES  64ULL /* 256 KiB initial heap */
#define KHEAP_MAX_SIZE    (64ULL * 1024ULL * 1024ULL) /* 64 MiB max heap */

struct kheap_block {
    uint64_t            magic;
    size_t              size; /* Payload size in bytes (excluding header & canary) */
    struct kheap_block *prev;
    struct kheap_block *next;
};

#define HDR_SIZE   sizeof(struct kheap_block) /* 32 bytes (16-byte aligned) */
#define TAIL_SIZE  sizeof(uint64_t)
#define OVERHEAD   (HDR_SIZE + TAIL_SIZE)
#define MIN_SPLIT  (OVERHEAD + 32)

static struct kheap_block *g_head = NULL;
static uint64_t            g_heap_end = KHEAP_VIRT_BASE;
static size_t              g_alloc_count = 0;
static spinlock_t          g_heap_lock = SPINLOCK_INIT;

static inline uint64_t *block_canary(struct kheap_block *b) {
    return (uint64_t *)((uint8_t *)b + HDR_SIZE + b->size);
}

static bool heap_grow_unlocked(size_t min_bytes) {
    size_t need = ALIGN_UP(min_bytes + OVERHEAD, PAGE_SIZE);
    if (need < 16 * PAGE_SIZE) {
        need = 16 * PAGE_SIZE;
    }
    if ((g_heap_end - KHEAP_VIRT_BASE) + need > KHEAP_MAX_SIZE) {
        return false;
    }

    uint64_t region_start = g_heap_end;
    size_t pages = need / PAGE_SIZE;
    for (size_t i = 0; i < pages; i++) {
        uint64_t pa = pmm_alloc_frame();
        if (!pa) {
            return false;
        }
        uint64_t va = region_start + i * PAGE_SIZE;
        if (!vmm_map_page(vmm_kernel_pml4(), va, pa,
                          PTE_PRESENT | PTE_WRITABLE | PTE_NX)) {
            pmm_free_frame(pa);
            return false;
        }
    }
    g_heap_end += need;

    struct kheap_block *nb = (struct kheap_block *)(uintptr_t)region_start;
    nb->magic = KHEAP_MAGIC_FREE;
    nb->size  = need - OVERHEAD;
    nb->prev  = NULL;
    nb->next  = NULL;
    *block_canary(nb) = KHEAP_TAIL_CANARY;

    if (!g_head) {
        g_head = nb;
        return true;
    }

    /* Append to end and coalesce with tail if tail is free */
    struct kheap_block *tail = g_head;
    while (tail->next) {
        tail = tail->next;
    }
    if (tail->magic == KHEAP_MAGIC_FREE &&
        ((uint8_t *)tail + OVERHEAD + tail->size == (uint8_t *)nb)) {
        tail->size += OVERHEAD + nb->size;
        *block_canary(tail) = KHEAP_TAIL_CANARY;
    } else {
        tail->next = nb;
        nb->prev = tail;
    }
    return true;
}

void kheap_init(void) {
    g_head = NULL;
    g_heap_end = KHEAP_VIRT_BASE;
    g_alloc_count = 0;
    KASSERT(heap_grow_unlocked(KHEAP_INIT_PAGES * PAGE_SIZE));
    pr_ok("Kernel heap initialized at 0x%016llx (%llu KiB initial)\n",
          (unsigned long long)KHEAP_VIRT_BASE,
          (unsigned long long)((g_heap_end - KHEAP_VIRT_BASE) / 1024ULL));
}

void *kmalloc(size_t size) {
    if (size == 0) {
        size = 16;
    }
    size = (size_t)ALIGN_UP(size, 16);

    uint64_t flags = spin_lock_irqsave(&g_heap_lock);

    for (int attempt = 0; attempt < 2; attempt++) {
        for (struct kheap_block *b = g_head; b != NULL; b = b->next) {
            if (b->magic != KHEAP_MAGIC_FREE && b->magic != KHEAP_MAGIC_USED) {
                spin_unlock_irqrestore(&g_heap_lock, flags);
                kpanic("Kernel heap corruption at %p (magic=0x%llx)",
                       b, (unsigned long long)b->magic);
            }
            if (b->magic != KHEAP_MAGIC_FREE || b->size < size) {
                continue;
            }

            /* Split block if sufficient remainder */
            if (b->size >= size + MIN_SPLIT) {
                struct kheap_block *rem =
                    (struct kheap_block *)((uint8_t *)b + OVERHEAD + size);
                rem->magic = KHEAP_MAGIC_FREE;
                rem->size  = b->size - size - OVERHEAD;
                rem->prev  = b;
                rem->next  = b->next;
                *block_canary(rem) = KHEAP_TAIL_CANARY;

                if (b->next) {
                    b->next->prev = rem;
                }
                b->next = rem;
                b->size = size;
            }

            b->magic = KHEAP_MAGIC_USED;
            *block_canary(b) = KHEAP_TAIL_CANARY;
            g_alloc_count++;
            spin_unlock_irqrestore(&g_heap_lock, flags);
            return (uint8_t *)b + HDR_SIZE;
        }

        if (!heap_grow_unlocked(size)) {
            break;
        }
    }

    spin_unlock_irqrestore(&g_heap_lock, flags);
    return NULL;
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p) {
        memset(p, 0, size);
    }
    return p;
}

void kfree(void *ptr) {
    if (!ptr) {
        return;
    }
    uint64_t addr = (uint64_t)(uintptr_t)ptr;
    if (addr < KHEAP_VIRT_BASE + HDR_SIZE || addr >= g_heap_end) {
        kpanic("kfree() invalid pointer %p", ptr);
    }

    uint64_t flags = spin_lock_irqsave(&g_heap_lock);
    struct kheap_block *b = (struct kheap_block *)((uint8_t *)ptr - HDR_SIZE);

    if (b->magic != KHEAP_MAGIC_USED) {
        spin_unlock_irqrestore(&g_heap_lock, flags);
        kpanic("kfree() double-free or corrupt header at %p (magic=0x%llx)",
               ptr, (unsigned long long)b->magic);
    }
    if (*block_canary(b) != KHEAP_TAIL_CANARY) {
        spin_unlock_irqrestore(&g_heap_lock, flags);
        kpanic("kfree() buffer overflow detected at %p (canary=0x%llx)",
               ptr, (unsigned long long)*block_canary(b));
    }

    b->magic = KHEAP_MAGIC_FREE;
    if (g_alloc_count > 0) {
        g_alloc_count--;
    }

    /* Coalesce with next free block */
    if (b->next && b->next->magic == KHEAP_MAGIC_FREE &&
        ((uint8_t *)b + OVERHEAD + b->size == (uint8_t *)b->next)) {
        struct kheap_block *nxt = b->next;
        b->size += OVERHEAD + nxt->size;
        b->next = nxt->next;
        if (b->next) {
            b->next->prev = b;
        }
        *block_canary(b) = KHEAP_TAIL_CANARY;
    }

    /* Coalesce with previous free block */
    if (b->prev && b->prev->magic == KHEAP_MAGIC_FREE &&
        ((uint8_t *)b->prev + OVERHEAD + b->prev->size == (uint8_t *)b)) {
        struct kheap_block *prv = b->prev;
        prv->size += OVERHEAD + b->size;
        prv->next = b->next;
        if (prv->next) {
            prv->next->prev = prv;
        }
        *block_canary(prv) = KHEAP_TAIL_CANARY;
    }

    spin_unlock_irqrestore(&g_heap_lock, flags);
}

void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) {
        return kmalloc(new_size);
    }
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }
    struct kheap_block *b = (struct kheap_block *)((uint8_t *)ptr - HDR_SIZE);
    size_t old_size = b->size;
    if (old_size >= new_size) {
        return ptr;
    }
    void *np = kmalloc(new_size);
    if (!np) {
        return NULL;
    }
    memcpy(np, ptr, old_size);
    kfree(ptr);
    return np;
}

void kheap_get_stats(struct kheap_stats *out) {
    uint64_t flags = spin_lock_irqsave(&g_heap_lock);
    out->mapped_bytes = (size_t)(g_heap_end - KHEAP_VIRT_BASE);
    out->used_bytes   = 0;
    out->free_bytes   = 0;
    out->alloc_count  = g_alloc_count;
    for (struct kheap_block *b = g_head; b != NULL; b = b->next) {
        if (b->magic == KHEAP_MAGIC_USED) {
            out->used_bytes += b->size;
        } else {
            out->free_bytes += b->size;
        }
    }
    spin_unlock_irqrestore(&g_heap_lock, flags);
}
