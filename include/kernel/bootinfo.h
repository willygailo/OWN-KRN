#ifndef OWNKRN_KERNEL_BOOTINFO_H
#define OWNKRN_KERNEL_BOOTINFO_H

#include <kernel/types.h>

#define BOOT_MB2_MAGIC 0x36D76289ULL
#define BOOT_PVH_MAGIC 0x336EC578ULL

#define BOOT_MMAP_MAX  64

#define MMAP_TYPE_USABLE   1U
#define MMAP_TYPE_RESERVED 2U
#define MMAP_TYPE_ACPI_REC 3U
#define MMAP_TYPE_ACPI_NVS 4U
#define MMAP_TYPE_BAD      5U

struct boot_mmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
};

struct boot_info {
    const char *protocol;
    uint64_t    magic;
    uint64_t    raw_info_phys;
    uint64_t    raw_info_size;
    char        cmdline[256];

    struct boot_mmap_entry mmap[BOOT_MMAP_MAX];
    size_t      mmap_count;
    uint64_t    total_usable_ram;
    uint64_t    max_phys_addr;

    uint64_t    initrd_phys;
    uint64_t    initrd_size;
};

bool bootinfo_parse(uint64_t magic, uint64_t info_phys, struct boot_info *out);
void bootinfo_dump(const struct boot_info *info);

#endif /* OWNKRN_KERNEL_BOOTINFO_H */
