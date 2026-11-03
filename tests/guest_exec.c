#include "guest.h"

static volatile uint32_t initialized = 7;
static volatile uint32_t zeroed;

/* Exercise the default linker's compact layout, launch tables, Linux brk, and
 * optional restoration without assuming the old demonstration memory slots. */
int main(int argc, char **argv, char **envp) {
    guest_puts("exec data=");
    guest_number(initialized);
    guest_puts(" bss=");
    guest_number(zeroed);
    guest_puts(" argc=");
    guest_number((uint32_t)argc);
    guest_puts("\n");
    initialized = 99;
    zeroed = 123;
    for (int i = 0; i < argc; ++i) {
        guest_puts("arg: ");
        guest_puts(argv[i]);
        guest_puts("\n");
    }
    for (char **p = envp; *p; ++p) {
        guest_puts("env: ");
        guest_puts(*p);
        guest_puts("\n");
    }
    uint32_t base = guest_brk(0);
    if (guest_brk(base + 64) != base + 64)
        return 20;
    volatile uint8_t *heap = (volatile uint8_t *)(uintptr_t)base;
    for (unsigned i = 0; i < 64; ++i) {
        if (heap[i])
            return 21;
        heap[i] = 0xa5;
    }
    if (guest_brk(base) != base || guest_brk(base + 64) != base + 64)
        return 22;
    for (unsigned i = 0; i < 64; ++i)
        if (heap[i])
            return 23;
    if (guest_brk(UINT32_MAX) != base + 64)
        return 24;
    guest_puts("exec heap OK\n");
    return 0;
}
