# OwnKrn Architecture

## Virtual Memory Map (x86_64 4-Level Paging)

| Virtual Address Range | PML4 Range | Purpose & Permissions |
|---|---|---|
| `0x0000000000000000` – `0x0000000000000FFF` | `0` | Unmapped null guard page |
| `0x0000000000400000` – `0x00007FFFFFFFFFFF` | `0..255` | Per-process Ring 3 user space (ELF `PT_LOAD` W^X segments, `brk` heap, 64 KiB user stack ending at `0x7FFFFFFFE000`) |
| `0xFFFF800000000000` – `0xFFFF8000FFFFFFFF`+ | `256..383` | Higher-Half Direct Map (HHDM) of physical RAM (2 MiB RW+NX pages) |
| `0xFFFFC00000000000` – `0xFFFFC00003FFFFFF` | `384` | Dynamic kernel heap (`kmalloc`, shared across all address spaces via shared PDPT) |
| `0xFFFFFFFF80200000` – `__kernel_end` | `511` | Higher-half kernel image (`.text` RX, `.rodata` RO+NX, `.data`/`.bss` RW+NX) |

## Boot Flow

1. **Entry (`arch/x86_64/boot.asm`)**:
   - Exposes both a **Multiboot2 header** (`0xE85250D6`) in `.multiboot` and a **Xen/QEMU PVH ELF Note** (`type=18`, `XEN_ELFNOTE_PHYS32_ENTRY`) in `.note.Xen`.
   - Validates CPUID + 64-bit Long Mode, builds initial boot page tables mapping low memory, HHDM (`0xFFFF800000000000`), and higher-half kernel (`0xFFFFFFFF80000000`), enables `PAE`, `EFER.LME | EFER.NXE`, and `CR0.PG | CR0.WP`.
   - Loads a 64-bit GDT, jumps into higher-half `.text`, zeroes `.bss`, switches to a 32 KiB higher-half kernel stack, and calls `kmain(boot_magic, boot_info_phys)`.
2. **Kernel Initialization (`kernel/main.c`)**:
   - Initializes COM1 serial (`0x3F8`) and the **Bochs VBE 32-Bit Linear Framebuffer (`1024×576×32`, `128×36` grid)** via PCI BAR0 discovery (`0x1234:0x1111`, ports `0x01CE`/`0x01CF`), falling back to `0xB8000` VGA text console when VBE is absent.
   - Normalizes Multiboot2 or PVH boot information (`kernel/bootinfo.c`) into E820 memory map + initrd physical range.
   - Installs final 64-bit GDT + TSS (`IST1` double-fault stack), 256-entry IDT, remapped 8259 PIC (`32..47`), 100 Hz PIT (`IRQ0` + 1 Hz live Top HUD clock updater), PS/2 extended keyboard (`IRQ1`), COM1 RX (`IRQ4`), and TTY line discipline (canonical + raw/non-blocking modes).
   - Brings up PMM (`mm/pmm.c`), clean W^X VMM (`mm/vmm.c`, unmapping low identity memory), dynamic kernel heap (`mm/kmalloc.c`), and preemptive round-robin scheduler (`kernel/sched.c`) with `IA32_GS_BASE` anchored to `struct cpu_local`.
   - Probes Primary Master ATA0 28-bit LBA PIO disk (`drivers/ata.c`), mounts `tmpfs` at `/`, `devfs` at `/dev` (`null`, `zero`, `tty`, `serial`, `urandom`, `speaker`), `procfs` at `/proc` (`version`, `meminfo`, `uptime`, `tasks`, `rtc`, `cpuinfo`, `interrupts`, `mounts`), persistent `diskfs` at `/disk` (`fs/diskfs.c`), and unpacks `initrd.tar` into `/`.
   - Configures `IA32_EFER.SCE`, `IA32_STAR`, `IA32_LSTAR`, and `IA32_FMASK`, then loads `/bin/init` (PID 1) into Ring 3 via `elf_spawn_user_process()`.

## GDT & `SYSCALL`/`SYSRET` Invariant

| Selector | Descriptor | DPL | Role |
|---|---|---|---|
| `0x00` | Null | — | Required null entry |
| `0x08` | Kernel Code (64-bit) | 0 | `STAR[47:32]`, `SYSCALL` target CS |
| `0x10` | Kernel Data | 0 | `SYSCALL` target SS; `STAR[63:48]` user base |
| `0x18` (`0x1B`) | User Data | 3 | `SYSRETQ` target SS (`(0x10 + 8) | 3`) |
| `0x20` (`0x23`) | User Code (64-bit) | 3 | `SYSRETQ` target CS (`(0x10 + 16) | 3`) |
| `0x28` | 64-bit TSS (16 bytes) | 0 | `RSP0` updated per context switch; `IST1` isolates `#DF` |

### `swapgs` Polarity Invariant
- **Ring 0**: `IA32_GS_BASE = &g_cpu_local` (`[gs:0] = kstack_top`, `[gs:8] = user_rsp`), `IA32_KERNEL_GS_BASE = 0`.
- **Ring 3**: `IA32_GS_BASE = 0`, `IA32_KERNEL_GS_BASE = &g_cpu_local`.
- Both `isr.asm` (checking `CS & 3`) and `syscall.asm` / `enter_usermode` / `fork_child_trampoline` preserve this polarity across every Ring 0 ↔ Ring 3 crossing.

## Syscall Table (`include/uapi/syscall.h`)

Uses Linux x86_64 calling convention (`RAX` = number; `RDI, RSI, RDX, R10, R8, R9` = args 1..6; `RAX` = return or `-errno`):

| Number | Name | Description |
|---:|---|---|
| 0 | `read` | Read from file descriptor |
| 1 | `write` | Write to file descriptor |
| 2 | `open` | Open/create file (`O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_TRUNC`, `O_APPEND`) |
| 3 | `close` | Close file descriptor |
| 4 | `stat` | Stat file by path |
| 5 | `fstat` | Stat open file descriptor |
| 8 | `lseek` | Reposition file offset |
| 9 | `mmap` | Map anonymous 4 KiB user virtual memory pages (`0x0000400000000000+` arena) |
| 11 | `munmap` | Unmap user virtual memory pages and release physical frames |
| 12 | `brk` | Grow/shrink user heap |
| 16 | `ioctl` | Get/set TTY mode (`OWNKRN_TIOCGETM`/`TIOCSETM`) or clear kernel ring buffer (`OWNKRN_TIOCCLRKMSG`) |
| 22 | `pipe` | Create unidirectional kernel ring-buffer pipe (`pipefd[2]`) |
| 24 | `sched_yield` | Voluntarily yield CPU |
| 32 | `dup` | Duplicate file descriptor to lowest free slot |
| 33 | `dup2` | Duplicate file descriptor onto specific slot |
| 35 | `nanosleep` | Sleep for `struct timespec` |
| 39 | `getpid` | Get current process ID |
| 57 | `fork` | Clone process address space, FDs, credentials, and register frame |
| 59 | `execve` | Replace address space with static ELF64 binary + `argv` |
| 60 | `exit` | Terminate process with exit status (auto-restores canonical TTY mode) |
| 61 | `wait4` | Wait for and reap zombie child process (`WNOHANG` supported) |
| 62 | `kill` | Deliver POSIX signal (`SIGINT`, `SIGTERM`, `SIGKILL`, `SIGPIPE`) |
| 63 | `uname` | Return `struct utsname` kernel identification |
| 78 | `getdents` | Read directory entries (`struct uapi_dirent`) |
| 79 | `getcwd` | Get current working directory |
| 80 | `chdir` | Change working directory |
| 82 | `rename` | Atomically move/rename file or directory entry across `tmpfs` |
| 83 | `mkdir` | Create directory |
| 87 | `unlink` | Remove directory entry (refcounted inode cleanup) |
| 90 | `chmod` | Modify VFS inode permission mode bits |
| 102 | `getuid` | Get real user ID |
| 104 | `getgid` | Get real group ID |
| 105 | `setuid` | Set user ID (`0..65535`) |
| 106 | `setgid` | Set group ID (`0..65535`) |
| 107 | `geteuid` | Get effective user ID |
| 108 | `getegid` | Get effective group ID |
| 110 | `getppid` | Get parent process ID |
| 169 | `reboot` | ACPI poweroff or 8042 CPU reset |
| 228 | `clock_gettime` | Query `CLOCK_REALTIME` (CMOS RTC) or `CLOCK_MONOTONIC` (PIT) |
