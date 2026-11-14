#ifndef MONITOR_ELF_H
#define MONITOR_ELF_H
#include "monitor.h"

/* Decode a complete bounded header prefix and plan the layout using disposable guest SRAM. */
const char *elf_validate(const uint8_t *prefix, uint32_t size, Image *image);
const char *elf_decode(const uint8_t *prefix, uint32_t size, Image *image);
#endif
