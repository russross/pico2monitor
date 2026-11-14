#include "syscall.h"
.option norelax
.section .rodata
busy_message: .ascii "two threads running\n"
busy_end:
done_message: .ascii "thread replacement OK\n"
done_end:
.section .text
.global _start
_start:
    la t0, first_stack
    sw sp, 0(t0)
    la a0, second_start
    la a1, second_stack
    li a7, SYS_THREAD_CREATE
    ecall
    li t0, 1
    bne a0, t0, failure
#if THREAD_TEST == 0
    /* Keep the second thread alive while checking the two-slot limit. */
    la a0, second_start
    li a1, 0
    li a7, SYS_THREAD_CREATE
    ecall
    li t0, -11
    bne a0, t0, failure
    la t0, release
    li t1, 1
    sw t1, 0(t0)
    li a0, 0
    li a7, SYS_EXIT
    ecall
#else
    li a0, 1
    la a1, busy_message
    la a2, busy_end
    sub a2, a2, a1
    li a7, SYS_WRITE
    ecall
1:
    j 1b
#endif

/* The raw secondary entry checks the actual hardware register contract before
 * using any stack or global pointer. Its parameter supplies its stack top. */
second_start:
    or t0, t0, ra
    or t0, t0, sp
    or t0, t0, gp
    or t0, t0, tp
    or t0, t0, t1
    or t0, t0, t2
    or t0, t0, s0
    or t0, t0, s1
    or t0, t0, a1
    or t0, t0, a2
    or t0, t0, a3
    or t0, t0, a4
    or t0, t0, a5
    or t0, t0, a6
    or t0, t0, a7
    or t0, t0, s2
    or t0, t0, s3
    or t0, t0, s4
    or t0, t0, s5
    or t0, t0, s6
    or t0, t0, s7
    or t0, t0, s8
    or t0, t0, s9
    or t0, t0, s10
    or t0, t0, s11
    or t0, t0, t3
    or t0, t0, t4
    or t0, t0, t5
    or t0, t0, t6
    bnez t0, failure
    mv sp, a0
#if THREAD_TEST == 0
    la s0, release
2:
    lw t0, 0(s0)
    beqz t0, 2b
    li s1, 3
    /* The surviving thread reuses slot zero repeatedly. The initial stack is
     * explicitly passed back to the replacement after its old owner exits. */
3:
    la a0, replacement
    la a1, first_stack
    li a7, SYS_THREAD_CREATE
    ecall
    li t0, -11
    beq a0, t0, 3b
    bnez a0, failure
4:
    lw t0, 4(s0)
    beqz t0, 4b
    sw zero, 4(s0)
    addi s1, s1, -1
    bnez s1, 3b
    li a0, 1
    la a1, done_message
    la a2, done_end
    sub a2, a2, a1
    li a7, SYS_WRITE
    ecall
    li a0, 0
    li a7, SYS_EXIT
    ecall
#elif THREAD_TEST == 1
5:
    j 5b
#else
    ebreak
#endif

replacement:
    lw sp, 0(a0)
    la t0, release
    li t1, 1
    sw t1, 4(t0)
    li a0, 0
    li a7, SYS_EXIT
    ecall
failure:
    li a0, 99
    li a7, SYS_EXIT_GROUP
    ecall

.section .data
.balign 4
first_stack: .word 0
release: .word 0, 0
.section .bss
.balign 16
    .space 4096
second_stack:
