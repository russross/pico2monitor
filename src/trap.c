#include "monitor.h"

#include "hardware/irq.h"
#include "hardware/regs/rvcsr.h"
#include "hardware/riscv_platform_timer.h"
#include "hardware/structs/watchdog.h"
#include "pico/platform.h"

#include <string.h>

extern uint32_t monitor_vectors[];
extern void monitor_trap_entry(void);

_Static_assert(offsetof(GuestFrame, pc) == 128 && offsetof(GuestFrame, cause) == 132 &&
                   offsetof(GuestFrame, status) == 136,
               "guest frame must match entry.S register offsets");

void monitor_trap_init(void) {
    uint32_t original;
    __asm volatile("csrr %0, mtvec" : "=r"(original));
    memcpy(monitor_vectors, (const void *)(uintptr_t)(original & ~3u), (0x34 + 52) * 4);

    /* Each hardware vector is a four-byte relative JAL with no compressed slots. */
    const unsigned slots[] = {0, 3, 7, 11};
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t *slot = &monitor_vectors[slots[i]];
        intptr_t delta = (intptr_t)(uintptr_t)monitor_trap_entry - (intptr_t)(uintptr_t)slot;
        if (delta < -1048576 || delta >= 1048576 || (delta & 1))
            monitor_panic("trap vector is out of JAL range");
        uint32_t d = (uint32_t)delta;
        *slot = 0x6fu | ((d >> 20) & 1) << 31 | ((d >> 1) & 0x3ff) << 21 | ((d >> 11) & 1) << 20 |
                ((d >> 12) & 0xff) << 12;
    }
    monitor_fence_i();
    uintptr_t vector = (uintptr_t)monitor_vectors | 1u;
    __asm volatile("csrw mtvec, %0\ncsrw mscratch, zero" ::"r"(vector) : "memory");
}

void monitor_service_irqs(void) {
    uint32_t context = RVCSR_MEICONTEXT_NOIRQ_BITS;
    __asm volatile("csrw 0xbe5, %0" ::"r"(context) : "memory");
    for (unsigned attempts = 0; attempts < 8; ++attempts) {
        int32_t next;
        __asm volatile("csrrsi %0, 0xbe4, 1" : "=r"(next)::"memory");
        if (next < 0)
            break;
        uint32_t number = (uint32_t)next >> 2;
        if (number == USBCTRL_IRQ) {
            irq_handler_t handler = irq_get_vtable_handler(number);
            handler();
        }
#ifdef MONITOR_EMULATOR
        else if (number == SIO_IRQ_MTIMECMP) {
            riscv_timer_set_mtimecmp(monitor_time_us() + 1000);
        }
#endif
        else {
            irq_set_enabled(number, false);
            shell_irq_masked(number);
        }
    }
    __asm volatile("csrw 0xbe5, %0" ::"r"(context) : "memory");
    uint64_t now = monitor_time_us();
    riscv_timer_set_mtimecmp(now + 1000);
}

void monitor_kernel_fault(uint32_t pc, uint32_t cause) {
    /* A watchdog reset goes to ROM BOOTSEL rather than repeating a faulty monitor boot. */
    watchdog_hw->scratch[0] = pc;
    watchdog_hw->scratch[1] = cause;
    monitor_panic("machine-mode trap");
}
