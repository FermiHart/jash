/*═══════════════════════════════════════════════════════════════════════════════
 *
 *   osif/linux/syscall.S — Linux x86_64 raw syscall backend
 *
 *   No libc, no translation, no indirection.
 *   The syscall instruction goes straight to the kernel.
 *   Arguments follow the Linux x86_64 ABI:
 *     rax = syscall number
 *     rdi, rsi, rdx, r10, r8, r9 = args 1-6
 *
 *   Build with: -ffreestanding -fno-omit-frame-pointer
 *
 *   F E R M I ∞ H A R T | SPDX-License-Identifier: LicenseRef-SPL-1.0
 *═══════════════════════════════════════════════════════════════════════════════*/

    .text
    .globl bear_osif_syscall6
    .type bear_osif_syscall6, @function

/*
 * long bear_osif_syscall6(long nr, long a1, long a2, long a3,
 *                          long a4, long a5, long a6)
 *
 * C calling convention (System V AMD64):
 *   rdi = nr, rsi = a1, rdx = a2, rcx = a3
 *   r8  = a4, r9  = a5, [rsp+8] = a6
 *
 * Linux syscall ABI:
 *   rax = nr, rdi = a1, rsi = a2, rdx = a3
 *   r10 = a4, r8  = a5, r9  = a6
 */
bear_osif_syscall6:
    /* Save callee-saved registers we clobber */
    push %rbp
    mov  %rsp, %rbp

    /* Move C args → syscall regs */
    mov  %rdi, %rax    /* nr → rax */
    mov  %rsi, %rdi    /* a1 → rdi */
    mov  %rdx, %rsi    /* a2 → rsi */
    mov  %rcx, %rdx    /* a3 → rdx */
    mov  %r8,  %r10    /* a4 → r10 */
    mov  %r9,  %r8     /* a5 → r8 */
    mov  16(%rbp), %r9 /* a6 (from stack) → r9 */

    /* Fire! */
    syscall

    /* Check for error (rax >= -4095 means -errno) */
    cmp  $-4095, %rax
    jae  .L_sys_error

    /* Success — return rax as-is */
    leave
    ret

.L_sys_error:
    /* Convert kernel -errno to negative return value */
    neg  %rax
    neg  %rax            /* already negative, keep it */
    leave
    ret

    .size bear_osif_syscall6, .-bear_osif_syscall6

/* ── bear_osif_exit(int code) ────────────────────────────────────────────── */
    .globl bear_osif_exit
    .type bear_osif_exit, @function

bear_osif_exit:
    mov  %edi, %edi      /* code in edi */
    mov  $60, %rax       /* __NR_exit */
    syscall
    /* Never returns */
    hlt
    jmp  .

    .size bear_osif_exit, .-bear_osif_exit

/* ── Section note: non-executable stack (PaX MPROTECT) ──────────────────── */
    .section .note.GNU-stack,"",@progbits
