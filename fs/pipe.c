#include <arch/io.h>
#include <fs/vfs.h>
#include <kernel/errno.h>
#include <kernel/sched.h>
#include <kernel/sync.h>
#include <mm/kmalloc.h>

#define PIPE_BUF_SIZE 4096

struct pipe_buf {
    uint8_t    data[PIPE_BUF_SIZE];
    size_t     rpos;
    size_t     wpos;
    size_t     count;
    uint32_t   readers;
    uint32_t   writers;
    spinlock_t lock;
};

static uint64_t g_pipe_ino = 50000;

static ssize_t pipe_read(struct vfs_node *node, void *buf, size_t len, size_t offset) {
    (void)offset;
    if (len == 0) return 0;

    struct pipe_buf *p = (struct pipe_buf *)node->priv;
    uint8_t *dst = (uint8_t *)buf;

    for (;;) {
        struct task *cur = sched_current();
        if (cur && cur->pending_sigs != 0) {
            return -EINTR;
        }

        uint64_t irq = spin_lock_irqsave(&p->lock);
        if (p->count > 0) {
            size_t n = 0;
            while (n < len && p->count > 0) {
                dst[n++] = p->data[p->rpos];
                p->rpos = (p->rpos + 1) % PIPE_BUF_SIZE;
                p->count--;
            }
            spin_unlock_irqrestore(&p->lock, irq);
            return (ssize_t)n;
        }
        if (p->writers == 0) {
            spin_unlock_irqrestore(&p->lock, irq);
            return 0; /* Clean EOF: all writers closed */
        }
        spin_unlock_irqrestore(&p->lock, irq);

        irq_enable();
        sched_yield();
    }
}

static ssize_t pipe_write(struct vfs_node *node, const void *buf, size_t len, size_t offset) {
    (void)offset;
    if (len == 0) return 0;

    struct pipe_buf *p = (struct pipe_buf *)node->priv;
    const uint8_t *src = (const uint8_t *)buf;
    size_t written = 0;

    while (written < len) {
        struct task *cur = sched_current();
        if (cur && cur->pending_sigs != 0) {
            return (written > 0) ? (ssize_t)written : -EINTR;
        }

        uint64_t irq = spin_lock_irqsave(&p->lock);
        if (p->readers == 0) {
            spin_unlock_irqrestore(&p->lock, irq);
            if (cur) {
                cur->pending_sigs |= (1U << SIGPIPE);
            }
            return (written > 0) ? (ssize_t)written : -EPIPE;
        }
        while (written < len && p->count < PIPE_BUF_SIZE) {
            p->data[p->wpos] = src[written++];
            p->wpos = (p->wpos + 1) % PIPE_BUF_SIZE;
            p->count++;
        }
        spin_unlock_irqrestore(&p->lock, irq);

        if (written < len) {
            irq_enable();
            sched_yield();
        }
    }
    return (ssize_t)written;
}

static void pipe_read_release(struct vfs_node *node) {
    struct pipe_buf *p = (struct pipe_buf *)node->priv;
    bool free_pipe = false;
    uint64_t irq = spin_lock_irqsave(&p->lock);
    if (p->readers > 0) p->readers--;
    if (p->readers == 0 && p->writers == 0) free_pipe = true;
    spin_unlock_irqrestore(&p->lock, irq);

    if (free_pipe) kfree(p);
    kfree(node);
}

static void pipe_write_release(struct vfs_node *node) {
    struct pipe_buf *p = (struct pipe_buf *)node->priv;
    bool free_pipe = false;
    uint64_t irq = spin_lock_irqsave(&p->lock);
    if (p->writers > 0) p->writers--;
    if (p->readers == 0 && p->writers == 0) free_pipe = true;
    spin_unlock_irqrestore(&p->lock, irq);

    if (free_pipe) kfree(p);
    kfree(node);
}

static const struct vfs_ops pipe_read_ops = {
    .read    = pipe_read,
    .release = pipe_read_release,
};

static const struct vfs_ops pipe_write_ops = {
    .write   = pipe_write,
    .release = pipe_write_release,
};

int pipe_create_nodes(struct vfs_node **rnode_out, struct vfs_node **wnode_out) {
    struct pipe_buf *p = kzalloc(sizeof(*p));
    struct vfs_node *rn = kzalloc(sizeof(*rn));
    struct vfs_node *wn = kzalloc(sizeof(*wn));
    if (!p || !rn || !wn) {
        kfree(p);
        kfree(rn);
        kfree(wn);
        return -ENOMEM;
    }

    spinlock_init(&p->lock);
    p->readers = 1;
    p->writers = 1;

    uint64_t ino = __sync_fetch_and_add(&g_pipe_ino, 2);
    rn->ino = ino;     rn->type = VFS_PIPE; rn->mode = 0444; rn->refcount = 1; rn->ops = &pipe_read_ops;  rn->priv = p;
    wn->ino = ino + 1; wn->type = VFS_PIPE; wn->mode = 0222; wn->refcount = 1; wn->ops = &pipe_write_ops; wn->priv = p;

    *rnode_out = rn;
    *wnode_out = wn;
    return 0;
}
