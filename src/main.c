#include "loader.h"
#include "monitor.h"
#include "transfer.h"

#include "hardware/irq.h"
#include "hardware/riscv_platform_timer.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/platform.h"

#include <stdatomic.h>

extern uint32_t __StackBottom[], __StackTop[], __bss_end__[];
extern uint32_t monitor_vectors[];
static uint32_t *paint_end;
_Atomic uint32_t monitor_gate_lock;
_Atomic uint32_t monitor_panic_owner, monitor_panic_parked;
_Atomic uint32_t monitor_cores_started = 1;

/* Paint only below the live main frame. SDK startup has already returned, so
 * this measures monitor service paths rather than boot ROM or SDK startup. */
static void stack_measure_init(void) {
    uintptr_t sp;
    __asm volatile("mv %0, sp" : "=r"(sp));
    paint_end = (uint32_t *)((sp - 128) & ~(uintptr_t)3);
    for (uint32_t *p = __StackBottom; p < paint_end; ++p)
        *p = 0x57acced1;
}

void monitor_memory_report(void) {
    uint32_t *p = __StackBottom;
    while (p < paint_end && *p == 0x57acced1)
        ++p;
    console_printf("monitor: static=%lu stack-used-at-least=%lu stack-reserved=%lu bytes\r\n",
                   (unsigned long)((uintptr_t)__bss_end__ - GUEST_END),
                   (unsigned long)((uintptr_t)__StackTop - (uintptr_t)p),
                   (unsigned long)((uintptr_t)__StackTop - (uintptr_t)__StackBottom));
}

uint64_t monitor_time_us(void) {
    return riscv_timer_get_mtime();
}
void monitor_fence_i(void) {
    __asm volatile("fence.i" ::: "memory");
}

void monitor_bootsel(void) {
    __asm volatile("csrci mstatus, 8" ::: "memory");
    reset_usb_boot(0, 0);
    __builtin_unreachable();
}
void monitor_panic(const char *message) {
    (void)message;
    monitor_bootsel();
}

/* Only the owning core acknowledges its saved guest frame. Everything after
 * that acknowledgment is ordinary sequential monitor state-machine work. */
GuestFrame *monitor_dispatch(unsigned core, bool guest_trap) {
    static bool initialized[2] = {true, false};
    if (!initialized[core]) {
        for (unsigned irq = 0; irq < NUM_IRQS; ++irq)
            irq_set_enabled(irq, false);
        pmp_core_init();
#ifdef MONITOR_EMULATOR
        irq_set_enabled(SIO_IRQ_MTIMECMP, true);
#endif
        __asm volatile("csrw mie, %0" ::"r"(0x880u) : "memory");
        initialized[core] = true;
    }
    pmp_disable();
    if (guest_trap)
        program_after_trap(core);
    monitor_service_irqs();
    io_service();
    transfer_step();
    loader_step();
    program_step();
    shell_step();
#ifndef MONITOR_EMULATOR
    watchdog_update();
#endif
    GuestThread *thread = &program.threads[core];
    if (program.state != ACTIVE || program.reset_busy || thread->state != THREAD_RUNNABLE)
        return NULL;
    pmp_enable(&program.image.plan);
    thread->entered_at = monitor_time_us();
    thread->executing = true;
    return &thread->frame;
}

/* The ROM launch handshake supplies only registers and an assembly entry PC.
 * It runs during boot, before either core can enter the monitor state machine. */
static void launch_core1(void) {
    const uint32_t commands[] = {0,
                                 0,
                                 1,
                                 (uint32_t)(uintptr_t)monitor_vectors | 1u,
                                 (uint32_t)(uintptr_t)__StackTop,
                                 (uint32_t)(uintptr_t)monitor_core_entry};
    unsigned cursor = 0;
    while (cursor < sizeof(commands) / sizeof(commands[0])) {
        uint32_t command = commands[cursor];
        if (!command) {
            while (sio_hw->fifo_st & SIO_FIFO_ST_VLD_BITS)
                (void)sio_hw->fifo_rd;
            __sev();
        }
        while (!(sio_hw->fifo_st & SIO_FIFO_ST_RDY_BITS))
            tight_loop_contents();
        sio_hw->fifo_wr = command;
        __sev();
        while (!(sio_hw->fifo_st & SIO_FIFO_ST_VLD_BITS))
            tight_loop_contents();
        cursor = sio_hw->fifo_rd == command ? cursor + 1 : 0;
    }
}

int main(void) {
    __asm volatile("csrci mstatus, 8" ::: "memory");
    stack_measure_init();
#ifndef MONITOR_EMULATOR
    /* A wedged kernel returns to the ROM programmer on its first watchdog reboot. */
    if (watchdog_enable_caused_reboot())
        monitor_bootsel();
    watchdog_enable(3000, true);
#endif
    monitor_trap_init();
    for (unsigned irq = 0; irq < NUM_IRQS; ++irq)
        irq_set_enabled(irq, false);
    pmp_init();
    program_forget();
    riscv_timer_set_fullspeed(false);
    riscv_timer_set_enabled(true);
    riscv_timer_set_mtimecmp(monitor_time_us() + 1000);
    io_init();
    __asm volatile("csrw mie, %0" ::"r"(0x880u) : "memory");
#ifdef MONITOR_EMULATOR
    /* The emulator exposes mtimecmp only through its external IRQ alias. */
    irq_set_enabled(SIO_IRQ_MTIMECMP, true);
#endif
    /* Boot owns the shared stack until assembly abandons its continuation.
     * Core 1 starts directly in assembly and never uses the supplied boot SP. */
    atomic_store_explicit(&monitor_gate_lock, 1, memory_order_release);
    /* Include a launching peer before ROM can release it. A panic during the
     * handshake waits for that peer or falls back to watchdog recovery. */
    atomic_store_explicit(&monitor_cores_started, 3, memory_order_release);
    launch_core1();
    monitor_start_locked();
}
