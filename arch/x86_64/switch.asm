; x86_64 Context Switch & New Kernel Thread / User Task Trampolines
; void switch_context(uint64_t *old_rsp, uint64_t new_rsp, uint64_t new_cr3);

section .text
bits 64

extern sched_post_switch
extern task_exit

global switch_context
switch_context:
    ; Save System V AMD64 callee-saved registers on old task's kernel stack
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; Store current RSP into *old_rsp (RDI)
    mov [rdi], rsp

    ; Switch CR3 only when address space changes (RDX = new_cr3)
    mov rax, cr3
    cmp rax, rdx
    je .same_cr3
    mov cr3, rdx
.same_cr3:

    ; Load new task's kernel stack pointer (RSI = new_rsp)
    mov rsp, rsi

    ; Restore callee-saved registers of new task
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; Initial entry point for newly spawned kernel threads.
; Expects: R12 = fn pointer, R13 = arg pointer.
global kthread_trampoline
kthread_trampoline:
    call sched_post_switch
    mov rdi, r13
    call r12
    xor edi, edi
    call task_exit
.hang:
    cli
    hlt
    jmp .hang

; Enter Ring 3 for the first time (used by ELF64 execve / init launch).
; void enter_usermode(uint64_t user_rip, uint64_t user_rsp, uint64_t user_rflags);
;   RDI = user RIP
;   RSI = user RSP
;   RDX = user RFLAGS (e.g. 0x202)
global enter_usermode
enter_usermode:
    cli
    ; Swap kernel GS -> user GS before dropping to Ring 3
    swapgs

    ; Set user data segments (USER_DS = 0x1B)
    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    mov fs, ax

    ; Build IRETQ frame: SS, RSP, RFLAGS, CS, RIP
    push qword 0x1B         ; SS (USER_DS)
    push rsi                ; RSP
    push rdx                ; RFLAGS (IF=1)
    push qword 0x23         ; CS (USER_CS)
    push rdi                ; RIP

    ; Clear GPRs so no kernel values leak to userspace
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rsi, rsi
    xor rdi, rdi
    xor rbp, rbp
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

    iretq

; Trampoline for fork() child task: releases scheduler lock, restores exact
; struct interrupt_frame on top of child's kernel stack (with RAX=0), and IRETQs
; back into Ring 3.
global fork_child_trampoline
fork_child_trampoline:
    call sched_post_switch
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
    swapgs
    add rsp, 16             ; skip int_no, err_code
    iretq
