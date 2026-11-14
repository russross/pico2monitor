/* The SDK owns startup and section placement; only its physical memory limits change. */
RAM_ORIGIN = 0x2007c000;
RAM_LENGTH = 16K;
SCRATCH_X_ORIGIN = 0x20080000;
SCRATCH_X_LENGTH = 4K;
SCRATCH_Y_ORIGIN = 0x20081000;
SCRATCH_Y_LENGTH = 4K;
INCLUDE "memmap_default.incl"
__panic_stack_top = 0x20001000;
ASSERT(__panic_stack_top <= RAM_ORIGIN, "panic stack overlaps monitor storage")
ASSERT(__HeapLimit <= 0x20080000, "monitor data exceeds main SRAM reservation")
ASSERT(ADDR(.console_output) == 0x20080000 && SIZEOF(.console_output) == 4096,
       "console output must fill scratch bank X")
ASSERT(__StackBottom == 0x20081000, "monitor stack must reserve scratch bank Y")
ASSERT(__StackTop == 0x20082000, "monitor stack exceeds scratch SRAM")
