/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* BethOS x86_64 OSIF L0.
 *
 * BethOS keeps its native inline syscall ABI for the existing runtime. The
 * Jash/Bear libc ABI uses a separate tagged range so both ABIs can coexist
 * while the POSIX surface is brought up incrementally.
 */

    .text
    .extern environ
.globl _start
    .type _start, @function
_start:
    xor  %ebp, %ebp
    mov  (%rsp), %edi
    mov  %edi, %r8d
    lea  8(%rsp), %rsi
    lea  8(%rsi,%rdi,8), %rax
    mov  %rax, environ(%rip)
    mov  %rsi, %rdx
    /* Point local-exec TLS at reserved space in the initial user stack. */
    mov  $0x109e, %eax
    mov  $0x1002, %edi
    lea  -128(%rsp), %rsi
    int  $0x81
    mov  %r8d, %edi
    mov  %rdx, %rsi
    call main
    mov  %eax, %edi
    call bear_osif_exit
1:
    hlt
    jmp  1b
    .size _start, .-_start

    .globl bear_osif_syscall6
    .type bear_osif_syscall6, @function

/* BethOS_POSIX_SYSCALL_BASE is 0x1000. */
bear_osif_syscall6:
    push %rbp
    mov  %rsp, %rbp
    add  $0x1000, %rdi
    mov  %rdi, %rax
    mov  %rsi, %rdi
    mov  %rdx, %rsi
    mov  %rcx, %rdx
    mov  %r8,  %r10
    mov  %r9,  %r8
    mov  16(%rbp), %r9
    int  $0x81
    leave
    ret

    .size bear_osif_syscall6, .-bear_osif_syscall6

    .globl bear_osif_exit
    .type bear_osif_exit, @function
bear_osif_exit:
    mov  $0x103c, %eax
    int  $0x81
1:
    hlt
    jmp  1b
    .size bear_osif_exit, .-bear_osif_exit

    .section .note.GNU-stack,"",@progbits
