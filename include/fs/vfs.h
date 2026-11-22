#ifndef OWNKRN_FS_VFS_H
#define OWNKRN_FS_VFS_H

#include <kernel/sched.h>
#include <kernel/types.h>

#define VFS_NAME_MAX 64
#define VFS_PATH_MAX 256

/* Open flags (matching Linux values) */
#define O_RDONLY   0x0000U
#define O_WRONLY   0x0001U
#define O_RDWR     0x0002U
#define O_ACCMODE  0x0003U
#define O_CREAT    0x0040U
#define O_TRUNC    0x0200U
#define O_APPEND   0x0400U

/* lseek whence */
#define SEEK_SET   0
#define SEEK_CUR   1
#define SEEK_END   2

enum vfs_type {
    VFS_FILE    = 1,
    VFS_DIR     = 2,
    VFS_CHARDEV = 3,
};

struct vfs_node;

struct vfs_dirent {
    uint64_t ino;
    uint32_t type;
    char     name[VFS_NAME_MAX];
};

struct vfs_stat {
    uint64_t ino;
    uint32_t type;
    uint32_t mode;
    uint64_t size;
};

struct vfs_ops {
    ssize_t (*read)(struct vfs_node *node, void *buf, size_t len, size_t offset);
    ssize_t (*write)(struct vfs_node *node, const void *buf, size_t len, size_t offset);
    int     (*lookup)(struct vfs_node *dir, const char *name, struct vfs_node **out);
    int     (*readdir)(struct vfs_node *dir, size_t index, struct vfs_dirent *out);
    int     (*create)(struct vfs_node *dir, const char *name, enum vfs_type type,
                      uint32_t mode, struct vfs_node **out);
    int     (*unlink)(struct vfs_node *dir, const char *name);
    int     (*truncate)(struct vfs_node *node, size_t new_size);
};

struct vfs_node {
    uint64_t              ino;
    enum vfs_type         type;
    uint32_t              mode;
    size_t                size;
    uint32_t              refcount;
    const struct vfs_ops *ops;
    void                 *priv;
    struct vfs_node      *parent;
    struct vfs_node      *mount_root;
};

struct file_desc {
    struct vfs_node *node;
    size_t           offset;
    uint32_t         flags;
    uint32_t         refcount;
};

/* Core VFS API */
void             vfs_init(void);
struct vfs_node *vfs_root(void);
int              vfs_mount(const char *path, struct vfs_node *fs_root);
int              vfs_normalize_path(const char *cwd, const char *path,
                                    char *out, size_t out_sz);
int              vfs_resolve(const char *cwd, const char *path, struct vfs_node **out);
int              vfs_resolve_parent(const char *cwd, const char *path,
                                    struct vfs_node **dir_out, char *leaf_out);

int              vfs_mkdir(const char *cwd, const char *path, uint32_t mode);
int              vfs_unlink(const char *cwd, const char *path);
int              vfs_stat_path(const char *cwd, const char *path, struct vfs_stat *st);
int              vfs_stat_node(struct vfs_node *node, struct vfs_stat *st);

/* File-descriptor table operations */
int              vfs_open(struct task *t, const char *path, uint32_t flags, uint32_t mode);
int              vfs_close(struct task *t, int fd);
ssize_t          vfs_read(struct task *t, int fd, void *buf, size_t len);
ssize_t          vfs_write(struct task *t, int fd, const void *buf, size_t len);
off_t            vfs_lseek(struct task *t, int fd, off_t offset, int whence);
int              vfs_fstat(struct task *t, int fd, struct vfs_stat *st);
int              vfs_getdents(struct task *t, int fd, struct vfs_dirent *entries, size_t max_entries);
void             vfs_task_close_all(struct task *t);
void             vfs_task_clone_fds(struct task *dst, struct task *src);
int              vfs_setup_stdio(struct task *t);

/* Filesystem constructors */
struct vfs_node *tmpfs_create_root(void);
struct vfs_node *devfs_create_root(void);
struct vfs_node *procfs_create_root(void);
int              initrd_unpack(uint64_t phys_addr, uint64_t size);

#endif /* OWNKRN_FS_VFS_H */
