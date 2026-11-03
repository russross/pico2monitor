.option norelax
.section .text.start,"ax",@progbits
.global _start
_start:
    /* Every saved register is live across several timer preemptions. */
    li ra, 0x101
    li gp, 0x103
    li tp, 0x104
    li t1, 0x106
    li t2, 0x107
    li s0, 0x108
    li s1, 0x109
    li a0, 0x10a
    li a1, 0x10b
    li a2, 0x10c
    li a3, 0x10d
    li a4, 0x10e
    li a5, 0x10f
    li a6, 0x110
    li a7, 0x111
    li s2, 0x112
    li s3, 0x113
    li s4, 0x114
    li s5, 0x115
    li s6, 0x116
    li s7, 0x117
    li s8, 0x118
    li s9, 0x119
    li s10, 0x11a
    li s11, 0x11b
    li t3, 0x11c
    li t4, 0x11d
    li t5, 0x11e
    li t6, 0x11f
    li t0, 1000000
1:  addi t0, t0, -1
    bnez t0, 1b

    /* Checks use one scratch register and leave all other comparisons independent. */
.macro check register, value
    li t0, \value
    bne \register, t0, failed
.endm
    check ra, 0x101
    # Startup SP includes exec tables and varies with the launch strings.
    check gp, 0x103
    check tp, 0x104
    check t1, 0x106
    check t2, 0x107
    check s0, 0x108
    check s1, 0x109
    check a0, 0x10a
    check a1, 0x10b
    check a2, 0x10c
    check a3, 0x10d
    check a4, 0x10e
    check a5, 0x10f
    check a6, 0x110
    check a7, 0x111
    check s2, 0x112
    check s3, 0x113
    check s4, 0x114
    check s5, 0x115
    check s6, 0x116
    check s7, 0x117
    check s8, 0x118
    check s9, 0x119
    check s10, 0x11a
    check s11, 0x11b
    check t3, 0x11c
    check t4, 0x11d
    check t5, 0x11e
    check t6, 0x11f
    li a0, 1
    la a1, message
    li a2, 19
    li a7, 64
    ecall
    li a0, 0
    j finished
failed:
    li a0, 42
finished:
    li a7, 93
    ecall
2:  j 2b
.section .rodata
message: .ascii "register frame OK\n"
.section .note.GNU-stack,"",@progbits
