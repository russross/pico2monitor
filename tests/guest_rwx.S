/* A single RWX segment is intentional: internal restrictions must not reject
 * self-modifying exercises or grant access outside the guest sandbox. */
.section .text
.global _start
_start:
    la t0, marker
    li t1, 17
    sw t1, 0(t0)
    lw t2, 0(t0)
    bne t1, t2, failed
    li a0, 1
    la a1, message
    la a2, message_end
    sub a2, a2, a1
    li a7, 64
    ecall
    j boundary_code
.balign 128
    .space 126
boundary_code:
    /* A four-byte instruction may begin on a two-byte boundary with RVC. */
.option norvc
    li a0, 0
    li a7, 93
    ecall
failed:
    li a0, 42
    li a7, 93
    ecall
.section .data
.balign 4
marker:
    .word 1
.section .text
message:
    .ascii "RWX sandbox OK\n"
message_end:
.section .note.GNU-stack,"",@progbits
