#include <arch/io.h>
#include <arch/pit.h>
#include <drivers/serial.h>
#include <drivers/speaker.h>
#include <drivers/tty.h>
#include <fs/vfs.h>
#include <kernel/errno.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <mm/kmalloc.h>

enum dev_kind {
    DEV_NULL      = 1,
    DEV_ZERO      = 2,
    DEV_TTY       = 3,
    DEV_SER       = 4,
    DEV_URANDOM   = 5,
    DEV_SPEAKER   = 6,
    DEV_CLIPBOARD = 7,
};

struct dev_entry {
    const char      *name;
    struct vfs_node *node;
};

#define CLIPBOARD_MAX 16384
static char             g_clip_buf[CLIPBOARD_MAX];
static size_t           g_clip_len = 0;
static struct dev_entry g_devs[7];
static struct vfs_node *g_dev_root = NULL;
static uint64_t         g_prng_state = 0x853C49E6748FEA9BULL;

static inline uint64_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t next_urandom64(void) {
    g_prng_state ^= read_tsc() ^ (pit_ticks() * 0x9E3779B97F4A7C15ULL);
    uint64_t z = (g_prng_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static ssize_t dev_read(struct vfs_node *node, void *buf, size_t len, size_t offset) {
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
        case DEV_URANDOM: {
            (void)offset;
            uint8_t *dst = (uint8_t *)buf;
            size_t i = 0;
            while (i < len) {
                uint64_t r = next_urandom64();
                for (int b = 0; b < 8 && i < len; b++, i++) {
                    dst[i] = (uint8_t)((r >> (b * 8)) & 0xFFU);
                }
            }
            return (ssize_t)len;
        }
        case DEV_CLIPBOARD: {
            static char snap[CLIPBOARD_MAX];
            if (offset == 0) {
                g_clip_len = serial_clipboard_read(snap, sizeof(snap) - 1);
                snap[g_clip_len] = '\0';
                memcpy(g_clip_buf, snap, g_clip_len + 1);
            }
            if (offset >= g_clip_len) {
                return 0;
            }
            size_t avail = g_clip_len - offset;
            if (avail > len) {
                avail = len;
            }
            memcpy(buf, g_clip_buf + offset, avail);
            return (ssize_t)avail;
        }
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
        case DEV_URANDOM:
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
        case DEV_SPEAKER: {
            const char *s = (const char *)buf;
            uint32_t freq = 0, ms = 0;
            size_t i = 0;
            while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
            while (i < len && s[i] >= '0' && s[i] <= '9') {
                freq = freq * 10U + (uint32_t)(s[i] - '0');
                i++;
            }
            while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
            while (i < len && s[i] >= '0' && s[i] <= '9') {
                ms = ms * 10U + (uint32_t)(s[i] - '0');
                i++;
            }
            if (freq == 0) freq = 880;
            if (ms == 0) ms = 60;
            speaker_beep(freq, ms);
            return (ssize_t)len;
        }
        case DEV_CLIPBOARD: {
            if (offset == 0) {
                g_clip_len = 0;
            }
            if (offset < CLIPBOARD_MAX - 1) {
                size_t space = (CLIPBOARD_MAX - 1) - offset;
                size_t to_copy = (len < space) ? len : space;
                memcpy(g_clip_buf + offset, buf, to_copy);
                if (offset + to_copy > g_clip_len) {
                    g_clip_len = offset + to_copy;
                }
                g_clip_buf[g_clip_len] = '\0';
                node->size = g_clip_len;
                serial_clipboard_write(g_clip_buf, g_clip_len);
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

    g_devs[0] = (struct dev_entry){ "null",      make_dev(1001, DEV_NULL,      g_dev_root) };
    g_devs[1] = (struct dev_entry){ "zero",      make_dev(1002, DEV_ZERO,      g_dev_root) };
    g_devs[2] = (struct dev_entry){ "tty",       make_dev(1003, DEV_TTY,       g_dev_root) };
    g_devs[3] = (struct dev_entry){ "serial",    make_dev(1004, DEV_SER,       g_dev_root) };
    g_devs[4] = (struct dev_entry){ "urandom",   make_dev(1005, DEV_URANDOM,   g_dev_root) };
    g_devs[5] = (struct dev_entry){ "speaker",   make_dev(1006, DEV_SPEAKER,   g_dev_root) };
    g_devs[6] = (struct dev_entry){ "clipboard", make_dev(1007, DEV_CLIPBOARD, g_dev_root) };

    return g_dev_root;
}
