#include "monitor.h"

typedef enum {
    MAP_OFF,
    MAP_TITLE,
    MAP_LEGEND,
    MAP_COLUMNS,
    MAP_RANGES,
    MAP_RESET,
    MAP_WARNING,
    MAP_PMP_TITLE,
    MAP_PMP,
    MAP_MONITOR
} MapStage;
static struct {
    MapStage stage;
    uint32_t cursor;
} map;

bool memory_map_busy(void) {
    return map.stage != MAP_OFF;
}
void memory_map_start(void) {
    map.stage = program.valid ? MAP_TITLE : MAP_OFF;
    map.cursor = GUEST_BASE;
}

/* Split at every allocation, ELF, and PMP boundary without retaining a second
 * copy of the map. Overlapping ELF segments contribute their union of access. */
static void boundary(uint32_t address, uint32_t candidate, uint32_t *end) {
    if (candidate > address && candidate < *end)
        *end = candidate;
}

static void range_row(void) {
    const Image *image = &program.image;
    const MemoryPlan *p = &image->plan;
    uint32_t address = map.cursor, end = MAIN_SRAM_END;
    uint8_t required = 0;
    const char *use = "unused";
    for (uint32_t i = 0; i < image->segment_count; ++i) {
        const Segment *s = &image->segments[i];
        boundary(address, s->address, &end);
        boundary(address, s->address + s->memory_size, &end);
        if (range_contains(s->address, s->memory_size, address, 1)) {
            use = "ELF";
            required |= s->permissions;
        }
    }

    /* The logical break divides allocated heap from capacity. PMP maps both;
     * syscall buffer checks additionally enforce the logical break. */
    boundary(address, p->heap_base, &end);
    boundary(address, program.current_break, &end);
    boundary(address, p->heap_limit, &end);
    boundary(address, p->stack_base, &end);
    boundary(address, p->stack_top, &end);
    boundary(address, GUEST_END, &end);
    if (address >= GUEST_END)
        use = "monitor";
    else if (address >= p->snapshot_base)
        use = "reset reserve";
    else if (address >= p->stack_base) {
        use = "stack";
        required = PERM_R | PERM_W;
    } else if (address >= p->heap_limit)
        use = "stack guard";
    else if (address >= p->heap_base) {
        use = address < program.current_break ? "heap used" : "heap capacity";
        required = PERM_R | PERM_W;
    }

    uint32_t entry = PMP_ENTRIES;
    for (uint32_t i = 0; i < p->region_count; ++i) {
        const PmpRegion *r = &p->regions[i];
        boundary(address, r->base, &end);
        boundary(address, r->base + r->size, &end);
        if (entry == PMP_ENTRIES && range_contains(r->base, r->size, address, 1))
            entry = i;
    }
    uint8_t effective = entry == PMP_ENTRIES ? 0 : p->regions[entry].permissions;
    console_printf("%08lx..%08lx  %-13s  %c%c%c       %c%c%c        %lu\r\n",
                   (unsigned long)address, (unsigned long)end, use, required & PERM_R ? 'r' : '-',
                   required & PERM_W ? 'w' : '-', required & PERM_X ? 'x' : '-',
                   effective & PERM_R ? 'r' : '-', effective & PERM_W ? 'w' : '-',
                   effective & PERM_X ? 'x' : '-', (unsigned long)entry);
    map.cursor = end;
}

/* Each pass emits bounded output. The prompt waits until every section drains,
 * while USB service and cancellation continue between rows. */
void memory_map_step(void) {
    if (!memory_map_busy() || console_space() < 512)
        return;
    const MemoryPlan *p = &program.image.plan;
    switch (map.stage) {
    case MAP_TITLE:
        console_printf("Guest memory map (end addresses exclusive)\r\n");
        ++map.stage;
        break;
    case MAP_LEGEND:
        console_printf("Required = ELF/runtime needs; effective = first matching PMP entry.\r\n"
                       "Effective --- denies access; extra permissions are relaxed protection.\r\n"
                       "Heap capacity is mapped; syscall buffers are limited by brk=%08lx.\r\n",
                       (unsigned long)program.current_break);
#ifdef MONITOR_EMULATOR
        console_printf("EMULATOR: planned permissions below are NOT enforced.\r\n");
#endif
        ++map.stage;
        break;
    case MAP_COLUMNS:
        console_printf("Address range       Use            Required  Effective  PMP\r\n");
        ++map.stage;
        break;
    case MAP_RANGES:
        range_row();
        if (map.cursor == MAIN_SRAM_END)
            ++map.stage;
        break;
    case MAP_RESET:
        console_printf("Reset: %s (requested %s); saved data=%lu, reserved=%lu bytes.\r\n",
                       p->snapshot ? "available" : "unavailable",
                       p->requested == SNAP_OFF ? "off" : "auto", (unsigned long)p->snapshot_size,
                       (unsigned long)(GUEST_END - p->snapshot_base));
        ++map.stage;
        break;
    case MAP_WARNING:
        if (!p->snapshot && p->requested != SNAP_OFF)
            console_printf("Reset disabled: insufficient usable memory/PMP headroom.\r\n");
        if (p->heap_limit - p->heap_base < HEAP_RESERVE)
            console_printf("warning: heap capacity below 32 KiB\r\n");
        ++map.stage;
        break;
    case MAP_PMP_TITLE:
        console_printf("PMP plan installed on guest entry (lower entry wins; overlaps allowed)\r\n"
                       "Entry  Address range       Access  Mode   pmpaddr\r\n");
        map.cursor = 0;
        ++map.stage;
        break;
    case MAP_PMP: {
        uint32_t i = map.cursor++;
        if (i < p->region_count) {
            const PmpRegion *r = &p->regions[i];
            console_printf("PMP %lu  %08lx..%08lx  %c%c%c     NAPOT  %08lx\r\n", (unsigned long)i,
                           (unsigned long)r->base, (unsigned long)(r->base + r->size),
                           r->permissions & PERM_R ? 'r' : '-', r->permissions & PERM_W ? 'w' : '-',
                           r->permissions & PERM_X ? 'x' : '-',
                           (unsigned long)((r->base >> 2) | (r->size / 8 - 1)));
        } else if (i < PMP_ENTRIES)
            console_printf("PMP %lu  (unused)                     OFF    00000000\r\n",
                           (unsigned long)i);
        else {
            console_printf("PMP 7  all addresses        ---     NAPOT  3fffffff (system deny)\r\n");
            ++map.stage;
        }
        break;
    }
    case MAP_MONITOR:
        monitor_memory_report();
        map.stage = MAP_OFF;
        break;
    case MAP_OFF:
        break;
    }
}
