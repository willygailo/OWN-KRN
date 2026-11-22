#include <arch/pit.h>
#include <fs/vfs.h>
#include <kernel/errno.h>
#include <kernel/sched.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <mm/kmalloc.h>
#include <mm/pmm.h>

enum proc_kind {
    PROC_VERSION = 1,
    PROC_MEMINFO = 2,
    PROC_UPTIME  = 3,
    PROC_TASKS   = 4,
};

struct proc_entry {
    const char      *name;
    struct vfs_node *node;
};

static struct proc_entry g_procs[4];
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

static size_t format_proc_content(enum proc_kind kind, char *buf, size_t cap) {
    switch (kind) {
        case PROC_VERSION:
            return (size_t)snprintf(buf, cap,
                "OwnKrn 0.1.0-x86_64 (freestanding C11 + NASM, higher-half)\n");
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
        default:
            return 0;
    }
}

static ssize_t proc_read(struct vfs_node *node, void *buf, size_t len, size_t offset) {
    char tmp[2048];
    size_t total = format_proc_content((enum proc_kind)(uintptr_t)node->priv,
                                       tmp, sizeof(tmp));
    if (offset >= total) {
        return 0;
    }
    size_t avail = total - offset;
    if (len > avail) {
        len = avail;
    }
    memcpy(buf, tmp + offset, len);
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

    g_procs[0] = (struct proc_entry){ "version", make_proc(2001, PROC_VERSION, g_proc_root) };
    g_procs[1] = (struct proc_entry){ "meminfo", make_proc(2002, PROC_MEMINFO, g_proc_root) };
    g_procs[2] = (struct proc_entry){ "uptime",  make_proc(2003, PROC_UPTIME,  g_proc_root) };
    g_procs[3] = (struct proc_entry){ "tasks",   make_proc(2004, PROC_TASKS,   g_proc_root) };

    return g_proc_root;
}
