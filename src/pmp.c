#include "monitor.h"

#include "hardware/regs/rvcsr.h"
#include "hardware/structs/accessctrl.h"
#ifdef MONITOR_PMP_TRACE
#include "hardware/structs/watchdog.h"
#define PMP_TRACE(value) (watchdog_hw->scratch[0] = (value))
#else
#define PMP_TRACE(value) ((void)0)
#endif

#ifndef MONITOR_EMULATOR
static uint32_t napot(uint32_t base, uint32_t size) {
    return (base >> 2) | (size / 8 - 1);
}
#endif

void pmp_disable(void) {
#ifndef MONITOR_EMULATOR
    __asm volatile("csrw pmpcfg0, zero" ::: "memory");
    uint32_t deny = 0x18000000;
    __asm volatile("csrw pmpcfg1, %0" ::"r"(deny) : "memory");
#endif
}

void pmp_init(void) {
#ifndef MONITOR_EMULATOR
    PMP_TRACE(1);
    pmp_disable();
    PMP_TRACE(2);
    uint32_t deny = UINT32_C(0x3fffffff), napot_config = 3u << 27;
    __asm volatile("csrw pmpcfg1, zero" ::: "memory");
    PMP_TRACE(3);
    __asm volatile("csrw pmpaddr7, %0" ::"r"(deny) : "memory");
    PMP_TRACE(4);
    __asm volatile("csrw pmpcfg1, %0" ::"r"(napot_config) : "memory");
    PMP_TRACE(5);
    __asm volatile("csrw 0xbd0, zero\ncsrw mcounteren, zero" ::: "memory");
    PMP_TRACE(6);

    /* ACCESSCTRL requires its write key even for read/modify/write operations.
     * Shared SRAM banks stay NSU-accessible; PMP selects the guest's addresses. */
    const uint32_t access_key = UINT32_C(0xacce0000);
    accessctrl_hw->rom = access_key | (accessctrl_hw->rom & ~1u);
    PMP_TRACE(7);
    accessctrl_hw->xip_main = access_key | (accessctrl_hw->xip_main & ~1u);
    PMP_TRACE(8);
    for (unsigned i = 0; i < 10; ++i)
        accessctrl_hw->sram[i] = access_key | accessctrl_hw->sram[i] | 3u;
    for (uint32_t offset = ACCESSCTRL_DMA_OFFSET; offset <= ACCESSCTRL_XIP_AUX_OFFSET;
         offset += 4) {
        PMP_TRACE(0x100 + offset);
        volatile uint32_t *permission = (volatile uint32_t *)((uintptr_t)accessctrl_hw + offset);
        *permission = access_key | (*permission & ~1u);
    }
    PMP_TRACE(9);
#endif
}

void pmp_enable(const MemoryPlan *plan) {
#ifndef MONITOR_EMULATOR
    uint32_t addresses[7] = {0}, config[2] = {0, 0x18000000};
    for (uint32_t i = 0; i < plan->region_count; ++i) {
        const PmpRegion *r = &plan->regions[i];
        addresses[i] = napot(r->base, r->size);
        uint32_t permissions = 0x18;
        if (r->permissions & PERM_R)
            permissions |= RVCSR_PMPCFG0_R0_R_BITS;
        if (r->permissions & PERM_W)
            permissions |= RVCSR_PMPCFG0_R0_R_BITS | RVCSR_PMPCFG0_R0_W_BITS;
        if (r->permissions & PERM_X)
            permissions |= RVCSR_PMPCFG0_R0_X_BITS;
        config[i / 4] |= permissions << ((i % 4) * 8);
    }
    /* All mutable grants are revoked before addresses change. Entry seven
     * continuously overrides the hardwired ROM and peripheral grants. */
    pmp_disable();
    __asm volatile("csrw pmpaddr0, %0\ncsrw pmpaddr1, %1\ncsrw pmpaddr2, %2\n"
                   "csrw pmpaddr3, %3\ncsrw pmpaddr4, %4\ncsrw pmpaddr5, %5\n"
                   "csrw pmpaddr6, %6\ncsrw pmpcfg0, %7\ncsrw pmpcfg1, %8" ::"r"(addresses[0]),
                   "r"(addresses[1]), "r"(addresses[2]), "r"(addresses[3]), "r"(addresses[4]),
                   "r"(addresses[5]), "r"(addresses[6]), "r"(config[0]), "r"(config[1])
                   : "memory");
#else
    (void)plan;
#endif
}
