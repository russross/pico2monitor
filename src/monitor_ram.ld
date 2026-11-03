/* Temporary diagnostics and the protected monitor can run entirely from SRAM. */
RAM_LENGTH = 120K;
SCRATCH_X_ORIGIN = 0x2001e000;
SCRATCH_X_LENGTH = 0;
SCRATCH_Y_ORIGIN = 0x2001e000;
SCRATCH_Y_LENGTH = 8K;
INCLUDE "memmap_no_flash.incl"
__emergency_stack_top = 0x20082000;
ASSERT(__HeapLimit <= 0x2001e000, "RAM monitor overlaps its stack")
