#include "monitor.h"

#include <string.h>

#define LAUNCH_BYTES 2048u
#define LAUNCH_COUNT 32u
/* Linux-style startup tables use guest pointers, never monitor pointers. The
 * complete request is sized before any guest byte or execution state changes. */
const char *program_launch(unsigned argc, const char *const *argv) {
    if (program.state != READY)
        return "run requires a ready program; use reset first";
    if (argc >= LAUNCH_COUNT)
        return "argument limit exceeded";
    uint32_t sizes = (uint32_t)strlen(program.image.name) + 1;
    for (unsigned i = 0; i < argc; ++i) {
        size_t length = strlen(argv[i]) + 1;
        if (length > LAUNCH_BYTES || sizes > LAUNCH_BYTES - length)
            return "argument string limit exceeded";
        sizes += (uint32_t)length;
    }
    if (sizes > LAUNCH_BYTES)
        return "argument string limit exceeded";
    uint32_t phdr = 0;
    for (uint32_t i = 0; i < program.image.segment_count; ++i) {
        const Segment *s = &program.image.segments[i];
        if (range_contains(s->offset, s->file_size, program.image.phoff, program.image.phnum * 32))
            phdr = s->address + program.image.phoff - s->offset;
    }
    uint32_t aux_words = phdr ? 12 : 6;
    uint32_t words = 1 + argc + 2 + 1 + aux_words;
    const MemoryPlan *p = &program.image.plan;
    uint32_t strings = p->stack_top - sizes;
    uint32_t sp = (strings - words * 4) & ~15u;
    if (sp < p->stack_base)
        return "startup arguments do not fit stack";
    uint32_t cursor = sp, string_cursor = strings;
    uint32_t word = argc + 1;
    memcpy(physical(cursor), &word, 4);
    cursor += 4;
    uint32_t argv_address = cursor;
    for (unsigned i = 0; i <= argc; ++i) {
        const char *value = i ? argv[i - 1] : program.image.name;
        size_t length = strlen(value) + 1;
        memcpy(physical(cursor), &string_cursor, 4);
        memcpy(physical(string_cursor), value, length);
        cursor += 4;
        string_cursor += (uint32_t)length;
    }
    word = 0;
    memcpy(physical(cursor), &word, 4);
    cursor += 4;
    uint32_t envp_address = cursor;
    memcpy(physical(cursor), &word, 4);
    cursor += 4;
    /* AT_PAGESZ describes the nominal userspace ABI unit, not an MMU mapping. */
    const uint32_t aux[] = {6, 4096, 9, program.image.entry};
    memcpy(physical(cursor), aux, sizeof(aux));
    cursor += sizeof(aux);
    if (phdr) {
        const uint32_t headers[] = {3, phdr, 4, 32, 5, program.image.phnum};
        memcpy(physical(cursor), headers, sizeof(headers));
        cursor += sizeof(headers);
    }
    const uint32_t end[] = {0, 0};
    memcpy(physical(cursor), end, sizeof(end));
    GuestThread *thread = &program.threads[0];
    thread->frame.x[2] = sp;
    thread->frame.x[10] = argc + 1;
    thread->frame.x[11] = argv_address;
    thread->frame.x[12] = envp_address;
    thread->state = THREAD_RUNNABLE;
    program.state = ACTIVE;
    guest_input_clear();
    return NULL;
}
