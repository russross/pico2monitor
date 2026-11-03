#include "hardware/regs/rvcsr.h"

.option norelax
.section .bss
.balign 16
kernel_context: .space 64
trap_anchor: .space 16

/* A complete guest frame is saved without trusting its SP, GP, or TP. */
.section .text
.global guest_enter
.type guest_enter,@function
guest_enter:
    la t0, kernel_context
    sw ra, 0(t0)
    sw sp, 4(t0)
    sw gp, 8(t0)
    sw tp, 12(t0)
    sw s0, 16(t0)
    sw s1, 20(t0)
    sw s2, 24(t0)
    sw s3, 28(t0)
    sw s4, 32(t0)
    sw s5, 36(t0)
    sw s6, 40(t0)
    sw s7, 44(t0)
    sw s8, 48(t0)
    sw s9, 52(t0)
    sw s10, 56(t0)
    sw s11, 60(t0)
    la t0, trap_anchor
    sw a0, 0(t0)
    csrw mscratch, t0
    mv t6, a0
    lw t0, 128(t6)
    csrw mepc, t0
    li t0, 0x200080
#ifdef MONITOR_EMULATOR
    li t0, 0x201880
#endif
    csrw mstatus, t0

    /* CSR state is kernel-selected; only the integer register file comes from the guest. */
    lw ra, 4(t6)
    lw sp, 8(t6)
    lw gp, 12(t6)
    lw tp, 16(t6)
    lw t0, 20(t6)
    lw t1, 24(t6)
    lw t2, 28(t6)
    lw s0, 32(t6)
    lw s1, 36(t6)
    lw a0, 40(t6)
    lw a1, 44(t6)
    lw a2, 48(t6)
    lw a3, 52(t6)
    lw a4, 56(t6)
    lw a5, 60(t6)
    lw a6, 64(t6)
    lw a7, 68(t6)
    lw s2, 72(t6)
    lw s3, 76(t6)
    lw s4, 80(t6)
    lw s5, 84(t6)
    lw s6, 88(t6)
    lw s7, 92(t6)
    lw s8, 96(t6)
    lw s9, 100(t6)
    lw s10, 104(t6)
    lw s11, 108(t6)
    lw t3, 112(t6)
    lw t4, 116(t6)
    lw t5, 120(t6)
    lw t6, 124(t6)
    mret

.section .time_critical.monitor_trap_entry,"ax",@progbits
.global monitor_trap_entry
.type monitor_trap_entry,@function
monitor_trap_entry:
    csrrw t0, mscratch, t0
    beqz t0, kernel_fault_entry
    sw t1, 8(t0)
    lw t1, 0(t0)
    sw ra, 4(t1)
    sw sp, 8(t1)
    sw gp, 12(t1)
    sw tp, 16(t1)
    sw t2, 28(t1)
    csrr t2, mscratch
    sw t2, 20(t1)
    lw t2, 8(t0)
    sw t2, 24(t1)
    sw s0, 32(t1)
    sw s1, 36(t1)
    sw a0, 40(t1)
    sw a1, 44(t1)
    sw a2, 48(t1)
    sw a3, 52(t1)
    sw a4, 56(t1)
    sw a5, 60(t1)
    sw a6, 64(t1)
    sw a7, 68(t1)
    sw s2, 72(t1)
    sw s3, 76(t1)
    sw s4, 80(t1)
    sw s5, 84(t1)
    sw s6, 88(t1)
    sw s7, 92(t1)
    sw s8, 96(t1)
    sw s9, 100(t1)
    sw s10, 104(t1)
    sw s11, 108(t1)
    sw t3, 112(t1)
    sw t4, 116(t1)
    sw t5, 120(t1)
    sw t6, 124(t1)
    csrr t0, mepc
    sw t0, 128(t1)
    csrr t0, mcause
    sw t0, 132(t1)
    csrr t0, mstatus
    sw t0, 136(t1)

    /* No kernel C code executes until the trusted continuation is fully restored. */
    csrw mscratch, zero
    li t0, 0x200000
    csrw mstatus, t0
    li t0, 0x8000
    csrw 0xbe5, t0
    la t0, kernel_context
    lw ra, 0(t0)
    lw sp, 4(t0)
    lw gp, 8(t0)
    lw tp, 12(t0)
    lw s0, 16(t0)
    lw s1, 20(t0)
    lw s2, 24(t0)
    lw s3, 28(t0)
    lw s4, 32(t0)
    lw s5, 36(t0)
    lw s6, 40(t0)
    lw s7, 44(t0)
    lw s8, 48(t0)
    lw s9, 52(t0)
    lw s10, 56(t0)
    lw s11, 60(t0)
    ret

kernel_fault_entry:
    la sp, __emergency_stack_top
    la gp, __global_pointer$
    li tp, 0
    csrr a0, mepc
    csrr a1, mcause
    tail monitor_kernel_fault

/* SDK IRQ registration indexes its software table relative to mtvec. */
.section .bss
.balign 64
.global monitor_vectors
monitor_vectors:
    .space 0xd0 + 52 * 4
