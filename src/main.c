#include "loader.h"
#include "monitor.h"
#include "transfer.h"

#include "hardware/irq.h"
#include "hardware/riscv_platform_timer.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/platform.h"

extern uint32_t __StackBottom[], __StackTop[], __bss_end__[];
static uint32_t *paint_end;

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
                   (unsigned long)((uintptr_t)__bss_end__ - 0x20000000),
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
    for (;;) {
        monitor_service_irqs();
        io_service();
        transfer_step();
        loader_step();
        shell_step();
#ifndef MONITOR_EMULATOR
        watchdog_update();
#endif
        program_step();
        if (program.state != RUNNING && !loader_busy() && !program.reset_busy)
            __asm volatile("wfi");
    }
}
