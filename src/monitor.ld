/* The SDK owns startup and section placement; only its physical memory limits change. */
RAM_LENGTH = 88K;
SCRATCH_X_ORIGIN = 0x20016000;
SCRATCH_X_LENGTH = 0;
SCRATCH_Y_ORIGIN = 0x20016000;
SCRATCH_Y_LENGTH = 8K;
INCLUDE "memmap_default.incl"
__emergency_stack_top = 0x20082000;
ASSERT(__HeapLimit <= 0x20016000, "monitor data overlaps its stack")
ASSERT(__StackTop == 0x20018000, "monitor stack overlaps guest arena")
