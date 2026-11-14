#include "hardware/regs/rvcsr.h"

.option norelax
/* The gate uses one trusted SRAM word and RISC-V atomic instructions.
 * All paths before acquisition and after release are independent of SP/GP/TP. */
.equ CORE_ID, 0xd0000000
.section .time_critical.monitor_trap_entry,"ax",@progbits
.global monitor_trap_entry
.type monitor_trap_entry,@function
monitor_trap_entry:
    csrrw t0, mscratch, t0
    beqz t0, machine_trap
    sw t1, 24(t0)
    sw t2, 28(t0)
    csrr t1, mscratch
    sw t1, 20(t0)
    la t1, monitor_panic_owner
    lw t1, 0(t1)
    bnez t1, monitor_panic_park
    sw ra, 4(t0)
    sw sp, 8(t0)
    sw gp, 12(t0)
    sw tp, 16(t0)
    sw s0, 32(t0)
    sw s1, 36(t0)
    sw a0, 40(t0)
    sw a1, 44(t0)
    sw a2, 48(t0)
    sw a3, 52(t0)
    sw a4, 56(t0)
    sw a5, 60(t0)
    sw a6, 64(t0)
    sw a7, 68(t0)
    sw s2, 72(t0)
    sw s3, 76(t0)
    sw s4, 80(t0)
    sw s5, 84(t0)
    sw s6, 88(t0)
    sw s7, 92(t0)
    sw s8, 96(t0)
    sw s9, 100(t0)
    sw s10, 104(t0)
    sw s11, 108(t0)
    sw t3, 112(t0)
    sw t4, 116(t0)
    sw t5, 120(t0)
    sw t6, 124(t0)
    csrr t1, mepc
    sw t1, 128(t0)
    csrr t1, mcause
    sw t1, 132(t0)
    csrr t1, mstatus
    sw t1, 136(t0)
    li a1, 1
    j monitor_gate

/* Idle interrupts need no saved machine context. Synchronous monitor faults
 * coordinate recovery without using the interrupted stack or normal gate. */
machine_trap:
    csrw mscratch, zero
    csrr t0, mcause
    bgez t0, kernel_fault_entry
    li a1, 0
monitor_gate:
    csrw mscratch, zero
    la t0, monitor_gate_lock
    li t1, 1
    la t3, monitor_panic_owner
1:
    lw t2, 0(t3)
    bnez t2, monitor_panic_park
    amoswap.w.aq t2, t1, (t0)
    bnez t2, 1b
    j monitor_owned

.global monitor_start_locked
.type monitor_start_locked,@function
monitor_start_locked:
    csrci mstatus, 8
    li a1, 0
monitor_owned:
    la t0, monitor_panic_owner
    lw t1, 0(t0)
    bnez t1, monitor_panic_park
    la sp, __StackTop
    la gp, __global_pointer$
    li tp, 0
    li t0, CORE_ID
    lw a0, 0(t0)
    call monitor_dispatch
monitor_return:

    /* The C stack is abandoned before publishing the unlocked gate. A guest
     * frame selected here cannot be reset while its executing flag is set. */
    mv t6, a0
    la t0, monitor_panic_owner
    lw t1, 0(t0)
    bnez t1, monitor_panic_park
    la t0, monitor_gate_lock
    amoswap.w.rl zero, zero, (t0)
    beqz t6, monitor_park
    csrw mscratch, t6
    lw t0, 128(t6)
    csrw mepc, t0
    li t0, 0x200080
#ifdef MONITOR_EMULATOR
    li t0, 0x201880
#endif
    csrw mstatus, t0
    fence.i
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

/* Periodic local timers guarantee progress even if a completion arrives just
 * before WFI. Idle interrupts reenter through the same stack-free trap gate. */
monitor_park:
    csrw mscratch, zero
    la t0, monitor_idle
    csrw mepc, t0
    li t0, 0x201880
    csrw mstatus, t0
    mret
monitor_idle:
    wfi
    j monitor_idle

.global monitor_core_entry
.type monitor_core_entry,@function
monitor_core_entry:
    csrci mstatus, 8
    csrw mie, zero
    fence.i
    la t0, monitor_vectors
    ori t0, t0, 1
    csrw mtvec, t0
    li a1, 0
    j monitor_gate

kernel_fault_entry:
    csrci mstatus, 8
    csrw mie, zero
    csrw mscratch, zero
    csrr a0, mepc
    csrr a1, mcause
    la t0, monitor_panic_halt
    csrw mtvec, t0

    /* Only the first fault owns diagnostics and recovery. Another faulting
     * core follows the same acknowledgment path as an ordinary peer trap. */
    li t0, CORE_ID
    lw t2, 0(t0)
    addi t2, t2, 1
    la t0, monitor_panic_owner
1:
    lr.w.aq t1, (t0)
    bnez t1, monitor_panic_park
    sc.w.rl t1, t2, (t0)
    bnez t1, 1b

    /* A peer acknowledgment follows its last memory access. Until it arrives,
     * guest RAM remains live and SP is untouched; watchdog expiry is recovery. */
    la t0, monitor_cores_started
    lw t3, 0(t0)
    not t2, t2
    and t3, t3, t2
    la t0, monitor_panic_parked
2:
    lw t1, 0(t0)
    fence r, rw
    and t1, t1, t3
    bne t1, t3, 2b
    la sp, __panic_stack_top
    la gp, __global_pointer$
    li tp, 0
    call monitor_kernel_fault
    j monitor_panic_halt

/* A parked peer never returns to C or guest code. Publishing with release
 * ordering drains its earlier accesses before the owner reuses guest RAM. */
monitor_panic_park:
    csrci mstatus, 8
    csrw mie, zero
    csrw mscratch, zero
    la t0, monitor_panic_halt
    csrw mtvec, t0
    li t0, CORE_ID
    lw t1, 0(t0)
    li t2, 1
    sll t1, t2, t1
    la t0, monitor_panic_parked
    amoor.w.rl zero, t1, (t0)

/* Nested faults and unexpected recovery returns cannot claim a stack or
 * overwrite diagnostics. The watchdog remains the final recovery mechanism. */
.balign 4
monitor_panic_halt:
    csrci mstatus, 8
    csrw mie, zero
1:
    wfi
    j 1b

/* SDK IRQ registration indexes its software table relative to mtvec. */
.section .bss
.balign 64
.global monitor_vectors
monitor_vectors:
    .space 0xd0 + 52 * 4
