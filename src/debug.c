#include "debug.h"
#include "disasm.h"
#include "monitor.h"

#include <string.h>

typedef enum { VIEW_NONE, VIEW_LIST, VIEW_DUMP } View;
static bool active;
static unsigned hart, remaining;
static uint32_t cursor;
static View view, previous;

/* Inspection includes unallocated guest bytes but excludes monitor snapshots.
 * No PMP or logical heap permission is needed for a stopped memory inspection. */
static bool contains(uint32_t address, uint32_t size) {
    return range_contains(GUEST_BASE, program.image.plan.snapshot_base - GUEST_BASE, address, size);
}
static void status(void) {
    static const char *const names[] = {"reset", "stopped", "exited", "faulted"};
    const GuestThread *thread = &program.threads[hart];
    console_printf("** hart%u: pc=%08lx brk=%08lx sp=%08lx .=%08lx %s", hart,
                   (unsigned long)thread->frame.pc, (unsigned long)program.current_break,
                   (unsigned long)thread->frame.x[2], (unsigned long)cursor,
                   names[thread->debug_status]);
    if (program.threads[hart ^ 1u].present)
        console_printf(" (hart%u %s)", hart ^ 1u, names[program.threads[hart ^ 1u].debug_status]);
    console_printf("\r\n");
    if (!contains(cursor, 1))
        console_printf(". is out of range\r\n");
}
bool debug_enter(void) {
    if (!program.valid) {
        console_printf("no loaded program\r\n");
        return false;
    }
    hart = program.threads[1].present && program.threads[1].state != THREAD_UNUSED &&
                   program.threads[0].state == THREAD_UNUSED
               ? 1
               : 0;
    cursor = program.threads[hart].frame.pc;
    active = true;
    previous = view = VIEW_NONE;
    remaining = 0;
    status();
    return true;
}
void debug_leave(void) {
    active = false;
    remaining = 0;
    previous = view = VIEW_NONE;
}
bool debug_active(void) {
    return active;
}
bool debug_busy(void) {
    return remaining != 0;
}

/* Register names take precedence over hexadecimal numbers such as a0.
 * ELF writable loads provide the data heuristic; heap is the fallback. */
static bool address_value(const char *text, uint32_t *value) {
    const GuestFrame *frame = &program.threads[hart].frame;
    for (unsigned i = 0; i < 32; ++i)
        if (!strcmp(text, abi_registers[i]) || (i == 8 && !strcmp(text, "fp"))) {
            *value = frame->x[i];
            return true;
        }
    const MemoryPlan *p = &program.image.plan;
    if (!strcmp(text, "pc"))
        *value = frame->pc;
    else if (!strcmp(text, "entry"))
        *value = program.image.entry;
    else if (!strcmp(text, "heap"))
        *value = p->heap_base;
    else if (!strcmp(text, "brk"))
        *value = program.current_break;
    else if (!strcmp(text, "stack"))
        *value = p->stack_base;
    else if (!strcmp(text, "top"))
        *value = p->stack_top;
    else if (!strcmp(text, "data")) {
        *value = p->heap_base;
        for (uint32_t i = 0; i < program.image.segment_count; ++i) {
            const Segment *s = &program.image.segments[i];
            if ((s->permissions & PERM_W) && s->memory_size && s->address < *value)
                *value = s->address;
        }
    } else {
        const char *digits = text;
        if (digits[0] == '0' && digits[1] == 'x')
            digits += 2;
        size_t count = strlen(digits);
        if (!count || count > 8)
            return false;
        uint32_t address = 0;
        for (size_t i = 0; i < count; ++i) {
            unsigned digit = (unsigned char)digits[i];
            if (digit >= '0' && digit <= '9')
                digit -= '0';
            else if (digit >= 'a' && digit <= 'f')
                digit -= 'a' - 10;
            else if (digit >= 'A' && digit <= 'F')
                digit -= 'A' - 10;
            else
                return false;
            address = (address << 4) | digit;
        }
        *value = address;
    }
    return true;
}
void debug_command(char *command) {
    while (*command == ' ' || *command == '\t')
        ++command;
    size_t size = strlen(command);
    while (size && (command[size - 1] == ' ' || command[size - 1] == '\t'))
        command[--size] = 0;
    if (!strcmp(command, "list") || !strcmp(command, "dump") ||
        (!*command && previous != VIEW_NONE)) {
        view = !*command ? previous : !strcmp(command, "list") ? VIEW_LIST : VIEW_DUMP;
        previous = view;
        remaining = 20;
        return;
    }
    previous = VIEW_NONE;
    if (!strcmp(command, "hart0") || !strcmp(command, "hart1")) {
        unsigned selected = (unsigned)(command[4] - '0');
        if (!program.threads[selected].present)
            console_printf("hart%u has no saved state\r\n", selected);
        else {
            hart = selected;
            status();
        }
    } else if (*command == '.') {
        ++command;
        while (*command == ' ' || *command == '\t')
            ++command;
        if (*command) {
            uint32_t address;
            if (!address_value(command, &address)) {
                console_printf("invalid address or register\r\n");
                status();
                return;
            }
            cursor = address <= 520u * 1024 ? GUEST_BASE + address : address;
        }
        status();
    } else if (*command)
        console_printf("unknown debug command: %s\r\n", command);
}

/* Rows are emitted one per dispatch, below the shell's output fence. Partial
 * first and last dump rows keep both hex and ASCII columns at fixed positions. */
static void dump_row(void) {
    unsigned first = cursor & 15, count = 16 - first;
    uint32_t end = program.image.plan.snapshot_base;
    if (count > end - cursor)
        count = end - cursor;
    char row[100], *out = row;
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; ++i) {
        if (i == 8)
            *out++ = ' ';
        if (i >= first && i < first + count) {
            uint8_t b = *physical(cursor + i - first);
            *out++ = hex[b >> 4];
            *out++ = hex[b & 15];
        } else {
            *out++ = ' ';
            *out++ = ' ';
        }
        *out++ = ' ';
    }
    *out++ = '|';
    *out++ = ' ';
    for (unsigned i = 0; i < 16; ++i) {
        if (i == 8)
            *out++ = ' ';
        if (i >= first && i < first + count) {
            uint8_t b = *physical(cursor + i - first);
            *out++ = b >= 32 && b <= 126 ? (char)b : '.';
        } else
            *out++ = ' ';
    }
    *out = 0;
    console_printf("0x%08lx: %s\r\n", (unsigned long)cursor, row);
    cursor += count;
}
void debug_step(void) {
    if (!remaining)
        return;
    --remaining;
    if (view == VIEW_LIST && (cursor & 1)) {
        console_printf("0x%08lx: not aligned\r\n", (unsigned long)cursor);
        remaining = 0;
    } else if (!contains(cursor, view == VIEW_LIST ? 2 : 1)) {
        console_printf("0x%08lx: address out of range\r\n", (unsigned long)cursor);
        remaining = 0;
    } else if (view == VIEW_DUMP)
        dump_row();
    else {
        uint16_t low;
        memcpy(&low, physical(cursor), 2);
        unsigned size = (low & 3) == 3 ? 4 : 2;
        if (!contains(cursor, size)) {
            console_printf("0x%08lx: address out of range\r\n", (unsigned long)cursor);
            remaining = 0;
        } else {
            uint32_t word = 0;
            memcpy(&word, physical(cursor), size);
            char row[128];
            disassemble(row, sizeof(row), cursor, word, size);
            console_printf("%s\r\n", row);
            cursor += size;
        }
    }
    if (!contains(cursor, 1))
        remaining = 0;
}
