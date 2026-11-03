.option norelax
.section .text.start,"ax",@progbits
.global _start
_start:
    la gp, __global_pointer$
    lw a0, 0(sp)
    addi a1, sp, 4
    addi a2, a0, 1
    slli a2, a2, 2
    add a2, a1, a2
    call main
    li a7, 93
    ecall
1:  j 1b
.section .note.GNU-stack,"",@progbits
