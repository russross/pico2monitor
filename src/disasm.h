#ifndef DISASM_H
#define DISASM_H

#include <stddef.h>
#include <stdint.h>

extern const char *const abi_registers[32];
/* Decode one already-fetched instruction into a complete display row. */
void disassemble(char *line, size_t capacity, uint32_t pc, uint32_t instruction, unsigned size);

#endif
