#include "guest.h"
#include "memory.h"

static volatile uint32_t initialized = 1;

int main(void) {
    if (initialized != 1)
        return 98;
    guest_puts("protection probe ");
    guest_number(PROBE);
    guest_puts("\n");
#if PROBE == 1
    *(volatile uint32_t *)0x20020000 = 0;
#elif PROBE == 2
    return *(volatile uint32_t *)0x10000000;
#elif PROBE == 3
    return *(volatile uint32_t *)0xd0000000;
#elif PROBE == 4
    __asm volatile("csrw mstatus, zero");
#elif PROBE == 5
    ((void (*)(void))(uintptr_t)0x20030000)();
#elif PROBE == 6
    __asm volatile("li sp, 1\n.word 0" ::: "memory");
#elif PROBE == 7
    __asm volatile("wfi");
#elif PROBE == 8
    return *(volatile uint32_t *)0x00000000;
#elif PROBE == 9
    return *(volatile uint32_t *)GUEST_END;
#elif PROBE == 10
    return *(volatile uint32_t *)(GUEST_END - 0x10000);
#elif PROBE == 11
    *(volatile uint32_t *)(GUEST_END - 0x14004) = 0;
#elif PROBE == 12
    *(volatile uint32_t *)0x20040000 = 0;
#endif
    return 99;
}
