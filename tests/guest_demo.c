#include "guest.h"

static volatile uint32_t initialized = 7;
static volatile uint32_t zeroed;

int main(void) {
#if DEMO == 1
    guest_puts("hello from SRAM: data=");
    guest_number(initialized);
    guest_puts(" bss=");
    guest_number(zeroed);
    guest_puts("\n");
    if (initialized != 7 || zeroed != 0)
        return 10;
    initialized = 99;
    zeroed = 123;
    guest_sleep(5);
    if (guest_write((const void *)0x10000000, 1) != -14)
        return 11;
    if (guest_read((void *)0x20020000, 1) != -14)
        return 12;
    if (guest_write((const void *)0xfffffff0, 64) != -14)
        return 13;
    guest_puts("syscalls and reset state OK\n");
    return 0;
#elif DEMO == 2
    (void)initialized;
    (void)zeroed;
    guest_puts("echo: Ctrl-C or Escape returns to the monitor\n");
    for (;;) {
        char bytes[80];
        int32_t count = guest_read(bytes, sizeof(bytes));
        if (count > 0)
            (void)guest_write(bytes, (uint32_t)count);
        else if (count == 0)
            guest_puts("EOF\n");
    }
#elif DEMO == 3
    (void)initialized;
    (void)zeroed;
    guest_puts("busy loop: no syscalls; Ctrl-C must still work\n");
    for (;;)
        __asm volatile("" ::: "memory");
#else
    (void)initialized;
    (void)zeroed;
    return 23;
#endif
}
