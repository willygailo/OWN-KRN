#include <fs/vfs.h>
#include <kernel/errno.h>
#include <lib/string.h>
#include <mm/kmalloc.h>

struct tmpfs_child {
    char                name[VFS_NAME_MAX];
    struct vfs_node    *node;
    struct tmpfs_child *next;
};

struct tmpfs_data {
    uint8_t            *buf;
    size_t              cap;
    struct tmpfs_child *children;
};

static uint64_t g_next_ino = 1;
static const struct vfs_ops tmpfs_ops;

static struct vfs_node *tmpfs_alloc_node(enum vfs_type type, uint32_t mode,
                                         struct vfs_node *parent) {
    struct vfs_node *n = kzalloc(sizeof(*n));
    struct tmpfs_data *d = kzalloc(sizeof(*d));
    if (!n || !d) {
        kfree(n);
        kfree(d);
        return NULL;
    }
    n->ino      = g_next_ino++;
    n->type     = type;
    n->mode     = mode;
    n->size     = 0;
    n->refcount = 1;
    n->ops      = &tmpfs_ops;
    n->priv     = d;
    n->parent   = parent ? parent : n;
    return n;
}

static ssize_t tmpfs_read(struct vfs_node *node, void *buf, size_t len, size_t offset) {
    struct tmpfs_data *d = (struct tmpfs_data *)node->priv;
    if (offset >= node->size) {
        return 0;
    }
    size_t avail = node->size - offset;
    if (len > avail) {
        len = avail;
    }
    memcpy(buf, d->buf + offset, len);
    return (ssize_t)len;
}

static ssize_t tmpfs_write(struct vfs_node *node, const void *buf, size_t len, size_t offset) {
    struct tmpfs_data *d = (struct tmpfs_data *)node->priv;
    size_t end = offset + len;
    if (end > d->cap) {
        size_t new_cap = d->cap ? d->cap * 2 : 256;
        while (new_cap < end) {
            new_cap *= 2;
        }
        uint8_t *nb = krealloc(d->buf, new_cap);
        if (!nb) {
            return -ENOMEM;
        }
        if (new_cap > d->cap) {
            memset(nb + d->cap, 0, new_cap - d->cap);
        }
        d->buf = nb;
        d->cap = new_cap;
    }
    memcpy(d->buf + offset, buf, len);
    if (end > node->size) {
        node->size = end;
    }
    return (ssize_t)len;
}

static int tmpfs_truncate(struct vfs_node *node, size_t new_size) {
    struct tmpfs_data *d = (struct tmpfs_data *)node->priv;
    if (new_size < node->size && d->buf) {
        memset(d->buf + new_size, 0, node->size - new_size);
    }
    node->size = new_size;
    return 0;
}

static int tmpfs_lookup(struct vfs_node *dir, const char *name, struct vfs_node **out) {
    struct tmpfs_data *d = (struct tmpfs_data *)dir->priv;
    for (struct tmpfs_child *c = d->children; c != NULL; c = c->next) {
        if (strcmp(c->name, name) == 0) {
            *out = c->node;
            return 0;
        }
    }
    return -ENOENT;
}

static int tmpfs_readdir(struct vfs_node *dir, size_t index, struct vfs_dirent *out) {
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

    struct tmpfs_data *d = (struct tmpfs_data *)dir->priv;
    size_t idx = 2;
    for (struct tmpfs_child *c = d->children; c != NULL; c = c->next, idx++) {
        if (idx == index) {
            out->ino  = c->node->ino;
            out->type = (uint32_t)c->node->type;
            strncpy(out->name, c->name, VFS_NAME_MAX - 1);
            out->name[VFS_NAME_MAX - 1] = '\0';
            return 1;
        }
    }
    return 0;
}

static int tmpfs_create(struct vfs_node *dir, const char *name, enum vfs_type type,
                        uint32_t mode, struct vfs_node **out) {
    struct tmpfs_data *d = (struct tmpfs_data *)dir->priv;
    for (struct tmpfs_child *c = d->children; c != NULL; c = c->next) {
        if (strcmp(c->name, name) == 0) {
            return -EEXIST;
        }
    }

    struct vfs_node *n = tmpfs_alloc_node(type, mode, dir);
    if (!n) return -ENOMEM;

    struct tmpfs_child *entry = kzalloc(sizeof(*entry));
    if (!entry) {
        kfree(n->priv);
        kfree(n);
        return -ENOMEM;
    }
    strncpy(entry->name, name, VFS_NAME_MAX - 1);
    entry->node = n;
    entry->next = d->children;
    d->children = entry;

    if (out) {
        *out = n;
    }
    return 0;
}

static int tmpfs_unlink(struct vfs_node *dir, const char *name) {
    struct tmpfs_data *d = (struct tmpfs_data *)dir->priv;
    struct tmpfs_child **pp = &d->children;
    while (*pp) {
        struct tmpfs_child *c = *pp;
        if (strcmp(c->name, name) == 0) {
            struct vfs_node *target = c->node;
            if (target->mount_root) {
                return -EBUSY;
            }
            struct tmpfs_data *td = (struct tmpfs_data *)target->priv;
            if (target->type == VFS_DIR && td->children != NULL) {
                return -ENOTEMPTY;
            }
            *pp = c->next;
            if (td->buf) {
                kfree(td->buf);
            }
            kfree(td);
            kfree(target);
            kfree(c);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -ENOENT;
}

static const struct vfs_ops tmpfs_ops = {
    .read     = tmpfs_read,
    .write    = tmpfs_write,
    .lookup   = tmpfs_lookup,
    .readdir  = tmpfs_readdir,
    .create   = tmpfs_create,
    .unlink   = tmpfs_unlink,
    .truncate = tmpfs_truncate,
};

struct vfs_node *tmpfs_create_root(void) {
    return tmpfs_alloc_node(VFS_DIR, 0755, NULL);
}
