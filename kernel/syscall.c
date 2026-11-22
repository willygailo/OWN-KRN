#include <arch/gdt.h>
#include <arch/idt.h>
#include <arch/io.h>
#include <arch/pit.h>
#include <drivers/rtc.h>
#include <fs/vfs.h>
#include <kernel/elf.h>
#include <kernel/errno.h>
#include <kernel/panic.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <lib/string.h>
#include <mm/kmalloc.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <uapi/syscall.h>

#define MSR_EFER  0xC0000080U
#define MSR_STAR  0xC0000081U
#define MSR_LSTAR 0xC0000082U
#define MSR_FMASK 0xC0000084U

#define EFER_SCE  BIT(0)

extern void syscall_entry(void);
extern void fork_child_trampoline(void);
extern void idt_set_user_fault_hook(bool (*hook)(struct interrupt_frame *frame));

static bool validate_user_range(uint64_t uaddr, size_t len, bool write) {
    if (len == 0) {
        return true;
    }
    if (uaddr < PAGE_SIZE || uaddr > USER_VIRT_MAX) {
        return false;
    }
    if (uaddr + len < uaddr || (uaddr + len - 1) > USER_VIRT_MAX) {
        return false;
    }

    struct task *cur = sched_current();
    uint64_t va = ALIGN_DOWN(uaddr, PAGE_SIZE);
    uint64_t end = uaddr + len;
    while (va < end) {
        uint64_t flags = 0;
        if (!vmm_virt_to_phys(cur->cr3, va, NULL, &flags)) {
            return false;
        }
        if (!(flags & PTE_PRESENT) || !(flags & PTE_USER)) {
            return false;
        }
        if (write && !(flags & PTE_WRITABLE)) {
            return false;
        }
        va += PAGE_SIZE;
    }
    return true;
}

int copy_from_user(void *kdst, uint64_t usrc, size_t len) {
    if (!validate_user_range(usrc, len, false)) {
        return -EFAULT;
    }
    if (len > 0) {
        memcpy(kdst, (const void *)(uintptr_t)usrc, len);
    }
    return 0;
}

int copy_to_user(uint64_t udst, const void *ksrc, size_t len) {
    if (!validate_user_range(udst, len, true)) {
        return -EFAULT;
    }
    if (len > 0) {
        memcpy((void *)(uintptr_t)udst, ksrc, len);
    }
    return 0;
}

int copy_str_from_user(char *kdst, uint64_t usrc, size_t max_len) {
    if (max_len == 0) return -EINVAL;
    for (size_t i = 0; i < max_len; i++) {
        char c = 0;
        int rc = copy_from_user(&c, usrc + i, 1);
        if (rc < 0) return rc;
        kdst[i] = c;
        if (c == '\0') {
            return 0;
        }
    }
    kdst[max_len - 1] = '\0';
    return -ENAMETOOLONG;
}

static bool handle_user_fault(struct interrupt_frame *f) {
    struct task *cur = sched_current();
    pr_warn("User process pid=%d (%s) killed by CPU exception vec=%llu err=0x%llx RIP=0x%llx CR2=0x%llx\n",
            cur->pid, cur->name,
            (unsigned long long)f->int_no,
            (unsigned long long)f->err_code,
            (unsigned long long)f->rip,
            (unsigned long long)read_cr2());
    task_exit((int)(128 + f->int_no));
    return true;
}

void syscall_init(void) {
    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | EFER_SCE);

    uint64_t star = ((uint64_t)GDT_KERNEL_DATA << 48) |
                    ((uint64_t)GDT_KERNEL_CODE << 32);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    wrmsr(MSR_FMASK, 0x700ULL);

    idt_set_user_fault_hook(handle_user_fault);
    pr_ok("SYSCALL/SYSRET MSRs configured (LSTAR=0x%016llx, STAR=0x%016llx)\n",
          (unsigned long long)(uintptr_t)syscall_entry,
          (unsigned long long)star);
}

static int64_t sys_read(int fd, uint64_t ubuf, size_t len) {
    if (len == 0) return 0;
    if (len > 65536) len = 65536;
    if (!validate_user_range(ubuf, len, true)) return -EFAULT;

    void *kbuf = kmalloc(len);
    if (!kbuf) return -ENOMEM;

    ssize_t n = vfs_read(sched_current(), fd, kbuf, len);
    if (n > 0) {
        if (copy_to_user(ubuf, kbuf, (size_t)n) < 0) {
            kfree(kbuf);
            return -EFAULT;
        }
    }
    kfree(kbuf);
    return n;
}

static int64_t sys_write(int fd, uint64_t ubuf, size_t len) {
    if (len == 0) return 0;
    if (len > 65536) len = 65536;
    if (!validate_user_range(ubuf, len, false)) return -EFAULT;

    void *kbuf = kmalloc(len);
    if (!kbuf) return -ENOMEM;
    if (copy_from_user(kbuf, ubuf, len) < 0) {
        kfree(kbuf);
        return -EFAULT;
    }

    ssize_t n = vfs_write(sched_current(), fd, kbuf, len);
    kfree(kbuf);
    return n;
}

static int64_t sys_open(uint64_t upath, uint32_t flags, uint32_t mode) {
    char path[VFS_PATH_MAX];
    int rc = copy_str_from_user(path, upath, sizeof(path));
    if (rc < 0) return rc;
    return vfs_open(sched_current(), path, flags, mode);
}

static int64_t sys_pipe(uint64_t upipefd) {
    int fds[2];
    int rc = vfs_pipe(sched_current(), fds);
    if (rc < 0) return rc;
    rc = copy_to_user(upipefd, fds, sizeof(fds));
    if (rc < 0) {
        vfs_close(sched_current(), fds[0]);
        vfs_close(sched_current(), fds[1]);
        return rc;
    }
    return 0;
}

static int64_t sys_stat(uint64_t upath, uint64_t ustat) {
    char path[VFS_PATH_MAX];
    int rc = copy_str_from_user(path, upath, sizeof(path));
    if (rc < 0) return rc;

    struct vfs_stat st;
    rc = vfs_stat_path(sched_current()->cwd, path, &st);
    if (rc < 0) return rc;

    return copy_to_user(ustat, &st, sizeof(st));
}

static int64_t sys_fstat(int fd, uint64_t ustat) {
    struct vfs_stat st;
    int rc = vfs_fstat(sched_current(), fd, &st);
    if (rc < 0) return rc;
    return copy_to_user(ustat, &st, sizeof(st));
}

static int64_t sys_brk(uint64_t new_brk) {
    struct task *cur = sched_current();
    if (new_brk == 0) {
        return (int64_t)cur->user_brk_curr;
    }
    if (new_brk < cur->user_brk_start ||
        new_brk >= USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE - PAGE_SIZE) {
        return (int64_t)cur->user_brk_curr;
    }

    uint64_t old_top = ALIGN_UP(cur->user_brk_curr, PAGE_SIZE);
    uint64_t new_top = ALIGN_UP(new_brk, PAGE_SIZE);

    if (new_top > old_top) {
        for (uint64_t va = old_top; va < new_top; va += PAGE_SIZE) {
            uint64_t pa = pmm_alloc_frame();
            if (!pa || !vmm_map_page(cur->cr3, va, pa,
                                     PTE_PRESENT | PTE_USER | PTE_WRITABLE | PTE_NX)) {
                if (pa) pmm_free_frame(pa);
                return (int64_t)cur->user_brk_curr;
            }
        }
    } else if (new_top < old_top) {
        for (uint64_t va = new_top; va < old_top; va += PAGE_SIZE) {
            uint64_t pa = 0;
            if (vmm_unmap_page(cur->cr3, va, &pa)) {
                pmm_free_frame(pa);
            }
        }
    }

    cur->user_brk_curr = new_brk;
    return (int64_t)cur->user_brk_curr;
}

static int64_t sys_fork(struct interrupt_frame *parent_frame) {
    struct task *parent = sched_current();
    uint64_t child_cr3 = vmm_clone_address_space(parent->cr3);
    if (!child_cr3) {
        return -ENOMEM;
    }

    struct task *child = sched_alloc_task(parent->name, parent->pid, child_cr3, true);
    if (!child) {
        vmm_destroy_address_space(child_cr3);
        return -ENOMEM;
    }

    child->user_brk_start = parent->user_brk_start;
    child->user_brk_curr  = parent->user_brk_curr;
    strcpy(child->cwd, parent->cwd);
    vfs_task_clone_fds(child, parent);

    uint8_t *cframe_dst = (uint8_t *)(uintptr_t)(child->kstack_top - sizeof(struct interrupt_frame));
    memcpy(cframe_dst, parent_frame, sizeof(struct interrupt_frame));
    struct interrupt_frame *cf = (struct interrupt_frame *)cframe_dst;
    cf->rax = 0;

    uint64_t *sp = (uint64_t *)cframe_dst;
    *--sp = (uint64_t)(uintptr_t)fork_child_trampoline;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    *--sp = 0;
    child->rsp = (uint64_t)(uintptr_t)sp;

    sched_make_ready(child);
    return (int64_t)child->pid;
}

static int64_t sys_execve(struct interrupt_frame *frame, uint64_t upath, uint64_t uargv) {
    char path[VFS_PATH_MAX];
    int rc = copy_str_from_user(path, upath, sizeof(path));
    if (rc < 0) return rc;

    char *kargv[32];
    int argc = 0;
    if (uargv != 0) {
        while (argc < 31) {
            uint64_t arg_ptr = 0;
            rc = copy_from_user(&arg_ptr, uargv + (uint64_t)argc * sizeof(uint64_t),
                                sizeof(uint64_t));
            if (rc < 0) goto cleanup_argv;
            if (arg_ptr == 0) break;

            char *s = kmalloc(256);
            if (!s) {
                rc = -ENOMEM;
                goto cleanup_argv;
            }
            rc = copy_str_from_user(s, arg_ptr, 256);
            if (rc < 0) {
                kfree(s);
                goto cleanup_argv;
            }
            kargv[argc++] = s;
        }
    }
    if (argc == 0) {
        char *s = kmalloc(VFS_PATH_MAX);
        if (!s) return -ENOMEM;
        strcpy(s, path);
        kargv[argc++] = s;
    }

    struct task *cur = sched_current();
    struct vfs_node *node = NULL;
    rc = vfs_resolve(cur->cwd, path, &node);
    if (rc < 0) goto cleanup_argv;
    if (node->type != VFS_FILE || node->size == 0) {
        rc = -ENOEXEC;
        goto cleanup_argv;
    }

    uint8_t *img = kmalloc(node->size);
    if (!img) {
        rc = -ENOMEM;
        goto cleanup_argv;
    }
    ssize_t rd = node->ops->read(node, img, node->size, 0);
    if (rd != (ssize_t)node->size) {
        kfree(img);
        rc = -EIO;
        goto cleanup_argv;
    }

    struct elf_load_result res;
    rc = elf_load_into_aspace(img, node->size, (const char *const *)kargv, argc, &res);
    kfree(img);
    if (rc < 0) goto cleanup_argv;

    uint64_t old_cr3 = cur->cr3;
    irq_disable();
    cur->cr3            = res.cr3;
    cur->user_brk_start = res.brk_start;
    cur->user_brk_curr  = res.brk_start;
    vmm_switch(res.cr3);
    irq_enable();

    const char *base = strrchr(path, '/');
    strncpy(cur->name, base ? (base + 1) : path, sizeof(cur->name) - 1);
    cur->name[sizeof(cur->name) - 1] = '\0';

    if (old_cr3 && old_cr3 != vmm_kernel_pml4()) {
        vmm_destroy_address_space(old_cr3);
    }

    memset(frame, 0, sizeof(*frame));
    frame->rip    = res.entry;
    frame->rsp    = res.user_rsp;
    frame->cs     = USER_CS;
    frame->ss     = USER_DS;
    frame->rflags = 0x202ULL;

    for (int i = 0; i < argc; i++) kfree(kargv[i]);
    return 0;

cleanup_argv:
    for (int i = 0; i < argc; i++) kfree(kargv[i]);
    return rc;
}

static int64_t sys_wait4(pid_t pid, uint64_t ustatus, int options) {
    int st = 0;
    int r = sched_waitpid(pid, &st, options);
    if (r <= 0) return r;
    if (ustatus != 0) {
        int packed = (st & 0xFF) << 8;
        int rc = copy_to_user(ustatus, &packed, sizeof(packed));
        if (rc < 0) return rc;
    }
    return r;
}

static int64_t sys_uname(uint64_t ubuf) {
    struct utsname u;
    memset(&u, 0, sizeof(u));
    strcpy(u.sysname,  "OwnKrn");
    strcpy(u.nodename, "ownkrn");
    strcpy(u.release,  "0.2.0");
    strcpy(u.version,  "#1 SMP PREEMPT x86_64");
    strcpy(u.machine,  "x86_64");
    return copy_to_user(ubuf, &u, sizeof(u));
}

static int64_t sys_getdents(int fd, uint64_t ubuf, size_t max_entries) {
    if (max_entries == 0) return 0;
    if (max_entries > 64) max_entries = 64;
    size_t bytes = max_entries * sizeof(struct vfs_dirent);
    if (!validate_user_range(ubuf, bytes, true)) return -EFAULT;

    struct vfs_dirent *kents = kmalloc(bytes);
    if (!kents) return -ENOMEM;

    int count = vfs_getdents(sched_current(), fd, kents, max_entries);
    if (count > 0) {
        if (copy_to_user(ubuf, kents, (size_t)count * sizeof(struct vfs_dirent)) < 0) {
            kfree(kents);
            return -EFAULT;
        }
    }
    kfree(kents);
    return count;
}

static int64_t sys_getcwd(uint64_t ubuf, size_t size) {
    struct task *cur = sched_current();
    size_t len = strlen(cur->cwd) + 1;
    if (size < len) return -EINVAL;
    int rc = copy_to_user(ubuf, cur->cwd, len);
    return (rc < 0) ? rc : (int64_t)len;
}

static int64_t sys_chdir(uint64_t upath) {
    char path[VFS_PATH_MAX];
    int rc = copy_str_from_user(path, upath, sizeof(path));
    if (rc < 0) return rc;

    struct task *cur = sched_current();
    char norm[VFS_PATH_MAX];
    rc = vfs_normalize_path(cur->cwd, path, norm, sizeof(norm));
    if (rc < 0) return rc;

    struct vfs_node *node = NULL;
    rc = vfs_resolve("/", norm, &node);
    if (rc < 0) return rc;
    if (node->type != VFS_DIR) return -ENOTDIR;

    strcpy(cur->cwd, norm);
    return 0;
}

static int64_t sys_mkdir(uint64_t upath, uint32_t mode) {
    char path[VFS_PATH_MAX];
    int rc = copy_str_from_user(path, upath, sizeof(path));
    if (rc < 0) return rc;
    return vfs_mkdir(sched_current()->cwd, path, mode);
}

static int64_t sys_unlink(uint64_t upath) {
    char path[VFS_PATH_MAX];
    int rc = copy_str_from_user(path, upath, sizeof(path));
    if (rc < 0) return rc;
    return vfs_unlink(sched_current()->cwd, path);
}

static int64_t sys_nanosleep(uint64_t utsp) {
    struct timespec ts;
    int rc = copy_from_user(&ts, utsp, sizeof(ts));
    if (rc < 0) return rc;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL) {
        return -EINVAL;
    }
    uint64_t ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000LL);
    if (ms > 0) {
        sleep_ms(ms);
    }
    return 0;
}

static int64_t sys_clock_gettime(int clk_id, uint64_t utsp) {
    struct timespec ts;
    if (clk_id == CLOCK_REALTIME) {
        ts.tv_sec  = rtc_unix_seconds();
        ts.tv_nsec = 0;
    } else if (clk_id == CLOCK_MONOTONIC) {
        uint64_t ms = pit_uptime_ms();
        ts.tv_sec  = (int64_t)(ms / 1000ULL);
        ts.tv_nsec = (int64_t)((ms % 1000ULL) * 1000000ULL);
    } else {
        return -EINVAL;
    }
    return copy_to_user(utsp, &ts, sizeof(ts));
}

int64_t syscall_dispatch(struct interrupt_frame *f) {
    uint64_t nr = f->rax;
    uint64_t a1 = f->rdi;
    uint64_t a2 = f->rsi;
    uint64_t a3 = f->rdx;

    int64_t ret;
    switch (nr) {
        case SYS_read:          ret = sys_read((int)a1, a2, (size_t)a3); break;
        case SYS_write:         ret = sys_write((int)a1, a2, (size_t)a3); break;
        case SYS_open:          ret = sys_open(a1, (uint32_t)a2, (uint32_t)a3); break;
        case SYS_close:         ret = vfs_close(sched_current(), (int)a1); break;
        case SYS_stat:          ret = sys_stat(a1, a2); break;
        case SYS_fstat:         ret = sys_fstat((int)a1, a2); break;
        case SYS_lseek:         ret = vfs_lseek(sched_current(), (int)a1, (off_t)a2, (int)a3); break;
        case SYS_brk:           ret = sys_brk(a1); break;
        case SYS_pipe:          ret = sys_pipe(a1); break;
        case SYS_sched_yield:   sched_yield(); ret = 0; break;
        case SYS_dup:           ret = vfs_dup(sched_current(), (int)a1); break;
        case SYS_dup2:          ret = vfs_dup2(sched_current(), (int)a1, (int)a2); break;
        case SYS_nanosleep:     ret = sys_nanosleep(a1); break;
        case SYS_getpid:        ret = (int64_t)sched_current()->pid; break;
        case SYS_getppid:       ret = (int64_t)sched_current()->ppid; break;
        case SYS_fork:          ret = sys_fork(f); break;
        case SYS_execve:        ret = sys_execve(f, a1, a2); break;
        case SYS_exit:          task_exit((int)a1);
        case SYS_wait4:         ret = sys_wait4((pid_t)a1, a2, (int)a3); break;
        case SYS_kill:          ret = sched_send_signal((pid_t)a1, (int)a2); break;
        case SYS_uname:         ret = sys_uname(a1); break;
        case SYS_getdents:      ret = sys_getdents((int)a1, a2, (size_t)a3); break;
        case SYS_getcwd:        ret = sys_getcwd(a1, (size_t)a2); break;
        case SYS_chdir:         ret = sys_chdir(a1); break;
        case SYS_mkdir:         ret = sys_mkdir(a1, (uint32_t)a2); break;
        case SYS_unlink:        ret = sys_unlink(a1); break;
        case SYS_clock_gettime: ret = sys_clock_gettime((int)a1, a2); break;
        case SYS_reboot:
            if (a1 == OWNKRN_CMD_REBOOT) {
                arch_reboot();
            }
            arch_poweroff();
            ret = 0;
            break;
        default:
            ret = -ENOSYS;
            break;
    }

    sched_check_signals();
    return ret;
}
