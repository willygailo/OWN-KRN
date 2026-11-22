; Raw Linux x86_64 SYSCALL invocation wrapper
; int64_t u_syscall(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
;                   uint64_t a4, uint64_t a5, uint64_t a6);
; C ABI args:   rdi=nr, rsi=a1, rdx=a2, rcx=a3, r8=a4, r9=a5, [rsp+8]=a6
; SYSCALL ABI:  rax=nr, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5, r9=a6

section .text
bits 64

global u_syscall
u_syscall:
    mov rax, rdi            ; syscall number
    mov rdi, rsi            ; arg1
    mov rsi, rdx            ; arg2
    mov rdx, rcx            ; arg3
    mov r10, r8             ; arg4
    mov r8,  r9             ; arg5
    mov r9,  [rsp + 8]      ; arg6
    syscall
    ret
