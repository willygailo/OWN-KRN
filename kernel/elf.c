#include <arch/gdt.h>
#include <arch/idt.h>
#include <arch/io.h>
#include <fs/vfs.h>
#include <kernel/elf.h>
#include <kernel/errno.h>
#include <kernel/panic.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <lib/string.h>
#include <mm/kmalloc.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

extern void enter_usermode(uint64_t user_rip, uint64_t user_rsp, uint64_t user_rflags);

#define EI_NIDENT  16
#define ET_EXEC    2
#define EM_X86_64  62
#define PT_LOAD    1
#define PF_X       0x1U
#define PF_W       0x2U
#define PF_R       0x4U

struct elf64_ehdr {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} PACKED;

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} PACKED;

static int map_and_populate_stack(uint64_t cr3, const char *const *argv, int argc,
                                  uint64_t *out_rsp) {
    uint64_t stack_base = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;

    for (uint64_t i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t pa = pmm_alloc_frame();
        if (!pa) return -ENOMEM;
        if (!vmm_map_page(cr3, stack_base + i * PAGE_SIZE, pa,
                          PTE_PRESENT | PTE_USER | PTE_WRITABLE | PTE_NX)) {
            pmm_free_frame(pa);
            return -ENOMEM;
        }
    }

    /* Helper to write into user stack via HHDM */
    uint64_t sp = USER_STACK_TOP;
    uint64_t argv_uaddrs[32] = {0};
    if (argc < 0) argc = 0;
    if (argc > 31) argc = 31;

    /* Copy argument strings onto the top of the user stack */
    for (int i = argc - 1; i >= 0; i--) {
        const char *s = argv[i] ? argv[i] : "";
        size_t len = strlen(s) + 1;
        sp -= len;
        uint64_t cur_va = sp;
        size_t rem = len;
        size_t src_off = 0;
        while (rem > 0) {
            uint64_t pa = 0;
            if (!vmm_virt_to_phys(cr3, cur_va, &pa, NULL)) return -EFAULT;
            size_t page_rem = PAGE_SIZE - (cur_va & (PAGE_SIZE - 1));
            size_t chunk = (rem < page_rem) ? rem : page_rem;
            memcpy(PHYS_TO_VIRT(pa), s + src_off, chunk);
            cur_va += chunk;
            src_off += chunk;
            rem -= chunk;
        }
        argv_uaddrs[i] = sp;
    }

    /* Align stack so that after pushing argc + argv[0..argc] + envp(NULL),
     * RSP is 16-byte aligned at _start per System V AMD64 ABI. */
    sp = ALIGN_DOWN(sp, 16);
    size_t qwords = (size_t)(1 + argc + 1 + 1); /* argc + argv[] + NULL + envp NULL */
    if (qwords & 1) {
        sp -= 8; /* Extra padding to guarantee 16-byte alignment */
    }

    uint64_t frame_vals[40];
    size_t idx = 0;
    frame_vals[idx++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) {
        frame_vals[idx++] = argv_uaddrs[i];
    }
    frame_vals[idx++] = 0; /* argv[argc] = NULL */
    frame_vals[idx++] = 0; /* envp[0] = NULL */

    sp -= idx * sizeof(uint64_t);
    uint64_t cur_va = sp;
    size_t total = idx * sizeof(uint64_t);
    const uint8_t *src = (const uint8_t *)frame_vals;
    size_t off = 0;
    while (total > 0) {
        uint64_t pa = 0;
        if (!vmm_virt_to_phys(cr3, cur_va, &pa, NULL)) return -EFAULT;
        size_t page_rem = PAGE_SIZE - (cur_va & (PAGE_SIZE - 1));
        size_t chunk = (total < page_rem) ? total : page_rem;
        memcpy(PHYS_TO_VIRT(pa), src + off, chunk);
        cur_va += chunk;
        off += chunk;
        total -= chunk;
    }

    *out_rsp = sp;
    return 0;
}

int elf_load_into_aspace(const uint8_t *image, size_t size,
                         const char *const *argv, int argc,
                         struct elf_load_result *out) {
    if (!image || size < sizeof(struct elf64_ehdr)) {
        return -ENOEXEC;
    }

    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)image;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') {
        return -ENOEXEC;
    }
    if (eh->e_ident[4] != 2 || eh->e_ident[5] != 1 ||
        eh->e_type != ET_EXEC || eh->e_machine != EM_X86_64) {
        return -ENOEXEC;
    }
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > size) {
        return -ENOEXEC;
    }
    if (eh->e_entry < PAGE_SIZE || eh->e_entry > USER_VIRT_MAX) {
        return -ENOEXEC;
    }

    uint64_t cr3 = vmm_create_address_space();
    if (!cr3) return -ENOMEM;

    uint64_t max_end = 0x400000ULL;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(image + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        if (ph->p_filesz > ph->p_memsz || ph->p_offset + ph->p_filesz > size) {
            vmm_destroy_address_space(cr3);
            return -ENOEXEC;
        }
        if (ph->p_vaddr < PAGE_SIZE ||
            ph->p_vaddr + ph->p_memsz > USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE) {
            vmm_destroy_address_space(cr3);
            return -ENOEXEC;
        }

        uint64_t seg_flags = PTE_PRESENT | PTE_USER;
        if (ph->p_flags & PF_W) seg_flags |= PTE_WRITABLE;
        if (!(ph->p_flags & PF_X)) seg_flags |= PTE_NX;

        uint64_t va_start = ALIGN_DOWN(ph->p_vaddr, PAGE_SIZE);
        uint64_t va_end   = ALIGN_UP(ph->p_vaddr + ph->p_memsz, PAGE_SIZE);
        if (va_end > max_end) {
            max_end = va_end;
        }

        for (uint64_t va = va_start; va < va_end; va += PAGE_SIZE) {
            uint64_t existing_pa = 0, existing_flags = 0;
            uint64_t pa = 0;
            if (vmm_virt_to_phys(cr3, va, &existing_pa, &existing_flags)) {
                pa = existing_pa & PTE_ADDR_MASK;
                /* Combine permissions when two PT_LOAD segments share a page */
                uint64_t merged = existing_flags | (seg_flags & PTE_WRITABLE);
                if (!(seg_flags & PTE_NX) || !(existing_flags & PTE_NX)) {
                    merged &= ~PTE_NX;
                }
                vmm_map_page(cr3, va, pa, merged);
            } else {
                pa = pmm_alloc_frame();
                if (!pa || !vmm_map_page(cr3, va, pa, seg_flags)) {
                    if (pa) pmm_free_frame(pa);
                    vmm_destroy_address_space(cr3);
                    return -ENOMEM;
                }
            }

            /* Copy overlapping bytes from the ELF file into this page */
            uint64_t file_vstart = ph->p_vaddr;
            uint64_t file_vend   = ph->p_vaddr + ph->p_filesz;
            uint64_t copy_s = (va > file_vstart) ? va : file_vstart;
            uint64_t copy_e = (va + PAGE_SIZE < file_vend) ? (va + PAGE_SIZE) : file_vend;
            if (copy_e > copy_s) {
                uint64_t dst_off = copy_s - va;
                uint64_t src_off = ph->p_offset + (copy_s - ph->p_vaddr);
                memcpy((uint8_t *)PHYS_TO_VIRT(pa) + dst_off,
                       image + src_off, (size_t)(copy_e - copy_s));
            }
        }
    }

    uint64_t user_rsp = 0;
    int rc = map_and_populate_stack(cr3, argv, argc, &user_rsp);
    if (rc < 0) {
        vmm_destroy_address_space(cr3);
        return rc;
    }

    out->entry     = eh->e_entry;
    out->user_rsp  = user_rsp;
    out->brk_start = ALIGN_UP(max_end, PAGE_SIZE);
    out->cr3       = cr3;
    return 0;
}

struct init_launch_args {
    uint64_t entry;
    uint64_t user_rsp;
    uint64_t cr3;
    uint64_t brk_start;
};

static void user_init_trampoline(void *arg) {
    struct init_launch_args *a = (struct init_launch_args *)arg;
    uint64_t entry     = a->entry;
    uint64_t user_rsp  = a->user_rsp;
    uint64_t cr3       = a->cr3;
    uint64_t brk_start = a->brk_start;
    kfree(a);

    struct task *cur = sched_current();
    cur->is_user        = true;
    cur->cr3            = cr3;
    cur->user_brk_start = brk_start;
    cur->user_brk_curr  = brk_start;

    KASSERT(vfs_setup_stdio(cur) == 0);

    irq_disable();
    vmm_switch(cr3);
    tss_set_rsp0(cur->kstack_top);
    arch_cpu_local()->kernel_rsp = cur->kstack_top;

    /* Drop permanently to Ring 3 */
    enter_usermode(entry, user_rsp, 0x202ULL);
}

pid_t elf_spawn_user_process(const char *path, const char *const *argv, int argc) {
    struct vfs_node *node = NULL;
    int rc = vfs_resolve("/", path, &node);
    if (rc < 0 || node->type != VFS_FILE || node->size == 0) {
        pr_err("elf_spawn: cannot find executable '%s' (rc=%d)\n", path, rc);
        return -ENOENT;
    }

    uint8_t *buf = kmalloc(node->size);
    if (!buf) return -ENOMEM;

    ssize_t rd = node->ops->read(node, buf, node->size, 0);
    if (rd != (ssize_t)node->size) {
        kfree(buf);
        return -EIO;
    }

    struct elf_load_result res;
    rc = elf_load_into_aspace(buf, node->size, argv, argc, &res);
    kfree(buf);
    if (rc < 0) {
        pr_err("elf_spawn: ELF load failed for '%s' (rc=%d)\n", path, rc);
        return rc;
    }

    struct init_launch_args *la = kmalloc(sizeof(*la));
    if (!la) {
        vmm_destroy_address_space(res.cr3);
        return -ENOMEM;
    }
    la->entry     = res.entry;
    la->user_rsp  = res.user_rsp;
    la->cr3       = res.cr3;
    la->brk_start = res.brk_start;

    const char *base = strrchr(path, '/');
    const char *tname = base ? (base + 1) : path;
    pid_t pid = kthread_create(tname, user_init_trampoline, la);
    if (pid < 0) {
        vmm_destroy_address_space(res.cr3);
        kfree(la);
        return pid;
    }
    return pid;
}
