#ifndef MONITOR_ELF_H
#define MONITOR_ELF_H
#include "monitor.h"

/* Decode a complete bounded header prefix without touching physical memory. */
const char *elf_validate(const uint8_t *prefix, uint32_t size, Image *image);
#endif
