#include <drivers/serial.h>
#include <drivers/tty.h>
#include <fs/vfs.h>
#include <kernel/errno.h>
#include <lib/string.h>
#include <mm/kmalloc.h>

enum dev_kind {
    DEV_NULL = 1,
    DEV_ZERO = 2,
    DEV_TTY  = 3,
    DEV_SER  = 4,
};

struct dev_entry {
    const char      *name;
    struct vfs_node *node;
};

static struct dev_entry g_devs[4];
static struct vfs_node *g_dev_root = NULL;

static ssize_t dev_read(struct vfs_node *node, void *buf, size_t len, size_t offset) {
    (void)offset;
    enum dev_kind kind = (enum dev_kind)(uintptr_t)node->priv;
    switch (kind) {
        case DEV_NULL:
            return 0;
        case DEV_ZERO:
            memset(buf, 0, len);
            return (ssize_t)len;
        case DEV_TTY:
        case DEV_SER:
            return tty_read(buf, len);
        default:
            return -ENODEV;
    }
}

static ssize_t dev_write(struct vfs_node *node, const void *buf, size_t len, size_t offset) {
    (void)offset;
    enum dev_kind kind = (enum dev_kind)(uintptr_t)node->priv;
    switch (kind) {
        case DEV_NULL:
        case DEV_ZERO:
            return (ssize_t)len;
        case DEV_TTY:
            return tty_write(buf, len);
        case DEV_SER: {
            const char *s = (const char *)buf;
            for (size_t i = 0; i < len; i++) {
                serial_putc(s[i]);
            }
            return (ssize_t)len;
        }
        default:
            return -ENODEV;
    }
}

static int dev_dir_lookup(struct vfs_node *dir, const char *name, struct vfs_node **out) {
    (void)dir;
    for (size_t i = 0; i < ARRAY_SIZE(g_devs); i++) {
        if (strcmp(g_devs[i].name, name) == 0) {
            *out = g_devs[i].node;
            return 0;
        }
    }
    return -ENOENT;
}

static int dev_dir_readdir(struct vfs_node *dir, size_t index, struct vfs_dirent *out) {
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
    if (k < ARRAY_SIZE(g_devs)) {
        out->ino = g_devs[k].node->ino;
        out->type = VFS_CHARDEV;
        strcpy(out->name, g_devs[k].name);
        return 1;
    }
    return 0;
}

static const struct vfs_ops dev_file_ops = {
    .read  = dev_read,
    .write = dev_write,
};

static const struct vfs_ops dev_dir_ops = {
    .lookup  = dev_dir_lookup,
    .readdir = dev_dir_readdir,
};

static struct vfs_node *make_dev(uint64_t ino, enum dev_kind kind, struct vfs_node *parent) {
    struct vfs_node *n = kzalloc(sizeof(*n));
    n->ino      = ino;
    n->type     = VFS_CHARDEV;
    n->mode     = 0666;
    n->refcount = 1;
    n->ops      = &dev_file_ops;
    n->priv     = (void *)(uintptr_t)kind;
    n->parent   = parent;
    return n;
}

struct vfs_node *devfs_create_root(void) {
    g_dev_root = kzalloc(sizeof(*g_dev_root));
    g_dev_root->ino      = 1000;
    g_dev_root->type     = VFS_DIR;
    g_dev_root->mode     = 0755;
    g_dev_root->refcount = 1;
    g_dev_root->ops      = &dev_dir_ops;
    g_dev_root->parent   = g_dev_root;

    g_devs[0] = (struct dev_entry){ "null",   make_dev(1001, DEV_NULL, g_dev_root) };
    g_devs[1] = (struct dev_entry){ "zero",   make_dev(1002, DEV_ZERO, g_dev_root) };
    g_devs[2] = (struct dev_entry){ "tty",    make_dev(1003, DEV_TTY,  g_dev_root) };
    g_devs[3] = (struct dev_entry){ "serial", make_dev(1004, DEV_SER,  g_dev_root) };

    return g_dev_root;
}
