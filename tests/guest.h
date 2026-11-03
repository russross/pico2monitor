#ifndef MONITOR_GUEST_H
#define MONITOR_GUEST_H
#include <stddef.h>
#include <stdint.h>

/* This small ABI has no dependency on a hosted C library or Pico SDK. */
int32_t guest_read(void *buffer, uint32_t count);
int32_t guest_write(const void *buffer, uint32_t count);
void guest_puts(const char *text);
void guest_number(uint32_t value);
void guest_sleep(uint32_t milliseconds);
uint32_t guest_uptime(void);
void guest_yield(void);
uint32_t guest_brk(uint32_t address);
#endif
