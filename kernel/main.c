#include <arch/gdt.h>
#include <arch/idt.h>
#include <arch/io.h>
#include <arch/pic.h>
#include <arch/pit.h>
#include <drivers/ata.h>
#include <drivers/console.h>
#include <drivers/keyboard.h>
#include <drivers/pci.h>
#include <drivers/rtc.h>
#include <drivers/serial.h>
#include <drivers/tty.h>
#include <fs/vfs.h>
#include <kernel/bootinfo.h>
#include <kernel/elf.h>
#include <kernel/panic.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/types.h>
#include <mm/kmalloc.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

static struct boot_info g_boot_info;

static uint64_t current_rip(void) {
    uint64_t rip;
    __asm__ volatile("lea (%%rip), %0" : "=r"(rip));
    return rip;
}

static void enable_cpu_security_features(void) {
    uint32_t max_leaf = 0, ebx = 0;
    cpuid_leaf(0, 0, &max_leaf, NULL, NULL, NULL);
    bool smep_enabled = false;
    if (max_leaf >= 7) {
        cpuid_leaf(7, 0, NULL, &ebx, NULL, NULL);
        if (ebx & (1U << 7)) { /* SMEP */
            write_cr4(read_cr4() | (1ULL << 20));
            smep_enabled = true;
        }
    }
    pr_ok("CPU hardening : NXE=on, WP=on, SMEP=%s\n", smep_enabled ? "on" : "n/a");
}

void kmain(uint64_t boot_magic, uint64_t boot_info_phys) {
    serial_init();
    console_init();

    printk("\n");
    printk("====================================================\n");
    printk("  OwnKrn 0.3.0-x86_64 (Linux-Inspired OS Kernel)\n");
    printk("====================================================\n");

    uint64_t rip = current_rip();
    KASSERT(rip >= KERNEL_VIRT_BASE);
    pr_ok("Long Mode active in higher half (RIP=0x%016llx)\n",
          (unsigned long long)rip);
    enable_cpu_security_features();

    if (!bootinfo_parse(boot_magic, boot_info_phys, &g_boot_info)) {
        kpanic("Failed to parse boot info (magic=0x%llx)",
               (unsigned long long)boot_magic);
    }
    pr_info("Boot protocol : %s (%llu MiB usable RAM)\n",
            g_boot_info.protocol,
            (unsigned long long)(g_boot_info.total_usable_ram / (1024ULL * 1024ULL)));

    struct rtc_time rt;
    rtc_read(&rt);
    pr_info("CMOS RTC      : %04u-%02u-%02u %02u:%02u:%02u UTC\n",
            rt.year, rt.month, rt.day, rt.hour, rt.minute, rt.second);

    /* Core CPU tables, interrupt controller, timer & TTY */
    gdt_init();
    idt_init();
    pic_init();
    pit_init(PIT_HZ);
    keyboard_init();
    tty_init();
    pr_ok("GDT/TSS, IDT (256 vectors + IST1 #DF), PIC, PIT (%u Hz), KBD/TTY initialized\n",
          PIT_HZ);

    /* Memory management & preemptive scheduler */
    pmm_init(&g_boot_info);
    vmm_init(&g_boot_info);
    kheap_init();
    sched_init();

    /* PCI Bus enumeration, Virtual File System, persistent ATA0 disk & initramfs */
    pci_init();
    ata_init();
    vfs_init();
    if (g_boot_info.initrd_size > 0) {
        initrd_unpack(g_boot_info.initrd_phys, g_boot_info.initrd_size);
    } else {
        pr_warn("No initrd attached\n");
    }

    /* Ring 3 SYSCALL/SYSRET interface */
    syscall_init();

    /* Launch PID 1 (/bin/init) in Ring 3 */
    irq_enable();
    const char *init_argv[] = { "/bin/init", NULL };
    pid_t init_pid = elf_spawn_user_process("/bin/init", init_argv, 1);
    if (init_pid < 0) {
        kpanic("Failed to launch /bin/init (err=%d)", init_pid);
    }
    pr_ok("Launched /bin/init (pid=%d) in Ring 3\n", init_pid);

    /* Kernel idle / init reaper loop */
    int status = 0;
    sched_waitpid(init_pid, &status, 0);
    pr_warn("/bin/init exited (status=%d); halting system.\n", status);
    arch_poweroff();
}
