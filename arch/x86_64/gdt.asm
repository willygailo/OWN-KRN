; Load 64-bit GDT, reload code/data segment registers, and load TSS.
; void gdt_flush(const struct gdt_ptr *gdtr, uint16_t tss_sel);

section .text
bits 64

global gdt_flush
gdt_flush:
    lgdt [rdi]

    ; Reload data segments with Kernel Data (0x10)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Far return to reload CS with Kernel Code (0x08)
    push 0x08
    lea rax, [rel .reload_cs]
    push rax
    retfq

.reload_cs:
    ; Load Task State Segment selector (passed in SI)
    mov ax, si
    ltr ax
    ret
