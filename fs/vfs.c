#include <fs/vfs.h>
#include <kernel/errno.h>
#include <kernel/panic.h>
#include <kernel/printk.h>
#include <kernel/sync.h>
#include <lib/string.h>
#include <mm/kmalloc.h>

extern void sched_set_close_fds_hook(void (*hook)(struct task *t));

static struct vfs_node *g_root = NULL;
static spinlock_t       g_vfs_ref_lock = SPINLOCK_INIT;

void vfs_node_ref(struct vfs_node *node) {
    if (!node) return;
    uint64_t flags = spin_lock_irqsave(&g_vfs_ref_lock);
    node->refcount++;
    spin_unlock_irqrestore(&g_vfs_ref_lock, flags);
}

void vfs_node_unref(struct vfs_node *node) {
    if (!node) return;
    uint64_t flags = spin_lock_irqsave(&g_vfs_ref_lock);
    bool should_release = false;
    if (node->refcount > 0) {
        node->refcount--;
        if (node->refcount == 0) {
            should_release = true;
        }
    }
    spin_unlock_irqrestore(&g_vfs_ref_lock, flags);

    if (should_release && node->ops && node->ops->release) {
        node->ops->release(node);
    }
}

void vfs_init(void) {
    g_root = tmpfs_create_root();
    KASSERT(g_root != NULL);

    KASSERT(vfs_mkdir("/", "/dev", 0755) == 0);
    KASSERT(vfs_mkdir("/", "/proc", 0755) == 0);
    KASSERT(vfs_mkdir("/", "/tmp", 0777) == 0);
    KASSERT(vfs_mkdir("/", "/bin", 0755) == 0);
    KASSERT(vfs_mkdir("/", "/etc", 0755) == 0);
    KASSERT(vfs_mkdir("/", "/disk", 0755) == 0);

    KASSERT(vfs_mount("/dev", devfs_create_root()) == 0);
    KASSERT(vfs_mount("/proc", procfs_create_root()) == 0);
    KASSERT(vfs_mount("/disk", diskfs_create_root()) == 0);

    sched_set_close_fds_hook(vfs_task_close_all);
    pr_ok("VFS initialized (root=tmpfs, /dev=devfs, /proc=procfs, /disk=diskfs)\n");
}

struct vfs_node *vfs_root(void) {
    return g_root;
}

int vfs_normalize_path(const char *cwd, const char *path, char *out, size_t out_sz) {
    if (!path || !path[0] || !out || out_sz < 2) {
        return -EINVAL;
    }

    char combined[VFS_PATH_MAX * 2];
    if (path[0] == '/') {
        if (strlen(path) >= sizeof(combined)) return -ENAMETOOLONG;
        strcpy(combined, path);
    } else {
        const char *base = (cwd && cwd[0]) ? cwd : "/";
        size_t blen = strlen(base);
        size_t plen = strlen(path);
        if (blen + plen + 2 >= sizeof(combined)) return -ENAMETOOLONG;
        strcpy(combined, base);
        if (blen == 0 || combined[blen - 1] != '/') {
            combined[blen++] = '/';
        }
        strcpy(combined + blen, path);
    }

    const char *parts[64];
    size_t part_lens[64];
    size_t depth = 0;

    const char *p = combined;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        const char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);

        if (len == 1 && start[0] == '.') {
            continue;
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (depth > 0) depth--;
            continue;
        }
        if (len >= VFS_NAME_MAX || depth >= ARRAY_SIZE(parts)) {
            return -ENAMETOOLONG;
        }
        parts[depth] = start;
        part_lens[depth] = len;
        depth++;
    }

    if (depth == 0) {
        out[0] = '/';
        out[1] = '\0';
        return 0;
    }

    size_t pos = 0;
    for (size_t i = 0; i < depth; i++) {
        if (pos + 1 + part_lens[i] >= out_sz) {
            return -ENAMETOOLONG;
        }
        out[pos++] = '/';
        memcpy(out + pos, parts[i], part_lens[i]);
        pos += part_lens[i];
    }
    out[pos] = '\0';
    return 0;
}

static struct vfs_node *follow_mount(struct vfs_node *n) {
    while (n && n->mount_root) {
        n = n->mount_root;
    }
    return n;
}

int vfs_resolve(const char *cwd, const char *path, struct vfs_node **out) {
    char norm[VFS_PATH_MAX];
    int rc = vfs_normalize_path(cwd, path, norm, sizeof(norm));
    if (rc < 0) return rc;

    struct vfs_node *cur = follow_mount(g_root);
    if (strcmp(norm, "/") == 0) {
        *out = cur;
        return 0;
    }

    const char *p = norm + 1;
    while (*p) {
        if (cur->type != VFS_DIR) {
            return -ENOTDIR;
        }
        const char *slash = strchr(p, '/');
        char comp[VFS_NAME_MAX];
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len >= sizeof(comp)) return -ENAMETOOLONG;
        memcpy(comp, p, len);
        comp[len] = '\0';

        if (!cur->ops || !cur->ops->lookup) {
            return -ENOENT;
        }
        struct vfs_node *next = NULL;
        rc = cur->ops->lookup(cur, comp, &next);
        if (rc < 0) {
            return rc;
        }
        cur = follow_mount(next);
        if (!slash) {
            break;
        }
        p = slash + 1;
    }

    *out = cur;
    return 0;
}

int vfs_resolve_parent(const char *cwd, const char *path,
                       struct vfs_node **dir_out, char *leaf_out) {
    char norm[VFS_PATH_MAX];
    int rc = vfs_normalize_path(cwd, path, norm, sizeof(norm));
    if (rc < 0) return rc;

    if (strcmp(norm, "/") == 0) {
        return -EINVAL;
    }

    char *last = strrchr(norm, '/');
    if (!last || !last[1]) {
        return -EINVAL;
    }
    if (strlen(last + 1) >= VFS_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    strcpy(leaf_out, last + 1);

    if (last == norm) {
        *dir_out = follow_mount(g_root);
        return 0;
    }
    *last = '\0';
    rc = vfs_resolve("/", norm, dir_out);
    if (rc < 0) return rc;
    if ((*dir_out)->type != VFS_DIR) return -ENOTDIR;
    return 0;
}

int vfs_mount(const char *path, struct vfs_node *fs_root) {
    if (!fs_root) return -EINVAL;
    struct vfs_node *mp = NULL;
    int rc = vfs_resolve("/", path, &mp);
    if (rc < 0) return rc;
    if (mp->type != VFS_DIR) return -ENOTDIR;

    mp->mount_root = fs_root;
    fs_root->parent = mp->parent ? mp->parent : g_root;
    return 0;
}

int vfs_mkdir(const char *cwd, const char *path, uint32_t mode) {
    struct vfs_node *parent = NULL;
    char leaf[VFS_NAME_MAX];
    int rc = vfs_resolve_parent(cwd, path, &parent, leaf);
    if (rc < 0) return rc;
    if (!parent->ops || !parent->ops->create) return -EROFS;

    struct vfs_node *created = NULL;
    return parent->ops->create(parent, leaf, VFS_DIR, mode ? mode : 0755, &created);
}

int vfs_unlink(const char *cwd, const char *path) {
    struct vfs_node *parent = NULL;
    char leaf[VFS_NAME_MAX];
    int rc = vfs_resolve_parent(cwd, path, &parent, leaf);
    if (rc < 0) return rc;
    if (!parent->ops || !parent->ops->unlink) return -EROFS;
    return parent->ops->unlink(parent, leaf);
}

int vfs_rename(const char *cwd, const char *oldpath, const char *newpath) {
    struct vfs_node *src_node = NULL;
    int rc = vfs_resolve(cwd, oldpath, &src_node);
    if (rc < 0) return rc;
    if (src_node == g_root || src_node->mount_root) return -EBUSY;

    struct vfs_node *dst_existing = NULL;
    if (vfs_resolve(cwd, newpath, &dst_existing) == 0) {
        if (dst_existing == src_node) {
            return 0;
        }
        rc = vfs_unlink(cwd, newpath);
        if (rc < 0) return rc;
    }

    struct vfs_node *new_parent = NULL;
    char new_leaf[VFS_NAME_MAX];
    rc = vfs_resolve_parent(cwd, newpath, &new_parent, new_leaf);
    if (rc < 0) return rc;
    if (!new_parent->ops || !new_parent->ops->link) return -EXDEV;

    rc = new_parent->ops->link(new_parent, new_leaf, src_node);
    if (rc < 0) return rc;

    rc = vfs_unlink(cwd, oldpath);
    return rc;
}

int vfs_stat_node(struct vfs_node *node, struct vfs_stat *st) {
    if (!node || !st) return -EINVAL;
    st->ino  = node->ino;
    st->type = (uint32_t)node->type;
    st->mode = node->mode;
    st->size = node->size;
    return 0;
}

int vfs_stat_path(const char *cwd, const char *path, struct vfs_stat *st) {
    struct vfs_node *node = NULL;
    int rc = vfs_resolve(cwd, path, &node);
    if (rc < 0) return rc;
    return vfs_stat_node(node, st);
}

static int alloc_fd(struct task *t, struct file_desc *fd_obj) {
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        if (!t->fds[i]) {
            t->fds[i] = fd_obj;
            return i;
        }
    }
    return -EMFILE;
}

static struct file_desc *get_fd(struct task *t, int fd) {
    if (!t || fd < 0 || fd >= TASK_MAX_FDS) {
        return NULL;
    }
    return t->fds[fd];
}

int vfs_open(struct task *t, const char *path, uint32_t flags, uint32_t mode) {
    if (!t || !path) return -EINVAL;

    struct vfs_node *node = NULL;
    int rc = vfs_resolve(t->cwd, path, &node);
    if (rc == -ENOENT && (flags & O_CREAT)) {
        struct vfs_node *parent = NULL;
        char leaf[VFS_NAME_MAX];
        rc = vfs_resolve_parent(t->cwd, path, &parent, leaf);
        if (rc < 0) return rc;
        if (!parent->ops || !parent->ops->create) return -EROFS;
        rc = parent->ops->create(parent, leaf, VFS_FILE, mode ? mode : 0644, &node);
        if (rc < 0) return rc;
    } else if (rc < 0) {
        return rc;
    }

    if ((flags & O_TRUNC) && node->type == VFS_FILE &&
        ((flags & O_ACCMODE) == O_WRONLY || (flags & O_ACCMODE) == O_RDWR)) {
        if (node->ops && node->ops->truncate) {
            node->ops->truncate(node, 0);
        }
    }

    struct file_desc *f = kzalloc(sizeof(*f));
    if (!f) return -ENOMEM;

    vfs_node_ref(node);
    f->node     = node;
    f->offset   = (flags & O_APPEND) ? node->size : 0;
    f->flags    = flags;
    f->refcount = 1;

    int fd = alloc_fd(t, f);
    if (fd < 0) {
        vfs_node_unref(node);
        kfree(f);
        return fd;
    }
    return fd;
}

int vfs_close(struct task *t, int fd) {
    struct file_desc *f = get_fd(t, fd);
    if (!f) return -EBADF;
    t->fds[fd] = NULL;

    uint64_t irq = spin_lock_irqsave(&g_vfs_ref_lock);
    bool drop = false;
    if (f->refcount > 0) {
        f->refcount--;
        if (f->refcount == 0) {
            drop = true;
        }
    }
    spin_unlock_irqrestore(&g_vfs_ref_lock, irq);

    if (drop) {
        vfs_node_unref(f->node);
        kfree(f);
    }
    return 0;
}

int vfs_dup(struct task *t, int oldfd) {
    struct file_desc *f = get_fd(t, oldfd);
    if (!f) return -EBADF;

    int newfd = alloc_fd(t, f);
    if (newfd < 0) return newfd;

    uint64_t irq = spin_lock_irqsave(&g_vfs_ref_lock);
    f->refcount++;
    spin_unlock_irqrestore(&g_vfs_ref_lock, irq);
    return newfd;
}

int vfs_dup2(struct task *t, int oldfd, int newfd) {
    struct file_desc *f = get_fd(t, oldfd);
    if (!f) return -EBADF;
    if (newfd < 0 || newfd >= TASK_MAX_FDS) return -EBADF;
    if (oldfd == newfd) return newfd;

    if (t->fds[newfd]) {
        vfs_close(t, newfd);
    }
    t->fds[newfd] = f;
    uint64_t irq = spin_lock_irqsave(&g_vfs_ref_lock);
    f->refcount++;
    spin_unlock_irqrestore(&g_vfs_ref_lock, irq);
    return newfd;
}

int vfs_pipe(struct task *t, int pipefd[2]) {
    if (!t || !pipefd) return -EINVAL;

    struct vfs_node *rnode = NULL, *wnode = NULL;
    int rc = pipe_create_nodes(&rnode, &wnode);
    if (rc < 0) return rc;

    struct file_desc *rf = kzalloc(sizeof(*rf));
    struct file_desc *wf = kzalloc(sizeof(*wf));
    if (!rf || !wf) {
        kfree(rf);
        kfree(wf);
        vfs_node_unref(rnode);
        vfs_node_unref(wnode);
        return -ENOMEM;
    }

    rf->node = rnode; rf->flags = O_RDONLY; rf->refcount = 1;
    wf->node = wnode; wf->flags = O_WRONLY; wf->refcount = 1;

    int rfd = alloc_fd(t, rf);
    if (rfd < 0) {
        kfree(rf); kfree(wf);
        vfs_node_unref(rnode); vfs_node_unref(wnode);
        return rfd;
    }
    int wfd = alloc_fd(t, wf);
    if (wfd < 0) {
        t->fds[rfd] = NULL;
        kfree(rf); kfree(wf);
        vfs_node_unref(rnode); vfs_node_unref(wnode);
        return wfd;
    }

    pipefd[0] = rfd;
    pipefd[1] = wfd;
    return 0;
}

ssize_t vfs_read(struct task *t, int fd, void *buf, size_t len) {
    struct file_desc *f = get_fd(t, fd);
    if (!f || !f->node) return -EBADF;
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    if (f->node->type == VFS_DIR) return -EISDIR;
    if (!f->node->ops || !f->node->ops->read) return -EINVAL;

    ssize_t n = f->node->ops->read(f->node, buf, len, f->offset);
    if (n > 0 && (f->node->type == VFS_FILE || f->node->type == VFS_CHARDEV)) {
        f->offset += (size_t)n;
    }
    return n;
}

ssize_t vfs_write(struct task *t, int fd, const void *buf, size_t len) {
    struct file_desc *f = get_fd(t, fd);
    if (!f || !f->node) return -EBADF;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (f->node->type == VFS_DIR) return -EISDIR;
    if (!f->node->ops || !f->node->ops->write) return -EROFS;

    if (f->flags & O_APPEND) {
        f->offset = f->node->size;
    }
    ssize_t n = f->node->ops->write(f->node, buf, len, f->offset);
    if (n > 0 && f->node->type == VFS_FILE) {
        f->offset += (size_t)n;
    }
    return n;
}

off_t vfs_lseek(struct task *t, int fd, off_t offset, int whence) {
    struct file_desc *f = get_fd(t, fd);
    if (!f || !f->node) return -EBADF;
    if (f->node->type == VFS_CHARDEV || f->node->type == VFS_PIPE) return -ESPIPE;

    int64_t next;
    if (whence == SEEK_SET) {
        next = offset;
    } else if (whence == SEEK_CUR) {
        next = (int64_t)f->offset + offset;
    } else if (whence == SEEK_END) {
        next = (int64_t)f->node->size + offset;
    } else {
        return -EINVAL;
    }
    if (next < 0) return -EINVAL;
    f->offset = (size_t)next;
    return (off_t)f->offset;
}

int vfs_fstat(struct task *t, int fd, struct vfs_stat *st) {
    struct file_desc *f = get_fd(t, fd);
    if (!f || !f->node) return -EBADF;
    return vfs_stat_node(f->node, st);
}

int vfs_getdents(struct task *t, int fd, struct vfs_dirent *entries, size_t max_entries) {
    struct file_desc *f = get_fd(t, fd);
    if (!f || !f->node) return -EBADF;
    if (f->node->type != VFS_DIR) return -ENOTDIR;
    if (!f->node->ops || !f->node->ops->readdir) return -EINVAL;

    size_t count = 0;
    while (count < max_entries) {
        int rc = f->node->ops->readdir(f->node, f->offset, &entries[count]);
        if (rc == 0) {
            break;
        }
        if (rc < 0) {
            return (count > 0) ? (int)count : rc;
        }
        f->offset++;
        count++;
    }
    return (int)count;
}

void vfs_task_close_all(struct task *t) {
    if (!t) return;
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        if (t->fds[i]) {
            vfs_close(t, i);
        }
    }
}

void vfs_task_clone_fds(struct task *dst, struct task *src) {
    uint64_t irq = spin_lock_irqsave(&g_vfs_ref_lock);
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        if (src->fds[i]) {
            dst->fds[i] = src->fds[i];
            dst->fds[i]->refcount++;
        } else {
            dst->fds[i] = NULL;
        }
    }
    spin_unlock_irqrestore(&g_vfs_ref_lock, irq);
}

int vfs_setup_stdio(struct task *t) {
    int fd0 = vfs_open(t, "/dev/tty", O_RDONLY, 0);
    int fd1 = vfs_open(t, "/dev/tty", O_WRONLY, 0);
    int fd2 = vfs_open(t, "/dev/tty", O_WRONLY, 0);
    if (fd0 != 0 || fd1 != 1 || fd2 != 2) {
        return -EIO;
    }
    return 0;
}
