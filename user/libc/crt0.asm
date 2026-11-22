; Freestanding Userspace C Runtime Startup (_start)
; Kernel ELF64 loader places [rsp] = argc, [rsp+8] = argv[0], 16-byte aligned.

section .text
bits 64

extern main
extern u_exit

global _start
_start:
    xor rbp, rbp
    mov rdi, [rsp]          ; argc
    lea rsi, [rsp + 8]      ; argv
    call main
    mov rdi, rax
    call u_exit
.hang:
    jmp .hang
