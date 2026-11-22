#ifndef OWNKRN_KERNEL_TYPES_H
#define OWNKRN_KERNEL_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int64_t  ssize_t;
typedef int64_t  off_t;
typedef int32_t  pid_t;
typedef uint32_t mode_t;

#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ULL
#define HHDM_BASE        0xFFFF800000000000ULL
#define KHEAP_VIRT_BASE  0xFFFFC00000000000ULL
#define USER_VIRT_MAX    0x00007FFFFFFFFFFFULL

#define PAGE_SIZE        4096ULL
#define PAGE_SHIFT       12U
#define PAGE_MASK        (~(PAGE_SIZE - 1ULL))

#define PHYS_TO_VIRT(pa) ((void *)((uint64_t)(pa) + HHDM_BASE))
#define VIRT_TO_PHYS(va) ((uint64_t)(va) - HHDM_BASE)
#define KVIRT_TO_PHYS(va) ((uint64_t)(va) - KERNEL_VIRT_BASE)

#define ALIGN_UP(x, a)   (((uint64_t)(x) + ((uint64_t)(a) - 1ULL)) & ~((uint64_t)(a) - 1ULL))
#define ALIGN_DOWN(x, a) ((uint64_t)(x) & ~((uint64_t)(a) - 1ULL))
#define BIT(n)           (1ULL << (n))
#define ARRAY_SIZE(arr)  (sizeof(arr) / sizeof((arr)[0]))

#define PACKED           __attribute__((packed))
#define NORETURN         __attribute__((noreturn))
#define UNUSED           __attribute__((unused))
#define ALIGNED(x)       __attribute__((aligned(x)))

/* Linker symbols */
extern char __kernel_phys_start[];
extern char __kernel_virt_base[];
extern char __kernel_start[];
extern char __text_start[];
extern char __text_end[];
extern char __rodata_start[];
extern char __rodata_end[];
extern char __data_start[];
extern char __data_end[];
extern char __bss_start[];
extern char __bss_end[];
extern char __kernel_end[];
extern char __kernel_phys_end[];

#endif /* OWNKRN_KERNEL_TYPES_H */
