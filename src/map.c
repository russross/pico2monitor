#include "monitor.h"

static uint32_t row;
static bool active;

bool memory_map_busy(void) {
    return active;
}
void memory_map_start(void) {
    row = 0;
    active = program.valid;
}

/* One bounded row per event-loop pass keeps large maps out of the output ring
 * and leaves room for USB servicing, cancellation, and transfer replies. */
void memory_map_step(void) {
    if (!active || console_space() < 256)
        return;
    const Image *image = &program.image;
    const MemoryPlan *p = &image->plan;
    uint32_t current = row++;
    if (!current) {
        monitor_memory_report();
        console_printf("memory: guest %08lx..%08lx; PMP %lu/7 (+ system deny)\r\n",
                       (unsigned long)GUEST_BASE, (unsigned long)p->snapshot_base,
                       (unsigned long)p->region_count);
    } else if (current <= image->segment_count) {
        const Segment *s = &image->segments[current - 1];
        console_printf("ELF %08lx..%08lx %c%c%c file=%lu\r\n", (unsigned long)s->address,
                       (unsigned long)(s->address + s->memory_size),
                       s->permissions & PERM_R ? 'r' : '-', s->permissions & PERM_W ? 'w' : '-',
                       s->permissions & PERM_X ? 'x' : '-', (unsigned long)s->file_size);
    } else if (current == image->segment_count + 1) {
        console_printf("heap %08lx..%08lx brk=%08lx; stack %08lx..%08lx\r\n",
                       (unsigned long)p->heap_base, (unsigned long)p->heap_limit,
                       (unsigned long)program.current_break, (unsigned long)p->stack_base,
                       (unsigned long)p->stack_top);
    } else if (current == image->segment_count + 2) {
        static const char *const names[] = {"auto", "off"};
        console_printf("reset-data requested=%s effective=%s snapshot=%lu reserved=%lu\r\n",
                       names[p->requested], p->snapshot ? "on" : "off",
                       (unsigned long)p->snapshot_size,
                       (unsigned long)(GUEST_END - p->snapshot_base));
        if (!p->snapshot && p->requested != SNAP_OFF)
            console_printf("reset-data disabled: insufficient usable memory/PMP headroom\r\n");
        if (p->heap_limit - p->heap_base < HEAP_RESERVE)
            console_printf("warning: heap capacity below 32 KiB\r\n");
    } else {
        uint32_t index = current - image->segment_count - 3;
        if (index < p->region_count) {
            const PmpRegion *r = &p->regions[index];
            console_printf("PMP %lu %08lx..%08lx %c%c%c (first match wins)\r\n",
                           (unsigned long)index, (unsigned long)r->base,
                           (unsigned long)(r->base + r->size), r->permissions & PERM_R ? 'r' : '-',
                           r->permissions & PERM_W ? 'w' : '-',
                           r->permissions & PERM_X ? 'x' : '-');
        } else
            active = false;
    }
}
