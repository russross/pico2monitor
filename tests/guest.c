#include "guest.h"
#include "syscall.h"

static int32_t call(uint32_t number, uint32_t first, uint32_t second, uint32_t third) {
    register uint32_t a0 __asm("a0") = first;
    register uint32_t a1 __asm("a1") = second;
    register uint32_t a2 __asm("a2") = third;
    register uint32_t a7 __asm("a7") = number;
    __asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return (int32_t)a0;
}
int32_t guest_read(void *buffer, uint32_t count) {
    return call(SYS_READ, 0, (uint32_t)(uintptr_t)buffer, count);
}
int32_t guest_write(const void *buffer, uint32_t count) {
    return call(SYS_WRITE, 1, (uint32_t)(uintptr_t)buffer, count);
}
void guest_sleep(uint32_t milliseconds) {
    int64_t times[2] = {milliseconds / 1000, (int64_t)(milliseconds % 1000) * 1000000};
    (void)call(SYS_CLOCK_NANOSLEEP64, 1, 0, (uint32_t)(uintptr_t)times);
}
uint32_t guest_uptime(void) {
    int64_t times[2] = {0};
    (void)call(SYS_CLOCK_GETTIME64, 1, (uint32_t)(uintptr_t)times, 0);
    return (uint32_t)times[0] * 1000 + (uint32_t)times[1] / 1000000;
}
void guest_yield(void) {
    (void)call(SYS_YIELD, 0, 0, 0);
}
uint32_t guest_brk(uint32_t address) {
    return (uint32_t)call(SYS_BRK, address, 0, 0);
}

void guest_puts(const char *text) {
    uint32_t length = 0;
    while (text[length])
        ++length;
    while (length) {
        int32_t written = guest_write(text, length);
        if (written <= 0)
            return;
        text += written;
        length -= (uint32_t)written;
    }
}
void guest_number(uint32_t value) {
    char digits[10];
    uint32_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (count)
        (void)guest_write(&digits[--count], 1);
}
