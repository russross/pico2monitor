#include "monitor.h"

#include <string.h>

#define LAUNCH_BYTES 2048u
#define LAUNCH_COUNT 32u
static char environment[LAUNCH_BYTES], updated[LAUNCH_BYTES];
static uint32_t environment_size, environment_count;

static bool valid_name(const char *name) {
    if (!*name)
        return false;
    for (const char *c = name; *c; ++c)
        if (*c == '=' || (unsigned char)*c < 33 || (unsigned char)*c > 126)
            return false;
    return true;
}

/* Environment changes are transactional; a rejected edit retains all entries.
 * The monitor owns these strings even when guest code overwrites its stack. */
const char *environment_set(const char *name, const char *value) {
    if (!valid_name(name))
        return "invalid environment name";
    size_t name_size = strlen(name), used = 0;
    uint32_t count = 0;
    for (uint32_t offset = 0; offset < environment_size;) {
        const char *entry = environment + offset;
        size_t length = strlen(entry) + 1;
        if (!(strncmp(entry, name, name_size) == 0 && entry[name_size] == '=')) {
            memcpy(updated + used, entry, length);
            used += length;
            ++count;
        }
        offset += (uint32_t)length;
    }
    if (value) {
        size_t length = name_size + strlen(value) + 2;
        if (count == LAUNCH_COUNT || length > LAUNCH_BYTES - used)
            return "environment limit exceeded";
        memcpy(updated + used, name, name_size);
        updated[used + name_size] = '=';
        memcpy(updated + used + name_size + 1, value, strlen(value) + 1);
        used += length;
        ++count;
    }
    memcpy(environment, updated, used);
    environment_size = (uint32_t)used;
    environment_count = count;
    return NULL;
}

void environment_print(void) {
    for (uint32_t offset = 0; offset < environment_size;) {
        uint32_t length = (uint32_t)strlen(environment + offset);
        console_write(environment + offset, length);
        console_write("\r\n", 2);
        offset += length + 1;
    }
}

/* Linux-style startup tables use guest pointers, never monitor pointers. The
 * complete request is sized before any guest byte or execution state changes. */
const char *program_launch(unsigned argc, const char *const *argv) {
    if (program.state != READY)
        return "run requires a ready program; use reset first";
    if (argc >= LAUNCH_COUNT)
        return "argument limit exceeded";
    uint32_t sizes = environment_size + (uint32_t)strlen(program.image.name) + 1;
    for (unsigned i = 0; i < argc; ++i) {
        size_t length = strlen(argv[i]) + 1;
        if (length > LAUNCH_BYTES || sizes > LAUNCH_BYTES - length)
            return "argument/environment string limit exceeded";
        sizes += (uint32_t)length;
    }
    if (sizes > LAUNCH_BYTES)
        return "argument/environment string limit exceeded";
    uint32_t phdr = 0;
    for (uint32_t i = 0; i < program.image.segment_count; ++i) {
        const Segment *s = &program.image.segments[i];
        if (range_contains(s->offset, s->file_size, program.image.phoff, program.image.phnum * 32))
            phdr = s->address + program.image.phoff - s->offset;
    }
    uint32_t aux_words = phdr ? 12 : 6;
    uint32_t words = 1 + argc + 2 + environment_count + 1 + aux_words;
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
    for (uint32_t offset = 0; offset < environment_size;) {
        uint32_t length = (uint32_t)strlen(environment + offset) + 1;
        memcpy(physical(cursor), &string_cursor, 4);
        memcpy(physical(string_cursor), environment + offset, length);
        cursor += 4;
        offset += length;
        string_cursor += length;
    }
    memcpy(physical(cursor), &word, 4);
    cursor += 4;
    /* AT_PAGESZ describes the nominal userspace ABI unit, not an MMU mapping. */
    uint32_t aux[] = {6, 4096, 9, program.image.entry, 0, 0, 3, phdr,
                      4, 32,   5, program.image.phnum};
    if (phdr) {
        aux[4] = 3;
        aux[5] = phdr;
        aux[6] = 4;
        aux[7] = 32;
        aux[8] = 5;
        aux[9] = program.image.phnum;
        aux[10] = aux[11] = 0;
    }
    memcpy(physical(cursor), aux, aux_words * 4);
    program.frame.x[2] = sp;
    program.frame.x[10] = argc + 1;
    program.frame.x[11] = argv_address;
    program.frame.x[12] = envp_address;
    program.state = RUNNING;
    guest_input_clear();
    return NULL;
}
