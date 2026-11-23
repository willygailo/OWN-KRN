#include <arch/idt.h>
#include <arch/io.h>
#include <arch/pit.h>
#include <drivers/ata.h>
#include <drivers/pci.h>
#include <drivers/rtc.h>
#include <fs/vfs.h>
#include <generated/autoconf.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <mm/kmalloc.h>
#include <mm/pmm.h>

enum proc_kind {
    PROC_VERSION    = 1,
    PROC_MEMINFO    = 2,
    PROC_UPTIME     = 3,
    PROC_TASKS      = 4,
    PROC_RTC        = 5,
    PROC_CPUINFO    = 6,
    PROC_INTERRUPTS = 7,
    PROC_MOUNTS     = 8,
    PROC_KMSG       = 9,
    PROC_PCI        = 10,
    PROC_PARTITIONS = 11,
    PROC_CMDLINE    = 12,
    PROC_CONFIG     = 13,
};

struct proc_entry {
    const char      *name;
    struct vfs_node *node;
};

static struct proc_entry g_procs[13];
static struct vfs_node  *g_proc_root = NULL;

static const char *task_state_str(enum task_state st) {
    switch (st) {
        case TASK_READY:    return "READY";
        case TASK_RUNNING:  return "RUN";
        case TASK_SLEEPING: return "SLEEP";
        case TASK_BLOCKED:  return "WAIT";
        case TASK_ZOMBIE:   return "ZOMBIE";
        default:            return "NONE";
    }
}

static size_t format_cpuinfo(char *buf, size_t cap) {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    char vendor[13];
    memset(vendor, 0, sizeof(vendor));
    cpuid_leaf(0, 0, &eax, &ebx, &ecx, &edx);
    memcpy(vendor + 0, &ebx, 4);
    memcpy(vendor + 4, &edx, 4);
    memcpy(vendor + 8, &ecx, 4);

    uint32_t fam_info = 0;
    cpuid_leaf(1, 0, &fam_info, &ebx, &ecx, &edx);
    uint32_t stepping = fam_info & 0xFU;
    uint32_t model    = (fam_info >> 4) & 0xFU;
    uint32_t family   = (fam_info >> 8) & 0xFU;
    if (family == 6 || family == 15) {
        model |= ((fam_info >> 16) & 0xFU) << 4;
    }

    char brand[49];
    memset(brand, 0, sizeof(brand));
    uint32_t max_ext = 0;
    cpuid_leaf(0x80000000U, 0, &max_ext, NULL, NULL, NULL);
    if (max_ext >= 0x80000004U) {
        uint32_t *bp = (uint32_t *)brand;
        cpuid_leaf(0x80000002U, 0, &bp[0], &bp[1], &bp[2], &bp[3]);
        cpuid_leaf(0x80000003U, 0, &bp[4], &bp[5], &bp[6], &bp[7]);
        cpuid_leaf(0x80000004U, 0, &bp[8], &bp[9], &bp[10], &bp[11]);
    } else {
        strcpy(brand, "QEMU Virtual x86_64 Processor");
    }

    return (size_t)snprintf(buf, cap,
        "processor       : 0\n"
        "vendor_id       : %s\n"
        "cpu family      : %u\n"
        "model           : %u\n"
        "model name      : %s\n"
        "stepping        : %u\n"
        "flags           : fpu pse tsc msr pae apic cmov pat mmx fxsr sse sse2 syscall nx lm wp smep\n"
        "address sizes   : 40 bits physical, 48 bits virtual\n",
        vendor[0] ? vendor : "GenuineIntel",
        family, model, brand, stepping);
}

static size_t format_proc_content(enum proc_kind kind, char *buf, size_t cap) {
    switch (kind) {
        case PROC_VERSION:
            return (size_t)snprintf(buf, cap,
                "Linux-compatible OwnKrn version 0.3.0-x86_64 (root@ownkrn) (gcc C11 + NASM) #1 SMP PREEMPT\n");
        case PROC_MEMINFO: {
            struct pmm_stats ps;
            struct kheap_stats hs;
            pmm_get_stats(&ps);
            kheap_get_stats(&hs);
            return (size_t)snprintf(buf, cap,
                "MemTotal:     %10llu kB\n"
                "MemFree:      %10llu kB\n"
                "MemUsed:      %10llu kB\n"
                "KHeapMapped:  %10llu kB\n"
                "KHeapUsed:    %10llu B (%zu allocs)\n",
                (unsigned long long)(ps.total_bytes / 1024ULL),
                (unsigned long long)(ps.free_bytes / 1024ULL),
                (unsigned long long)((ps.total_bytes - ps.free_bytes) / 1024ULL),
                (unsigned long long)(hs.mapped_bytes / 1024ULL),
                (unsigned long long)hs.used_bytes,
                hs.alloc_count);
        }
        case PROC_UPTIME: {
            uint64_t ms = pit_uptime_ms();
            return (size_t)snprintf(buf, cap, "%llu.%03llu s (%llu ticks)\n",
                (unsigned long long)(ms / 1000ULL),
                (unsigned long long)(ms % 1000ULL),
                (unsigned long long)pit_ticks());
        }
        case PROC_TASKS: {
            size_t max = 0;
            struct task *tbl = sched_task_table(&max);
            size_t pos = (size_t)snprintf(buf, cap,
                "PID   PPID  STATE   RING  TICKS   NAME\n");
            for (size_t i = 0; i < max && pos + 64 < cap; i++) {
                if (tbl[i].state == TASK_UNUSED) continue;
                pos += (size_t)snprintf(buf + pos, cap - pos,
                    "%-5d %-5d %-7s %-5s %-7llu %s\n",
                    tbl[i].pid,
                    tbl[i].ppid,
                    task_state_str(tbl[i].state),
                    tbl[i].is_user ? "U3" : "K0",
                    (unsigned long long)tbl[i].ticks_used,
                    tbl[i].name);
            }
            return pos;
        }
        case PROC_RTC: {
            struct rtc_time rt;
            rtc_read(&rt);
            int64_t epoch = rtc_unix_seconds();
            return (size_t)snprintf(buf, cap,
                "%04u-%02u-%02u %02u:%02u:%02u UTC (epoch=%lld)\n",
                rt.year, rt.month, rt.day, rt.hour, rt.minute, rt.second,
                (long long)epoch);
        }
        case PROC_CPUINFO:
            return format_cpuinfo(buf, cap);
        case PROC_INTERRUPTS:
            return (size_t)snprintf(buf, cap,
                "VEC   SOURCE          COUNT       DESCRIPTION\n"
                "32    IRQ0 (PIC1)     %-11llu PIT 100 Hz System Timer\n"
                "33    IRQ1 (PIC1)     %-11llu PS/2 Keyboard Controller\n"
                "36    IRQ4 (PIC1)     %-11llu COM1 16550A Serial UART\n"
                "14    EXC14 (IDT)     %-11llu CPU Page Faults (#PF)\n"
                "MSR   SYSCALL (LSTAR) %-11llu Ring 3 System Call Dispatches\n",
                (unsigned long long)idt_get_irq_count(0),
                (unsigned long long)idt_get_irq_count(1),
                (unsigned long long)idt_get_irq_count(4),
                (unsigned long long)idt_get_pf_count(),
                (unsigned long long)idt_get_syscall_count());
        case PROC_MOUNTS: {
            struct kheap_stats hs;
            kheap_get_stats(&hs);
            uint32_t dfiles = 0, dmax = 0, dused = 0, dtot = 0;
            diskfs_get_stats(&dfiles, &dmax, &dused, &dtot);
            return (size_t)snprintf(buf, cap,
                "FILESYSTEM   MOUNT   USED         TOTAL        STATUS\n"
                "tmpfs        /       %-10llu B %-10llu B rw,ram\n"
                "diskfs(ata0) /disk   %-10u B %-10u B rw,persistent (%u/%u files)\n"
                "devfs        /dev    0 B          0 B          rw,chardev\n"
                "procfs       /proc   0 B          0 B          ro,telemetry\n",
                (unsigned long long)hs.used_bytes,
                (unsigned long long)hs.mapped_bytes,
                dused, dtot, dfiles, dmax);
        }
        case PROC_KMSG:
            return printk_read_kmsg(buf, cap);
        case PROC_PCI:
            return pci_format_proc(buf, cap);
        case PROC_PARTITIONS: {
            uint32_t secs = ata_sector_count();
            return (size_t)snprintf(buf, cap,
                "NAME   MAJ:MIN   BLOCKS   SIZE     TYPE   MOUNTPOINT\n"
                "ram0     1:0        920   920 KiB  rom    / (initramfs)\n"
                "hda      3:0       %4u   %4u KiB disk   /disk (ATA0 LBA)\n",
                (secs * 512U) / 1024U,
                (secs * 512U) / 1024U);
        }
        case PROC_CMDLINE:
            return (size_t)snprintf(buf, cap,
                "BOOT_IMAGE=/boot/kernel.elf root=/dev/ram0 rw console=tty0 console=ttyS0,115200n8\n");
        case PROC_CONFIG:
            return (size_t)snprintf(buf, cap, "%s", OWNKRN_IKCONFIG_TEXT);
        default:
            return 0;
    }
}

static ssize_t proc_read(struct vfs_node *node, void *buf, size_t len, size_t offset) {
    char *tmp = kmalloc(16384);
    if (!tmp) return -ENOMEM;
    size_t total = format_proc_content((enum proc_kind)(uintptr_t)node->priv,
                                       tmp, 16384);
    if (offset >= total) {
        kfree(tmp);
        return 0;
    }
    size_t avail = total - offset;
    if (len > avail) {
        len = avail;
    }
    memcpy(buf, tmp + offset, len);
    kfree(tmp);
    return (ssize_t)len;
}

static int proc_dir_lookup(struct vfs_node *dir, const char *name, struct vfs_node **out) {
    (void)dir;
    for (size_t i = 0; i < ARRAY_SIZE(g_procs); i++) {
        if (strcmp(g_procs[i].name, name) == 0) {
            *out = g_procs[i].node;
            return 0;
        }
    }
    return -ENOENT;
}

static int proc_dir_readdir(struct vfs_node *dir, size_t index, struct vfs_dirent *out) {
    if (index == 0) {
        out->ino = dir->ino;
        out->type = VFS_DIR;
        strcpy(out->name, ".");
        return 1;
    }
    if (index == 1) {
        out->ino = dir->parent ? dir->parent->ino : dir->ino;
        out->type = VFS_DIR;
        strcpy(out->name, "..");
        return 1;
    }
    size_t k = index - 2;
    if (k < ARRAY_SIZE(g_procs)) {
        out->ino = g_procs[k].node->ino;
        out->type = VFS_FILE;
        strcpy(out->name, g_procs[k].name);
        return 1;
    }
    return 0;
}

static const struct vfs_ops proc_file_ops = {
    .read = proc_read,
};

static const struct vfs_ops proc_dir_ops = {
    .lookup  = proc_dir_lookup,
    .readdir = proc_dir_readdir,
};

static struct vfs_node *make_proc(uint64_t ino, enum proc_kind kind, struct vfs_node *parent) {
    struct vfs_node *n = kzalloc(sizeof(*n));
    n->ino      = ino;
    n->type     = VFS_FILE;
    n->mode     = 0444;
    n->refcount = 1;
    n->ops      = &proc_file_ops;
    n->priv     = (void *)(uintptr_t)kind;
    n->parent   = parent;
    return n;
}

struct vfs_node *procfs_create_root(void) {
    g_proc_root = kzalloc(sizeof(*g_proc_root));
    g_proc_root->ino      = 2000;
    g_proc_root->type     = VFS_DIR;
    g_proc_root->mode     = 0555;
    g_proc_root->refcount = 1;
    g_proc_root->ops      = &proc_dir_ops;
    g_proc_root->parent   = g_proc_root;

    g_procs[0]  = (struct proc_entry){ "version",    make_proc(2001, PROC_VERSION,    g_proc_root) };
    g_procs[1]  = (struct proc_entry){ "meminfo",    make_proc(2002, PROC_MEMINFO,    g_proc_root) };
    g_procs[2]  = (struct proc_entry){ "uptime",     make_proc(2003, PROC_UPTIME,     g_proc_root) };
    g_procs[3]  = (struct proc_entry){ "tasks",      make_proc(2004, PROC_TASKS,      g_proc_root) };
    g_procs[4]  = (struct proc_entry){ "rtc",        make_proc(2005, PROC_RTC,        g_proc_root) };
    g_procs[5]  = (struct proc_entry){ "cpuinfo",    make_proc(2006, PROC_CPUINFO,    g_proc_root) };
    g_procs[6]  = (struct proc_entry){ "interrupts", make_proc(2007, PROC_INTERRUPTS, g_proc_root) };
    g_procs[7]  = (struct proc_entry){ "mounts",     make_proc(2008, PROC_MOUNTS,     g_proc_root) };
    g_procs[8]  = (struct proc_entry){ "kmsg",       make_proc(2009, PROC_KMSG,       g_proc_root) };
    g_procs[9]  = (struct proc_entry){ "pci",        make_proc(2010, PROC_PCI,        g_proc_root) };
    g_procs[10] = (struct proc_entry){ "partitions", make_proc(2011, PROC_PARTITIONS, g_proc_root) };
    g_procs[11] = (struct proc_entry){ "cmdline",    make_proc(2012, PROC_CMDLINE,    g_proc_root) };
    g_procs[12] = (struct proc_entry){ "config",     make_proc(2013, PROC_CONFIG,     g_proc_root) };

    return g_proc_root;
}
