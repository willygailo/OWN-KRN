; x86_64 SYSCALL / SYSRET Entry & Exit Trampoline
; Follows Linux x86_64 ABI:
;   RAX = syscall number
;   RDI, RSI, RDX, R10, R8, R9 = args 1..6
;   RCX = user RIP (saved by CPU)
;   R11 = user RFLAGS (saved by CPU)

section .text
bits 64

extern syscall_dispatch

global syscall_entry
syscall_entry:
    ; Switch to kernel per-CPU GS base (&g_cpu_local)
    swapgs

    ; Save user RSP in cpu_local.user_rsp and switch to task's kernel stack top
    mov [gs:8], rsp
    mov rsp, [gs:0]

    ; Build a full struct interrupt_frame on the kernel stack (22 qwords = 176 bytes)
    ; Keeping exact layout parity with isr.asm allows fork() to duplicate the frame
    ; directly onto the child's kernel stack and resume Ring 3 cleanly via IRETQ.
    push qword 0x1B             ; ss (USER_DS)
    push qword [gs:8]           ; user rsp
    push r11                    ; user rflags
    push qword 0x23             ; cs (USER_CS)
    push rcx                    ; user rip
    push qword 0                ; err_code
    push qword 0x80             ; int_no (syscall marker)

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

    ; RSP is 16-byte aligned and points to struct interrupt_frame
    sti
    mov rdi, rsp
    call syscall_dispatch

    ; Store syscall return value in frame->rax (index 14 -> offset 112)
    mov [rsp + 14 * 8], rax

    cli
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

    add rsp, 16                 ; skip int_no, err_code

    ; Return to Ring 3 via 64-bit SYSRETQ:
    ;   RCX = user RIP, R11 = user RFLAGS, RSP = user RSP
    pop rcx                     ; user RIP
    add rsp, 8                  ; skip CS
    pop r11                     ; user RFLAGS
    mov rsp, [rsp]              ; restore user RSP

    swapgs
    o64 sysret
