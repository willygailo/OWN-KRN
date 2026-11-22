; OwnKrn x86_64 Bootstrap (Multiboot2 + QEMU PVH ELF64)
; Transitions from 32-bit protected mode -> 64-bit Long Mode in the higher half.

%define MB2_MAGIC           0xE85250D6
%define MB2_ARCH_I386       0
%define PVH_MAGIC           0x336EC578
%define XEN_ELFNOTE_PHYS32  18

%define KERNEL_VIRT_BASE    0xFFFFFFFF80000000
%define PAGE_PRESENT        (1 << 0)
%define PAGE_WRITABLE       (1 << 1)
%define PAGE_HUGE           (1 << 7)
%define PAGE_FLAGS_2M       (PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE)

extern kmain
extern __bss_start
extern __bss_end

; ---------------------------------------------------------------------------
; Multiboot2 Header (for GRUB2 / bootable ISO)
; ---------------------------------------------------------------------------
section .multiboot alloc noexec align=8
mb2_header_start:
    dd MB2_MAGIC
    dd MB2_ARCH_I386
    dd mb2_header_end - mb2_header_start
    dd -(MB2_MAGIC + MB2_ARCH_I386 + (mb2_header_end - mb2_header_start))

    ; End tag
    align 8
    dw 0
    dw 0
    dd 8
mb2_header_end:

; ---------------------------------------------------------------------------
; PVH ELF Note (for direct QEMU -kernel ELF64 boot)
; ---------------------------------------------------------------------------
section .note.Xen note alloc noexec align=4
    dd 4                    ; namesz ("Xen\0")
    dd 4                    ; descsz (uint32_t phys32 entry)
    dd XEN_ELFNOTE_PHYS32   ; type = 18
    db "Xen", 0             ; name
    dd _start_pvh           ; physical entry point

; ---------------------------------------------------------------------------
; 32-bit Protected-Mode Entry & Long-Mode Trampoline
; ---------------------------------------------------------------------------
section .boot.text alloc exec align=16
bits 32

global _start
global _start_pvh

_start:
    ; Multiboot2 entry: EAX = 0x36D76289, EBX = physical addr of mb2 info
    jmp _start_common

_start_pvh:
    ; QEMU PVH entry: EBX = physical addr of struct hvm_start_info
    mov eax, PVH_MAGIC
    jmp _start_common

_start_common:
    cli
    cld

    ; Preserve boot magic (EDI) and boot info physical pointer (ESI)
    mov edi, eax
    mov esi, ebx

    ; Establish temporary 32-bit stack
    mov esp, boot_stack_top

    ; Verify CPUID support
    pushfd
    pop eax
    mov ecx, eax
    xor eax, (1 << 21)
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .no_long_mode

    ; Verify extended CPUID >= 0x80000001 and Long Mode bit (EDX bit 29)
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode

    mov eax, 0x80000001
    cpuid
    test edx, (1 << 29)
    jz .no_long_mode

    ; Zero boot page tables (8 * 4096 bytes)
    push edi
    push esi
    mov edi, boot_pml4
    xor eax, eax
    mov ecx, (8 * 4096) / 4
    rep stosd
    pop esi
    pop edi

    ; Populate 4 Page Directories (boot_pd_0..3) covering 0..4 GiB with 2 MiB pages
    ; boot_pd_0..3 are contiguous in .boot.bss (4 * 512 = 2048 entries)
    push edi
    mov edi, boot_pd_0
    xor eax, eax            ; physical base = 0
    mov ecx, 2048
.fill_pd:
    mov edx, eax
    or edx, PAGE_FLAGS_2M
    mov [edi], edx
    mov dword [edi + 4], 0
    add eax, 0x00200000     ; +2 MiB
    add edi, 8
    dec ecx
    jnz .fill_pd
    pop edi

    ; Wire boot_pdpt_low[0..3] -> boot_pd_0..3 (identity map 0..4 GiB)
    mov eax, boot_pd_0
    or eax, (PAGE_PRESENT | PAGE_WRITABLE)
    mov [boot_pdpt_low + 0 * 8], eax
    add eax, 4096
    mov [boot_pdpt_low + 1 * 8], eax
    add eax, 4096
    mov [boot_pdpt_low + 2 * 8], eax
    add eax, 4096
    mov [boot_pdpt_low + 3 * 8], eax

    ; Wire boot_pdpt_hhdm[0..3] -> boot_pd_0..3 (HHDM 0xFFFF800000000000+)
    mov eax, boot_pd_0
    or eax, (PAGE_PRESENT | PAGE_WRITABLE)
    mov [boot_pdpt_hhdm + 0 * 8], eax
    add eax, 4096
    mov [boot_pdpt_hhdm + 1 * 8], eax
    add eax, 4096
    mov [boot_pdpt_hhdm + 2 * 8], eax
    add eax, 4096
    mov [boot_pdpt_hhdm + 3 * 8], eax

    ; Wire boot_pdpt_high[510] -> boot_pd_0 (Kernel VMA 0xFFFFFFFF80000000..+1 GiB)
    mov eax, boot_pd_0
    or eax, (PAGE_PRESENT | PAGE_WRITABLE)
    mov [boot_pdpt_high + 510 * 8], eax

    ; Wire PML4 entries:
    ;   PML4[0]   -> boot_pdpt_low  (identity map for transition)
    ;   PML4[256] -> boot_pdpt_hhdm (0xFFFF800000000000)
    ;   PML4[511] -> boot_pdpt_high (0xFFFFFFFF80000000)
    mov eax, boot_pdpt_low
    or eax, (PAGE_PRESENT | PAGE_WRITABLE)
    mov [boot_pml4 + 0 * 8], eax

    mov eax, boot_pdpt_hhdm
    or eax, (PAGE_PRESENT | PAGE_WRITABLE)
    mov [boot_pml4 + 256 * 8], eax

    mov eax, boot_pdpt_high
    or eax, (PAGE_PRESENT | PAGE_WRITABLE)
    mov [boot_pml4 + 511 * 8], eax

    ; Load CR3 with PML4 physical address
    mov eax, boot_pml4
    mov cr3, eax

    ; Enable PAE (CR4.PAE = bit 5)
    mov eax, cr4
    or eax, (1 << 5)
    mov cr4, eax

    ; Enable Long Mode (IA32_EFER.LME = bit 8) and NXE (bit 11)
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8) | (1 << 11)
    wrmsr

    ; Enable Paging (CR0.PG = bit 31) and Write Protect (CR0.WP = bit 16)
    mov eax, cr0
    or eax, (1 << 31) | (1 << 16)
    mov cr0, eax

    ; Load 64-bit GDT and far jump into 64-bit code segment (0x08)
    lgdt [boot_gdt64_ptr]
    jmp 0x08:long_mode_low

.no_long_mode:
    mov dword [0xB8000], 0x4F4E4F45 ; "EN" red background
    mov dword [0xB8004], 0x4F4D4F4C ; "LM"
.fail_halt:
    cli
    hlt
    jmp .fail_halt

bits 64
long_mode_low:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Zero-extend 32-bit boot args into 64-bit RDI/RSI
    mov edi, edi
    mov esi, esi

    ; Jump into the higher-half virtual address space
    mov rax, higher_half_start
    jmp rax

; ---------------------------------------------------------------------------
; Bootstrap GDT & Page Tables (Low Memory)
; ---------------------------------------------------------------------------
section .boot.rodata alloc noexec align=16
boot_gdt64:
    dq 0x0000000000000000           ; 0x00: Null descriptor
    dq 0x00AF9A000000FFFF           ; 0x08: 64-bit Kernel Code (DPL=0, L=1)
    dq 0x00CF92000000FFFF           ; 0x10: 64-bit Kernel Data (DPL=0)
boot_gdt64_end:

boot_gdt64_ptr:
    dw boot_gdt64_end - boot_gdt64 - 1
    dq boot_gdt64

section .boot.bss alloc write noexec nobits align=4096
global boot_pml4
boot_pml4:      resb 4096
boot_pdpt_low:  resb 4096
boot_pdpt_hhdm: resb 4096
boot_pdpt_high: resb 4096
boot_pd_0:      resb 4096
boot_pd_1:      resb 4096
boot_pd_2:      resb 4096
boot_pd_3:      resb 4096

align 16
boot_stack_bottom:
    resb 4096
boot_stack_top:

; ---------------------------------------------------------------------------
; Higher-Half 64-Bit Kernel Entry
; ---------------------------------------------------------------------------
section .text
bits 64

higher_half_start:
    ; Zero kernel .bss while still on the low boot stack
    push rdi
    push rsi
    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi
    xor eax, eax
    rep stosb
    pop rsi
    pop rdi

    ; Switch to higher-half kernel stack (16-byte aligned)
    mov rsp, kernel_boot_stack_top
    xor rbp, rbp

    ; kmain(uint64_t boot_magic, uint64_t boot_info_phys)
    call kmain

.halt_loop:
    cli
    hlt
    jmp .halt_loop

section .bss
align 16
global kernel_boot_stack_bottom
global kernel_boot_stack_top
kernel_boot_stack_bottom:
    resb 32768                      ; 32 KiB early kernel stack
kernel_boot_stack_top:
