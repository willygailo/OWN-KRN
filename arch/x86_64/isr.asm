; x86_64 IDT Exception & IRQ Assembly Stubs
; Captures full GPR state into struct interrupt_frame and dispatches to C.

section .text
bits 64

extern interrupt_dispatch

%macro ISR_NOERR 1
isr_stub_%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
isr_stub_%1:
    push qword %1
    jmp isr_common
%endmacro

; CPU Exceptions (0..31)
ISR_NOERR 0     ; #DE Divide Error
ISR_NOERR 1     ; #DB Debug
ISR_NOERR 2     ; NMI
ISR_NOERR 3     ; #BP Breakpoint
ISR_NOERR 4     ; #OF Overflow
ISR_NOERR 5     ; #BR Bound Range Exceeded
ISR_NOERR 6     ; #UD Invalid Opcode
ISR_NOERR 7     ; #NM Device Not Available
ISR_ERR   8     ; #DF Double Fault (IST1)
ISR_NOERR 9     ; Coprocessor Segment Overrun
ISR_ERR   10    ; #TS Invalid TSS
ISR_ERR   11    ; #NP Segment Not Present
ISR_ERR   12    ; #SS Stack-Segment Fault
ISR_ERR   13    ; #GP General Protection Fault
ISR_ERR   14    ; #PF Page Fault
ISR_NOERR 15    ; Reserved
ISR_NOERR 16    ; #MF x87 FPU Error
ISR_ERR   17    ; #AC Alignment Check
ISR_NOERR 18    ; #MC Machine Check
ISR_NOERR 19    ; #XM SIMD Floating-Point
ISR_NOERR 20    ; #VE Virtualization
ISR_ERR   21    ; #CP Control Protection
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

; Hardware IRQs (32..47) + Software Test Vector (48)
%assign i 32
%rep 17
ISR_NOERR i
%assign i i+1
%endrep

isr_common:
    ; If interrupted from Ring 3 (CS & 3 != 0), switch to kernel GS base
    test qword [rsp + 24], 3
    jz .no_swapgs_in
    swapgs
.no_swapgs_in:

    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; RSP now points to struct interrupt_frame and is 16-byte aligned
    mov rdi, rsp
    call interrupt_dispatch

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; If returning to Ring 3, restore user GS base
    test qword [rsp + 24], 3
    jz .no_swapgs_out
    swapgs
.no_swapgs_out:

    add rsp, 16         ; discard int_no and err_code
    iretq

section .rodata
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 49
    dq isr_stub_%+i
%assign i i+1
%endrep
