#include <fs/vfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/types.h>
#include <lib/string.h>

/* POSIX ustar 512-byte header */
struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} PACKED;

static uint64_t parse_octal(const char *s, size_t len) {
    uint64_t val = 0;
    for (size_t i = 0; i < len && s[i] != '\0'; i++) {
        if (s[i] >= '0' && s[i] <= '7') {
            val = (val << 3) | (uint64_t)(s[i] - '0');
        }
    }
    return val;
}

static void ensure_parent_dirs(const char *abs_path) {
    char tmp[VFS_PATH_MAX];
    strncpy(tmp, abs_path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            (void)vfs_mkdir("/", tmp, 0755);
            *p = '/';
        }
    }
}

int initrd_unpack(uint64_t phys_addr, uint64_t size) {
    if (!phys_addr || size < 512) {
        return 0;
    }

    const uint8_t *base = (const uint8_t *)PHYS_TO_VIRT(phys_addr);
    uint64_t off = 0;
    int unpacked_files = 0;

    while (off + 512 <= size) {
        const struct tar_header *hdr = (const struct tar_header *)(base + off);
        if (hdr->name[0] == '\0') {
            break; /* End-of-archive zero block */
        }

        uint64_t fsize = parse_octal(hdr->size, sizeof(hdr->size));
        uint32_t fmode = (uint32_t)parse_octal(hdr->mode, sizeof(hdr->mode));

        /* Build absolute path (strip leading "./" if present) */
        const char *raw = hdr->name;
        if (raw[0] == '.' && raw[1] == '/') {
            raw += 2;
        }
        if (raw[0] == '\0' || (raw[0] == '.' && raw[1] == '\0')) {
            off += 512 + ALIGN_UP(fsize, 512);
            continue;
        }

        char abs_path[VFS_PATH_MAX];
        if (raw[0] == '/') {
            strncpy(abs_path, raw, sizeof(abs_path) - 1);
        } else {
            abs_path[0] = '/';
            strncpy(abs_path + 1, raw, sizeof(abs_path) - 2);
        }
        abs_path[sizeof(abs_path) - 1] = '\0';

        /* Strip trailing slash */
        size_t plen = strlen(abs_path);
        while (plen > 1 && abs_path[plen - 1] == '/') {
            abs_path[--plen] = '\0';
        }

        if (hdr->typeflag == '5') {
            ensure_parent_dirs(abs_path);
            (void)vfs_mkdir("/", abs_path, fmode ? fmode : 0755);
        } else if (hdr->typeflag == '0' || hdr->typeflag == '\0') {
            ensure_parent_dirs(abs_path);
            struct vfs_node *parent = NULL;
            char leaf[VFS_NAME_MAX];
            if (vfs_resolve_parent("/", abs_path, &parent, leaf) == 0 &&
                parent->ops && parent->ops->create) {
                struct vfs_node *node = NULL;
                int rc = parent->ops->lookup(parent, leaf, &node);
                if (rc == -ENOENT) {
                    rc = parent->ops->create(parent, leaf, VFS_FILE,
                                             fmode ? fmode : 0644, &node);
                }
                if (rc == 0 && node && node->ops && node->ops->write && fsize > 0) {
                    if (off + 512 + fsize <= size) {
                        node->ops->write(node, base + off + 512, (size_t)fsize, 0);
                    }
                }
                if (rc == 0) {
                    unpacked_files++;
                }
            }
        }

        off += 512 + ALIGN_UP(fsize, 512);
    }

    pr_ok("Initrd unpacked: %d files from %llu-byte ustar archive\n",
          unpacked_files, (unsigned long long)size);
    return unpacked_files;
}
