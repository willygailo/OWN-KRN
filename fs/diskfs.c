#include <drivers/ata.h>
#include <fs/vfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/sync.h>
#include <lib/string.h>
#include <mm/kmalloc.h>

#define DISKFS_MAGIC        0x4F574E4B524E4653ULL /* "OWNKRNFS" */
#define DISKFS_MAX_FILES    32
#define DISKFS_DIR_LBA      1U
#define DISKFS_DIR_SECTORS  4U
#define DISKFS_DATA_LBA     16U
#define DISKFS_FILE_SECTORS 64U
#define DISKFS_MAX_FILESIZE (DISKFS_FILE_SECTORS * ATA_SECTOR_SIZE) /* 32 KiB */

struct diskfs_super {
    uint64_t magic;
    uint32_t version;
    uint32_t max_files;
    uint32_t total_sectors;
    uint8_t  pad[492];
} PACKED;

struct diskfs_dirent {
    uint8_t  used;
    uint8_t  type;
    uint16_t mode;
    uint32_t size;
    uint32_t start_lba;
    char     name[52];
} PACKED;

static struct diskfs_super  g_super;
static struct diskfs_dirent g_dir[DISKFS_MAX_FILES];
static struct vfs_node     *g_nodes[DISKFS_MAX_FILES];
static uint8_t             *g_cache[DISKFS_MAX_FILES];
static struct vfs_node     *g_disk_root = NULL;
static spinlock_t           g_diskfs_lock = SPINLOCK_INIT;

static const struct vfs_ops diskfs_file_ops;
static const struct vfs_ops diskfs_dir_ops;

static void sync_dir_table_unlocked(void) {
    if (ata_is_present()) {
        ata_write_sectors(DISKFS_DIR_LBA, DISKFS_DIR_SECTORS, g_dir);
    }
}

static void sync_file_data_unlocked(size_t slot) {
    if (!ata_is_present() || slot >= DISKFS_MAX_FILES) return;
    uint32_t sz = g_dir[slot].size;
    uint8_t secs = (uint8_t)((sz + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE);
    if (secs == 0) secs = 1;
    if (secs > DISKFS_FILE_SECTORS) secs = DISKFS_FILE_SECTORS;
    ata_write_sectors(g_dir[slot].start_lba, secs, g_cache[slot]);
}

static struct vfs_node *alloc_slot_node_unlocked(size_t slot) {
    struct vfs_node *n = kzalloc(sizeof(*n));
    if (!n) return NULL;
    if (!g_cache[slot]) {
        g_cache[slot] = kzalloc(DISKFS_MAX_FILESIZE);
        if (!g_cache[slot]) {
            kfree(n);
            return NULL;
        }
    }
    n->ino      = 3001 + slot;
    n->type     = VFS_FILE;
    n->mode     = g_dir[slot].mode ? g_dir[slot].mode : 0644;
    n->size     = g_dir[slot].size;
    n->refcount = 1;
    n->ops      = &diskfs_file_ops;
    n->priv     = (void *)(uintptr_t)slot;
    n->parent   = g_disk_root;
    g_nodes[slot] = n;
    return n;
}

static ssize_t diskfs_read(struct vfs_node *node, void *buf, size_t len, size_t offset) {
    uint64_t irq = spin_lock_irqsave(&g_diskfs_lock);
    size_t slot = (size_t)(uintptr_t)node->priv;
    if (slot >= DISKFS_MAX_FILES || !g_dir[slot].used || offset >= node->size) {
        spin_unlock_irqrestore(&g_diskfs_lock, irq);
        return 0;
    }
    size_t avail = node->size - offset;
    if (len > avail) len = avail;
    memcpy(buf, g_cache[slot] + offset, len);
    spin_unlock_irqrestore(&g_diskfs_lock, irq);
    return (ssize_t)len;
}

static ssize_t diskfs_write(struct vfs_node *node, const void *buf, size_t len, size_t offset) {
    uint64_t irq = spin_lock_irqsave(&g_diskfs_lock);
    size_t slot = (size_t)(uintptr_t)node->priv;
    if (slot >= DISKFS_MAX_FILES || !g_dir[slot].used) {
        spin_unlock_irqrestore(&g_diskfs_lock, irq);
        return -ENOENT;
    }
    if (offset >= DISKFS_MAX_FILESIZE) {
        spin_unlock_irqrestore(&g_diskfs_lock, irq);
        return -ENOSPC;
    }
    if (offset + len > DISKFS_MAX_FILESIZE) {
        len = DISKFS_MAX_FILESIZE - offset;
    }
    memcpy(g_cache[slot] + offset, buf, len);
    if (offset + len > node->size) {
        node->size = offset + len;
        g_dir[slot].size = (uint32_t)node->size;
    }
    sync_file_data_unlocked(slot);
    sync_dir_table_unlocked();
    spin_unlock_irqrestore(&g_diskfs_lock, irq);
    return (ssize_t)len;
}

static int diskfs_truncate(struct vfs_node *node, size_t new_size) {
    uint64_t irq = spin_lock_irqsave(&g_diskfs_lock);
    size_t slot = (size_t)(uintptr_t)node->priv;
    if (slot >= DISKFS_MAX_FILES || !g_dir[slot].used) {
        spin_unlock_irqrestore(&g_diskfs_lock, irq);
        return -ENOENT;
    }
    if (new_size > DISKFS_MAX_FILESIZE) new_size = DISKFS_MAX_FILESIZE;
    if (new_size < node->size && g_cache[slot]) {
        memset(g_cache[slot] + new_size, 0, node->size - new_size);
    }
    node->size = new_size;
    g_dir[slot].size = (uint32_t)new_size;
    sync_file_data_unlocked(slot);
    sync_dir_table_unlocked();
    spin_unlock_irqrestore(&g_diskfs_lock, irq);
    return 0;
}

static int diskfs_lookup(struct vfs_node *dir, const char *name, struct vfs_node **out) {
    (void)dir;
    uint64_t irq = spin_lock_irqsave(&g_diskfs_lock);
    for (size_t i = 0; i < DISKFS_MAX_FILES; i++) {
        if (g_dir[i].used && strcmp(g_dir[i].name, name) == 0) {
            *out = g_nodes[i];
            spin_unlock_irqrestore(&g_diskfs_lock, irq);
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_diskfs_lock, irq);
    return -ENOENT;
}

static int diskfs_readdir(struct vfs_node *dir, size_t index, struct vfs_dirent *out) {
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

    uint64_t irq = spin_lock_irqsave(&g_diskfs_lock);
    size_t seen = 2;
    for (size_t i = 0; i < DISKFS_MAX_FILES; i++) {
        if (!g_dir[i].used) continue;
        if (seen == index) {
            out->ino  = 3001 + i;
            out->type = VFS_FILE;
            strncpy(out->name, g_dir[i].name, VFS_NAME_MAX - 1);
            out->name[VFS_NAME_MAX - 1] = '\0';
            spin_unlock_irqrestore(&g_diskfs_lock, irq);
            return 1;
        }
        seen++;
    }
    spin_unlock_irqrestore(&g_diskfs_lock, irq);
    return 0;
}

static int diskfs_create(struct vfs_node *dir, const char *name, enum vfs_type type,
                         uint32_t mode, struct vfs_node **out) {
    (void)dir;
    if (type != VFS_FILE) return -EINVAL;

    uint64_t irq = spin_lock_irqsave(&g_diskfs_lock);
    for (size_t i = 0; i < DISKFS_MAX_FILES; i++) {
        if (g_dir[i].used && strcmp(g_dir[i].name, name) == 0) {
            spin_unlock_irqrestore(&g_diskfs_lock, irq);
            return -EEXIST;
        }
    }

    size_t slot = DISKFS_MAX_FILES;
    for (size_t i = 0; i < DISKFS_MAX_FILES; i++) {
        if (!g_dir[i].used) {
            slot = i;
            break;
        }
    }
    if (slot >= DISKFS_MAX_FILES) {
        spin_unlock_irqrestore(&g_diskfs_lock, irq);
        return -ENOSPC;
    }

    memset(&g_dir[slot], 0, sizeof(g_dir[slot]));
    g_dir[slot].used      = 1;
    g_dir[slot].type      = (uint8_t)VFS_FILE;
    g_dir[slot].mode      = (uint16_t)(mode ? mode : 0644);
    g_dir[slot].size      = 0;
    g_dir[slot].start_lba = DISKFS_DATA_LBA + (uint32_t)slot * DISKFS_FILE_SECTORS;
    strncpy(g_dir[slot].name, name, sizeof(g_dir[slot].name) - 1);

    struct vfs_node *n = alloc_slot_node_unlocked(slot);
    if (!n) {
        g_dir[slot].used = 0;
        spin_unlock_irqrestore(&g_diskfs_lock, irq);
        return -ENOMEM;
    }
    memset(g_cache[slot], 0, DISKFS_MAX_FILESIZE);
    sync_dir_table_unlocked();
    if (out) *out = n;
    spin_unlock_irqrestore(&g_diskfs_lock, irq);
    return 0;
}

static int diskfs_unlink(struct vfs_node *dir, const char *name) {
    (void)dir;
    uint64_t irq = spin_lock_irqsave(&g_diskfs_lock);
    for (size_t i = 0; i < DISKFS_MAX_FILES; i++) {
        if (g_dir[i].used && strcmp(g_dir[i].name, name) == 0) {
            struct vfs_node *n = g_nodes[i];
            g_dir[i].used = 0;
            g_dir[i].size = 0;
            g_dir[i].name[0] = '\0';
            g_nodes[i] = NULL;
            sync_dir_table_unlocked();
            spin_unlock_irqrestore(&g_diskfs_lock, irq);
            if (n) vfs_node_unref(n);
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_diskfs_lock, irq);
    return -ENOENT;
}

static const struct vfs_ops diskfs_file_ops = {
    .read     = diskfs_read,
    .write    = diskfs_write,
    .truncate = diskfs_truncate,
};

static const struct vfs_ops diskfs_dir_ops = {
    .lookup  = diskfs_lookup,
    .readdir = diskfs_readdir,
    .create  = diskfs_create,
    .unlink  = diskfs_unlink,
};

void diskfs_get_stats(uint32_t *out_files, uint32_t *out_max_files,
                      uint32_t *out_used_bytes, uint32_t *out_total_bytes) {
    uint64_t irq = spin_lock_irqsave(&g_diskfs_lock);
    uint32_t files = 0, used = 0;
    for (size_t i = 0; i < DISKFS_MAX_FILES; i++) {
        if (g_dir[i].used) {
            files++;
            used += g_dir[i].size;
        }
    }
    spin_unlock_irqrestore(&g_diskfs_lock, irq);
    if (out_files) *out_files = files;
    if (out_max_files) *out_max_files = DISKFS_MAX_FILES;
    if (out_used_bytes) *out_used_bytes = used;
    if (out_total_bytes) *out_total_bytes = DISKFS_MAX_FILES * DISKFS_MAX_FILESIZE;
}

struct vfs_node *diskfs_create_root(void) {
    g_disk_root = kzalloc(sizeof(*g_disk_root));
    g_disk_root->ino      = 3000;
    g_disk_root->type     = VFS_DIR;
    g_disk_root->mode     = 0755;
    g_disk_root->refcount = 1;
    g_disk_root->ops      = &diskfs_dir_ops;
    g_disk_root->parent   = g_disk_root;

    memset(&g_super, 0, sizeof(g_super));
    memset(g_dir, 0, sizeof(g_dir));

    if (ata_is_present() && ata_read_sectors(0, 1, &g_super) == 0 &&
        g_super.magic == DISKFS_MAGIC) {
        ata_read_sectors(DISKFS_DIR_LBA, DISKFS_DIR_SECTORS, g_dir);
        uint32_t loaded = 0;
        for (size_t i = 0; i < DISKFS_MAX_FILES; i++) {
            if (!g_dir[i].used) continue;
            if (g_dir[i].size > DISKFS_MAX_FILESIZE) {
                g_dir[i].size = DISKFS_MAX_FILESIZE;
            }
            struct vfs_node *n = alloc_slot_node_unlocked(i);
            if (n && g_dir[i].size > 0) {
                uint8_t secs = (uint8_t)((g_dir[i].size + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE);
                ata_read_sectors(g_dir[i].start_lba, secs, g_cache[i]);
            }
            loaded++;
        }
        pr_ok("DiskFS mounted at /disk (%u persistent files restored from ATA0)\n", loaded);
    } else {
        g_super.magic         = DISKFS_MAGIC;
        g_super.version       = 1;
        g_super.max_files     = DISKFS_MAX_FILES;
        g_super.total_sectors = ata_sector_count();

        const char *readme =
            "OwnKrn Persistent Disk (/disk) - Backed by ATA0 28-bit LBA sectors.\n"
            "Any file you create or edit in /disk (e.g. 'edit /disk/notes.txt')\n"
            "is written directly to build/disk.img and persists across reboots!\n";

        g_dir[0].used      = 1;
        g_dir[0].type      = (uint8_t)VFS_FILE;
        g_dir[0].mode      = 0644;
        g_dir[0].size      = (uint32_t)strlen(readme);
        g_dir[0].start_lba = DISKFS_DATA_LBA;
        strcpy(g_dir[0].name, "README.txt");

        struct vfs_node *n = alloc_slot_node_unlocked(0);
        if (n) {
            memcpy(g_cache[0], readme, g_dir[0].size);
            if (ata_is_present()) {
                ata_write_sectors(0, 1, &g_super);
                sync_file_data_unlocked(0);
                sync_dir_table_unlocked();
                pr_ok("DiskFS formatted & mounted at /disk (ATA0 persistent storage ready)\n");
            }
        }
    }

    return g_disk_root;
}
