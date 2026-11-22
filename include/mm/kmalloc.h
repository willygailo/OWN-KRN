#ifndef OWNKRN_MM_KMALLOC_H
#define OWNKRN_MM_KMALLOC_H

#include <stddef.h>
#include <stdint.h>

struct kheap_stats {
    size_t mapped_bytes;
    size_t used_bytes;
    size_t free_bytes;
    size_t alloc_count;
};

void  kheap_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *krealloc(void *ptr, size_t new_size);
void  kfree(void *ptr);
void  kheap_get_stats(struct kheap_stats *out);

#endif /* OWNKRN_MM_KMALLOC_H */
