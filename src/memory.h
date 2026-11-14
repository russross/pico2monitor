#ifndef MONITOR_MEMORY_H
#define MONITOR_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Physical linkage stays stable while runtime allocations follow the image. */
#define GUEST_BASE UINT32_C(0x20000000)
#define MAIN_SRAM_END UINT32_C(0x20080000)
#define GUEST_END UINT32_C(0x2007c000)
#define PMP_ENTRIES 7u
#define MAX_SEGMENTS 16u
#define HEAP_RESERVE UINT32_C(0x8000)
#define STACK_SIZE UINT32_C(0x4000)
#define ELF_PREFIX_SIZE 4096u
#define ELF_FILE_LIMIT (1024u * 1024u)
#define IO_CHUNK 256u

typedef enum { PERM_NONE = 0, PERM_X = 1, PERM_W = 2, PERM_R = 4 } Permission;
typedef enum { SNAP_AUTO, SNAP_OFF } SnapshotPolicy;
typedef struct {
    uint32_t base, size;
    uint8_t permissions;
} PmpRegion;
typedef struct {
    uint32_t heap_base, heap_limit, stack_base, stack_top, snapshot_base, snapshot_size;
    PmpRegion regions[PMP_ENTRIES];
    uint32_t region_count;
    SnapshotPolicy requested;
    bool snapshot;
} MemoryPlan;

/* Native tests supply the same physical-address contract using an array. */
#ifdef MONITOR_NATIVE_TEST
uint8_t *physical(uint32_t address);
#else
static inline uint8_t *physical(uint32_t address) {
    return (uint8_t *)(uintptr_t)address;
}
#endif

static inline bool range_contains(uint32_t base, uint32_t size, uint32_t address, uint32_t count) {
    return address >= base && address - base <= size && count <= size - (address - base);
}

#endif
