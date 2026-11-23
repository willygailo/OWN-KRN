#include <kernel/bootinfo.h>
#include <kernel/printk.h>
#include <lib/string.h>

/* Multiboot2 structures */
struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

struct mb2_tag_cmdline {
    uint32_t type;
    uint32_t size;
    char     string[];
};

struct mb2_tag_module {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char     cmdline[];
};

struct mb2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
};

struct mb2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct mb2_mmap_entry entries[];
};

/* Xen/QEMU PVH start_info structures */
struct hvm_start_info {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t nr_modules;
    uint64_t modlist_paddr;
    uint64_t cmdline_paddr;
    uint64_t rsdp_paddr;
    uint64_t memmap_paddr;
    uint32_t memmap_entries;
    uint32_t reserved;
};

struct hvm_modlist_entry {
    uint64_t paddr;
    uint64_t size;
    uint64_t cmdline_paddr;
    uint64_t reserved;
};

struct hvm_memmap_entry {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
    uint32_t reserved;
};

static void record_mmap(struct boot_info *out, uint64_t base, uint64_t len, uint32_t type) {
    if (len == 0 || out->mmap_count >= BOOT_MMAP_MAX) {
        return;
    }
    struct boot_mmap_entry *e = &out->mmap[out->mmap_count++];
    e->base = base;
    e->length = len;
    e->type = type;

    uint64_t end = base + len;
    if (type == MMAP_TYPE_USABLE) {
        out->total_usable_ram += len;
        if (end > out->max_phys_addr) {
            out->max_phys_addr = end;
        }
    }
}

static bool parse_multiboot2(uint64_t info_phys, struct boot_info *out) {
    const uint8_t *base = (const uint8_t *)PHYS_TO_VIRT(info_phys);
    uint32_t total_size = *(const uint32_t *)base;

    out->protocol = "Multiboot2";
    out->raw_info_phys = info_phys;
    out->raw_info_size = total_size;

    uint32_t off = 8;
    while (off + sizeof(struct mb2_tag) <= total_size) {
        const struct mb2_tag *tag = (const struct mb2_tag *)(base + off);
        if (tag->type == 0) {
            break;
        }
        if (tag->size < 8) {
            break;
        }

        if (tag->type == 1) { /* Boot command line */
            const struct mb2_tag_cmdline *c = (const struct mb2_tag_cmdline *)tag;
            strncpy(out->cmdline, c->string, sizeof(out->cmdline) - 1);
            out->cmdline[sizeof(out->cmdline) - 1] = '\0';
        } else if (tag->type == 3) { /* Module (initrd) */
            const struct mb2_tag_module *m = (const struct mb2_tag_module *)tag;
            if (out->initrd_size == 0 && m->mod_end > m->mod_start) {
                out->initrd_phys = m->mod_start;
                out->initrd_size = m->mod_end - m->mod_start;
            }
        } else if (tag->type == 6) { /* Memory map */
            const struct mb2_tag_mmap *mm = (const struct mb2_tag_mmap *)tag;
            uint32_t pos = sizeof(struct mb2_tag_mmap);
            while (pos + mm->entry_size <= mm->size) {
                const struct mb2_mmap_entry *ent =
                    (const struct mb2_mmap_entry *)((const uint8_t *)mm + pos);
                record_mmap(out, ent->addr, ent->len, ent->type);
                pos += mm->entry_size;
            }
        }

        off = (uint32_t)ALIGN_UP(off + tag->size, 8);
    }

    return out->mmap_count > 0;
}

static bool parse_pvh(uint64_t info_phys, struct boot_info *out) {
    if (info_phys == 0 || info_phys >= 0x100000000ULL) {
        return false;
    }
    const struct hvm_start_info *si =
        (const struct hvm_start_info *)PHYS_TO_VIRT(info_phys);
    if (si->magic != BOOT_PVH_MAGIC) {
        return false;
    }

    out->protocol = "PVH (QEMU ELF64)";
    out->raw_info_phys = info_phys;
    out->raw_info_size = sizeof(struct hvm_start_info);

    if (si->cmdline_paddr && si->cmdline_paddr < 0x100000000ULL) {
        const char *cmd = (const char *)PHYS_TO_VIRT(si->cmdline_paddr);
        strncpy(out->cmdline, cmd, sizeof(out->cmdline) - 1);
        out->cmdline[sizeof(out->cmdline) - 1] = '\0';
    }

    if (si->nr_modules > 0 && si->modlist_paddr && si->modlist_paddr < 0x100000000ULL) {
        const struct hvm_modlist_entry *mods =
            (const struct hvm_modlist_entry *)PHYS_TO_VIRT(si->modlist_paddr);
        out->initrd_phys = mods[0].paddr;
        out->initrd_size = mods[0].size;
    }

    if (si->version >= 1 && si->memmap_paddr && si->memmap_paddr < 0x100000000ULL &&
        si->memmap_entries > 0) {
        const struct hvm_memmap_entry *ents =
            (const struct hvm_memmap_entry *)PHYS_TO_VIRT(si->memmap_paddr);
        uint32_t count = si->memmap_entries;
        if (count > BOOT_MMAP_MAX) {
            count = BOOT_MMAP_MAX;
        }
        for (uint32_t i = 0; i < count; i++) {
            record_mmap(out, ents[i].addr, ents[i].size, ents[i].type);
        }
    }

    return out->mmap_count > 0;
}

bool bootinfo_parse(uint64_t magic, uint64_t info_phys, struct boot_info *out) {
    memset(out, 0, sizeof(*out));
    out->magic = magic;

    if (magic == BOOT_MB2_MAGIC) {
        return parse_multiboot2(info_phys, out);
    }
    if (magic == BOOT_PVH_MAGIC) {
        return parse_pvh(info_phys, out);
    }
    out->protocol = "Unknown";
    return false;
}

static const char *mmap_type_str(uint32_t type) {
    switch (type) {
        case MMAP_TYPE_USABLE:   return "USABLE";
        case MMAP_TYPE_RESERVED: return "RESERVED";
        case MMAP_TYPE_ACPI_REC: return "ACPI_RECLAIM";
        case MMAP_TYPE_ACPI_NVS: return "ACPI_NVS";
        case MMAP_TYPE_BAD:      return "BAD_RAM";
        default:                 return "OTHER";
    }
}

void bootinfo_dump(const struct boot_info *info) {
    pr_info("Boot protocol : %s (magic=0x%08llx)\n",
            info->protocol, (unsigned long long)info->magic);
    if (info->cmdline[0]) {
        pr_info("Cmdline       : %s\n", info->cmdline);
    }
    if (info->initrd_size > 0) {
        pr_info("Initrd        : phys=0x%llx size=%llu bytes\n",
                (unsigned long long)info->initrd_phys,
                (unsigned long long)info->initrd_size);
    }
    pr_info("Memory map (%zu entries, usable=%llu MiB):\n",
            info->mmap_count,
            (unsigned long long)(info->total_usable_ram / (1024ULL * 1024ULL)));
    for (size_t i = 0; i < info->mmap_count; i++) {
        const struct boot_mmap_entry *e = &info->mmap[i];
        printk("  [%02zu] 0x%016llx - 0x%016llx (%10llu KiB) %s\n",
               i,
               (unsigned long long)e->base,
               (unsigned long long)(e->base + e->length - 1),
               (unsigned long long)(e->length / 1024ULL),
               mmap_type_str(e->type));
    }
}
