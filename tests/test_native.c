#include "elf.h"
#include "loader.h"
#include "syscall.h"
#include "transfer.h"
#include "wire.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The fixed linker fixture remains a regression case, not the monitor ABI. */
#define TEXT_BASE UINT32_C(0x20020000)
#define TEXT_SIZE UINT32_C(0x10000)
#define DATA_BASE UINT32_C(0x20030000)
#define DATA_SIZE UINT32_C(0x10000)

/* The native memory array retains physical offsets and detects every bad loader write. */
static uint8_t ram[520 * 1024];
static uint64_t now;
static size_t guest_output_credit = SIZE_MAX, guest_output_count;
static uint8_t guest_output[2048];
static uint8_t incoming[8192], outgoing[8192];
static uint8_t read_bytes[8];
static size_t read_count, read_cursor;
static size_t in_count, in_cursor, out_count;
static char console_text[16384];
static size_t console_size, console_available = 8192;
static bool capture_console;
static bool console_online = true, upload_online = true;
uint8_t *physical(uint32_t address) {
    assert(address >= 0x20000000 && address < 0x20082000);
    return ram + address - 0x20000000;
}
uint64_t monitor_time_us(void) {
    return now;
}
void monitor_fence_i(void) {}
void monitor_memory_report(void) {}
void pmp_disable(void) {}
void pmp_enable(const MemoryPlan *plan) {
    (void)plan;
}
void monitor_panic(const char *message) {
    fprintf(stderr, "panic: %s\n", message);
    abort();
}
size_t console_space(void) {
    return console_available;
}
void shell_prompt(void) {}
void shell_stopped(void) {}
bool shell_idle(void) {
    return true;
}
bool console_connected(void) {
    return console_online;
}
bool console_empty(void) {
    return true;
}
bool guest_output_empty(void) {
    return true;
}
size_t guest_output_peek(const uint8_t **bytes) {
    *bytes = NULL;
    return 0;
}
void guest_output_consume(size_t count) {
    assert(count == 0);
}
void guest_output_discard(size_t staged) {
    (void)staged;
}
bool guest_input_put(uint8_t byte) {
    (void)byte;
    return true;
}
void shell_notice(const char *message) {
    (void)message;
}
void guest_input_clear(void) {}
int32_t guest_input_read(void *bytes, size_t count) {
    if (read_cursor == read_count)
        return -1;
    if (count > read_count - read_cursor)
        count = read_count - read_cursor;
    memcpy(bytes, read_bytes + read_cursor, count);
    read_cursor += count;
    return (int32_t)count;
}
size_t console_write(const void *bytes, size_t count) {
    (void)bytes;
    return count;
}
size_t guest_output_write(const void *bytes, size_t count) {
    if (count > guest_output_credit)
        count = guest_output_credit;
    assert(guest_output_count + count <= sizeof(guest_output));
    memcpy(guest_output + guest_output_count, bytes, count);
    guest_output_count += count;
    guest_output_credit -= count;
    return count;
}
void console_printf(const char *format, ...) {
    if (!capture_console)
        return;
    va_list args;
    va_start(args, format);
    int count =
        vsnprintf(console_text + console_size, sizeof(console_text) - console_size, format, args);
    va_end(args);
    assert(count >= 0 && (size_t)count < sizeof(console_text) - console_size);
    console_size += (size_t)count;
}
bool upload_connected(void) {
    return upload_online;
}
void upload_flush_input(void) {
    in_count = in_cursor = 0;
}
size_t upload_read(void *bytes, size_t count) {
    if (count > in_count - in_cursor)
        count = in_count - in_cursor;
    memcpy(bytes, incoming + in_cursor, count);
    in_cursor += count;
    return count;
}
size_t upload_write(const void *bytes, size_t count) {
    assert(count <= sizeof(outgoing) - out_count);
    memcpy(outgoing + out_count, bytes, count);
    out_count += count;
    return count;
}

static void settle_loader(void) {
    for (unsigned i = 0; loader_busy() && i < 50000; ++i)
        loader_step();
    assert(!loader_busy());
}
static void feed(const uint8_t *bytes, uint32_t count) {
    uint32_t cursor = 0;
    while (cursor < count) {
        uint32_t size = count - cursor;
        if (size > 397)
            size = 397;
        loader_feed(cursor, bytes + cursor, size);
        settle_loader();
        cursor += size;
    }
}

static void validate_headers(const uint8_t *file, uint32_t size) {
    Image image = {0};
    assert(elf_validate(file, size, &image) == NULL);
    uint8_t prefix[4096];
    memcpy(prefix, file, sizeof(prefix));
    uint32_t phoff = read_le32(file + 28), count = read_le16(file + 44), text = 0, data = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t offset = phoff + i * 32;
        if (read_le32(file + offset) == 1 && read_le32(file + offset + 24) == 5)
            text = offset;
        if (read_le32(file + offset) == 1 && read_le32(file + offset + 24) == 6)
            data = offset;
    }
    assert(text && data);
    const uint32_t bad_flags[] = {0, 2, 6, 8};
    for (unsigned i = 0; i < sizeof(bad_flags) / sizeof(bad_flags[0]); ++i) {
        write_le32(prefix + text + 24, bad_flags[i]);
        assert(elf_validate(prefix, size, &image) != NULL);
    }
    memcpy(prefix, file, sizeof(prefix));
    write_le32(prefix + text + 4, 0xfffffff0);
    assert(elf_validate(prefix, size, &image) != NULL);
    memcpy(prefix, file, sizeof(prefix));
    write_le32(prefix + text + 8, GUEST_END);
    assert(elf_validate(prefix, size, &image) != NULL);
    memcpy(prefix, file, sizeof(prefix));
    write_le32(prefix + text + 16, read_le32(prefix + text + 20) + 1);
    assert(elf_validate(prefix, size, &image) != NULL);
    memcpy(prefix, file, sizeof(prefix));
    write_le32(prefix + data + 8, TEXT_BASE);
    assert(elf_validate(prefix, size, &image) != NULL);
    memcpy(prefix, file, sizeof(prefix));
    write_le32(prefix + 28, UINT32_MAX);
    assert(elf_validate(prefix, size, &image) != NULL);
    memcpy(prefix, file, sizeof(prefix));
    write_le32(prefix + 24, TEXT_BASE + TEXT_SIZE - 2);
    assert(elf_validate(prefix, size, &image) != NULL);
}

static void load_and_reset(const uint8_t *file, uint32_t size) {
    memset(ram, 0xa5, sizeof(ram));
    loader_begin(size, 123, "native.elf");
    feed(file, size);
    loader_end();
    settle_loader();
    assert(loader.state == LOAD_DONE && !loader.error && program.state == READY);
    assert(program.threads[0].frame.x[2] == program.image.plan.stack_top &&
           program.threads[0].frame.pc == program.image.entry);
    for (uint32_t i = 0; i < program.image.segment_count; ++i) {
        const Segment *s = &program.image.segments[i];
        assert(memcmp(physical(s->address), file + s->offset, s->file_size) == 0);
        for (uint32_t j = s->file_size; j < s->memory_size; ++j)
            assert(physical(s->address)[j] == 0);
    }
    for (uint32_t address = GUEST_END; address < 0x20082000; ++address)
        assert(*physical(address) == 0xa5);
    assert(guest_range(DATA_BASE, DATA_SIZE, true));
    assert(!guest_range(TEXT_BASE, 1, true));
    assert(!guest_range(program.image.plan.snapshot_base, 1, true));
    assert(!guest_range(0xfffffff0, 64, false));
    memset(physical(DATA_BASE), 0xff, DATA_SIZE);
    memset(physical(program.image.plan.stack_base), 0xff, STACK_SIZE);
    program.state = FAULTED;
    program_reset();
    for (unsigned i = 0; program.reset_busy && i < 1000; ++i)
        program_step();
    assert(!program.reset_busy && program.state == READY);
    assert(memcmp(physical(DATA_BASE), file + program.image.segments[1].offset,
                  program.image.segments[1].file_size) == 0);
    for (uint32_t i = 0; i < STACK_SIZE; ++i)
        assert(physical(program.image.plan.stack_base)[i] == 0);

    /* BEGIN and cancellation preserve the guest; the first payload invalidates it. */
    program.state = STOPPED;
    program.threads[0].frame.x[5] = 0x87654321;
    Program old = program;
    uint8_t prefix[4096];
    memcpy(prefix, file, sizeof(prefix));
    prefix[4] = 2;
    loader_begin(size, 0, "bad.elf");
    assert(!memcmp(&old, &program, sizeof(program)));
    loader_abort("cancelled before data");
    assert(!memcmp(&old, &program, sizeof(program)));
    loader_begin(size, 0, "bad.elf");
    loader_feed(0, prefix, 1024);
    assert(loader.destructive && !program.valid);
    loader_feed(1024, prefix + 1024, 1024);
    loader_feed(2048, prefix + 2048, 1024);
    loader_feed(3072, prefix + 3072, 1024);
    assert(loader.error && loader.destructive && !program.valid);
    loader_begin(size, 0, "interrupted.elf");
    for (uint32_t i = 0; i < 4096; i += 1024)
        loader_feed(i, file + i, 1024);
    settle_loader();
    assert(loader.destructive && !program.valid);
    loader_abort("interrupted");
    assert(program.state == EMPTY);
}

static void wire_checks(void) {
    assert(crc32_update(0, (const uint8_t *)"123456789", 9) == UINT32_C(0xcbf43926));
    uint8_t payload[WIRE_PAYLOAD];
    WireFrame frame = {.type = WIRE_DATA,
                       .session = 0x123456789abcdef0,
                       .sequence = 4,
                       .offset = 3072,
                       .length = 1024,
                       .payload = payload},
              decoded;
    for (unsigned i = 0; i < 1024; ++i)
        payload[i] = (uint8_t)i;
    uint8_t encoded[WIRE_LIMIT], received[WIRE_LIMIT];
    size_t size = wire_encode(&frame, encoded, sizeof(encoded));
    assert(size < WIRE_LIMIT && encoded[0] == 0 && encoded[size - 1] == 0);
    memcpy(received, encoded, size);
    assert(wire_decode(received + 1, size - 2, &decoded));
    assert(decoded.session == frame.session && decoded.offset == 3072 && decoded.length == 1024);
    assert(memcmp(decoded.payload, frame.payload, 1024) == 0);
    encoded[size / 2] ^= 1;
    assert(!wire_decode(encoded + 1, size - 2, &decoded));
    uint8_t truncated[] = {0xff};
    assert(!wire_decode(truncated, sizeof(truncated), &decoded));

    /* Zero runs and 254-byte COBS boundaries must survive destructive decode.
     * The capacity guard rejects an undersized destination before writing it. */
    for (unsigned pattern = 0; pattern < 3; ++pattern) {
        for (unsigned i = 0; i < sizeof(payload); ++i)
            payload[i] = pattern == 0 ? 0 : pattern == 1 ? 255 : (uint8_t)i;
        for (unsigned count = 0; count <= WIRE_PAYLOAD; ++count) {
            frame.length = (uint16_t)count;
            size_t required = 24 + count + (24 + count) / 254 + 3;
            memset(encoded, 0x5a, sizeof(encoded));
            assert(wire_encode(&frame, encoded, required - 1) == 0);
            for (unsigned i = 0; i < sizeof(encoded); ++i)
                assert(encoded[i] == 0x5a);
            size = wire_encode(&frame, encoded, required);
            assert(size && size <= required);
            assert(wire_decode(encoded + 1, size - 2, &decoded));
            assert(decoded.payload == encoded + 21 && decoded.length == count);
            assert(!memcmp(decoded.payload, payload, count));
        }
    }
}

/* A tiny RWX image exercises header bytes as load data and permits ordinary
 * guest self-modification while preserving the surrounding sandbox. */
static void flexible_checks(void) {
    uint8_t file[512] = {0};
    memcpy(file, "\177ELF\1\1\1", 7);
    file[16] = 2;
    file[18] = 243;
    write_le32(file + 20, 1);
    write_le32(file + 24, TEXT_BASE + 128);
    write_le32(file + 28, 52);
    file[40] = 52;
    file[42] = 32;
    file[44] = 1;
    write_le32(file + 52, 1);
    write_le32(file + 60, TEXT_BASE);
    write_le32(file + 64, 0x1234);
    write_le32(file + 68, sizeof(file));
    write_le32(file + 72, 1024);
    write_le32(file + 76, 7);
    write_le32(file + 80, 1);
    Image image = {0};
    assert(!elf_validate(file, sizeof(file), &image));
    assert(image.plan.snapshot && memory_plan_verify(&image));
    loader_begin(sizeof(file), 0, "tiny.elf");
    feed(file, sizeof(file));
    loader_end();
    settle_loader();
    assert(!loader.error && program.valid);
    assert(!memcmp(physical(TEXT_BASE), file, sizeof(file)));
    assert(guest_range(TEXT_BASE, 512, true));
    assert(!guest_range(0x10000000, 1, false));
    assert(!guest_range(program.image.plan.snapshot_base, 1, false));

    /* Unmapped program headers omit PHDR metadata and still terminate auxv. */
    Image unmapped = image;
    unmapped.phoff = sizeof(file);
    program_install(&unmapped);
    assert(!program_launch(0, NULL));
    uint32_t aux = program.threads[0].frame.x[12] + 4;
    assert(read_le32(physical(aux)) == 6 && read_le32(physical(aux + 4)) == 4096);
    assert(read_le32(physical(aux + 8)) == 9);
    assert(read_le32(physical(aux + 12)) == image.entry);
    assert(read_le32(physical(aux + 16)) == 0 && read_le32(physical(aux + 20)) == 0);

    /* Mapped headers follow the common entries before the terminating pair. */
    program_install(&image);
    const char *args[] = {"one", "two words", ""};
    assert(!program_launch(3, args));
    uint32_t sp = program.threads[0].frame.x[2];
    assert(!(sp & 15) && read_le32(physical(sp)) == 4);
    assert(!strcmp((char *)physical(read_le32(physical(sp + 12))), "two words"));
    assert(read_le32(physical(sp + 24)) == 0);
    assert(program.threads[0].frame.x[12] == sp + 24);
    aux = sp + 28;
    assert(read_le32(physical(aux)) == 6 && read_le32(physical(aux + 4)) == 4096);
    assert(read_le32(physical(aux + 8)) == 9);
    assert(read_le32(physical(aux + 12)) == image.entry);
    assert(read_le32(physical(aux + 16)) == 3);
    assert(read_le32(physical(aux + 20)) == TEXT_BASE + image.phoff);
    assert(read_le32(physical(aux + 24)) == 4 && read_le32(physical(aux + 28)) == 32);
    assert(read_le32(physical(aux + 32)) == 5);
    assert(read_le32(physical(aux + 36)) == image.phnum);
    assert(read_le32(physical(aux + 40)) == 0 && read_le32(physical(aux + 44)) == 0);
    uint32_t base = program.current_break;
    program.threads[0].frame.cause = 8;
    program.threads[0].frame.x[17] = 214;
    program.threads[0].frame.x[10] = base + 1500;
    memset(physical(base), 0xaa, 1500);
    program_after_trap(0);
    program_step();
    assert(program.state == ACTIVE && !strcmp(program_state_name(), "waiting"));
    assert(!guest_range(base, 1, true));
    program_step();
    assert(program.state == ACTIVE && !strcmp(program_state_name(), "waiting"));
    program_step();
    assert(program.current_break == base + 1500 && guest_range(base, 1500, true));
    for (unsigned i = 0; i < 1500; ++i)
        assert(!*physical(base + i));
    program.threads[0].frame.cause = 8;
    program.threads[0].frame.x[17] = 214;
    program.threads[0].frame.x[10] = UINT32_MAX;
    program_after_trap(0);
    program_step();
    assert(program.threads[0].frame.x[10] == base + 1500);

    /* A large write waits across queue pressure and resumes without duplicating bytes. */
    for (unsigned i = 0; i < 700; ++i)
        physical(base)[i] = (uint8_t)i;
    guest_output_count = 0;
    guest_output_credit = 300;
    program.threads[0].frame.cause = 8;
    program.threads[0].frame.x[17] = 64;
    program.threads[0].frame.x[10] = 1;
    program.threads[0].frame.x[11] = base;
    program.threads[0].frame.x[12] = 700;
    program_after_trap(0);
    program_step();
    assert(program.state == ACTIVE && !strcmp(program_state_name(), "waiting") &&
           guest_output_count == 256);
    program_step();
    assert(program.state == ACTIVE && !strcmp(program_state_name(), "waiting") &&
           guest_output_count == 300);
    program_suspend("test");
    assert(program.state == STOPPED && !strcmp(program_state_name(), "stopped"));
    program_resume();
    program_step();
    assert(program.state == ACTIVE && !strcmp(program_state_name(), "waiting") &&
           guest_output_count == 300);
    guest_output_credit = 400;
    program_step();
    program_step();
    assert(program.threads[0].frame.x[10] == 700 && guest_output_count == 700);
    assert(!memcmp(guest_output, physical(base), 700));
    guest_output_credit = SIZE_MAX;

    /* Reset retains the same layout and restores initialized writable bytes. */
    *physical(TEXT_BASE + 128) = 0xaa;
    *physical(TEXT_BASE + 600) = 0xbb;
    MemoryPlan original = program.image.plan;
    program_reset();
    for (unsigned i = 0; program.reset_busy && i < 1000; ++i)
        program_step();
    assert(!program.reset_busy && !memcmp(&original, &program.image.plan, sizeof(original)));
    assert(*physical(TEXT_BASE + 128) == file[128] && !*physical(TEXT_BASE + 600));

    /* Explicitly disabling reset permits a run but never a restart. */
    loader_begin(sizeof(file), 0, "no-reset.elf");
    loader.image.plan.requested = SNAP_OFF;
    feed(file, sizeof(file));
    loader_end();
    settle_loader();
    assert(!loader.error && !program.image.plan.snapshot);
    assert(!program_launch(0, NULL));
    program.state = EXITED;
    program_reset();
    assert(!program.reset_busy && program.state == EXITED);

    /* Prefix destinations overwrite the initial buffers and planning workspace.
     * Shared file bytes and overlapping BSS must survive final clearing. */
    uint8_t shared[8192];
    for (unsigned i = 0; i < sizeof(shared); ++i)
        shared[i] = (uint8_t)i;
    memcpy(shared, file, 84);
    shared[44] = 2;
    write_le32(shared + 24, GUEST_BASE + 128);
    write_le32(shared + 60, GUEST_BASE);
    write_le32(shared + 68, sizeof(shared));
    write_le32(shared + 72, 16384);
    memcpy(shared + 84, shared + 52, 32);
    write_le32(shared + 92, GUEST_BASE + 8192);
    loader_begin(sizeof(shared), 0, "scratch.elf");
    uint32_t cursor = 0;
    while (cursor < sizeof(shared)) {
        uint32_t count = sizeof(shared) - cursor;
        if (count > 997)
            count = 997;
        loader_feed(cursor, shared + cursor, count);
        cursor += count;
        settle_loader();
    }
    loader_end();
    settle_loader();
    assert(!loader.error && program.state == READY);
    assert(!memcmp(physical(GUEST_BASE), shared, sizeof(shared)));
    assert(!memcmp(physical(GUEST_BASE + 8192), shared, sizeof(shared)));
    for (uint32_t address = GUEST_BASE + 16384; address < program.image.plan.snapshot_base;
         ++address)
        assert(!*physical(address));

    Image large = {.segment_count = 1};
    large.segments[0] = (Segment){
        .address = TEXT_BASE, .memory_size = 168 * 1024, .file_size = 168 * 1024, .permissions = 7};
    assert(!memory_plan_build(&large, SNAP_AUTO) && !large.plan.snapshot);
    assert(!memory_plan_build(&large, SNAP_OFF) && !large.plan.snapshot);
    large.segments[0].memory_size = large.segments[0].file_size = 210 * 1024;
    assert(!memory_plan_build(&large, SNAP_AUTO) && !large.plan.snapshot);
    large.plan.regions[0] = (PmpRegion){0x20000000, 0x80000, 7};
    assert(!memory_plan_verify(&large));
}

uint64_t memory_test_solve(Image *image, uint32_t base, uint32_t size, unsigned budget);

/* Enumerate every ordered pair of regions in a four-cell NAPOT tree. This
 * oracle evaluates first-match permissions directly, without DP state. */
static void solver_oracle(void) {
    const uint8_t modes[] = {0, 4, 5, 6, 7};
    PmpRegion candidates[35];
    unsigned count = 0;
    for (uint32_t size = 32; size <= 128; size *= 2)
        for (uint32_t offset = 0; offset < 128; offset += size)
            for (unsigned m = 0; m < 5; ++m)
                candidates[count++] = (PmpRegion){GUEST_BASE + offset, size, modes[m]};
    for (unsigned pattern = 0; pattern < 256; ++pattern) {
        Image image = {.segment_count = 4};
        image.plan.snapshot_base = GUEST_END;
        image.plan.heap_base = image.plan.heap_limit = GUEST_END;
        image.plan.stack_base = image.plan.stack_top = GUEST_END;
        for (unsigned i = 0; i < 4; ++i)
            image.segments[i] = (Segment){.address = GUEST_BASE + i * 32,
                                          .memory_size = 32,
                                          .permissions = modes[1 + ((pattern >> (i * 2)) & 3)]};
        uint64_t solved = memory_test_solve(&image, GUEST_BASE, 128, 2);
        uint64_t best = 0;
        bool found = false;
        for (unsigned a = 0; a < count; ++a) {
            for (unsigned b = 0; b < count; ++b) {
                bool valid = true;
                uint64_t score = 0;
                unsigned previous_entry = 2;
                for (unsigned i = 0; i < 4; ++i) {
                    uint32_t address = GUEST_BASE + i * 32;
                    uint8_t actual = 0, required = image.segments[i].permissions;
                    unsigned entry = 2;
                    if (range_contains(candidates[a].base, candidates[a].size, address, 1)) {
                        actual = candidates[a].permissions;
                        entry = 0;
                    } else if (range_contains(candidates[b].base, candidates[b].size, address, 1)) {
                        actual = candidates[b].permissions;
                        entry = 1;
                    }
                    valid = valid && (actual & required) == required;
                    if (i && (required & PERM_X) && (image.segments[i - 1].permissions & PERM_X) &&
                        entry != previous_entry)
                        valid = false;
                    previous_entry = entry;
                    if (!(required & PERM_W) && !(actual & PERM_W))
                        score += UINT64_C(1) << ((required & PERM_X) ? 42 : 28);
                    if ((required & PERM_W) && !(actual & PERM_X))
                        score += UINT64_C(1) << 14;
                }
                if (valid && (!found || score > best)) {
                    best = score;
                    found = true;
                }
            }
        }
        assert(found && solved == best);
    }
}

static WireFrame request(const WireFrame *frame) {
    if (frame) {
        in_count = wire_encode(frame, incoming, sizeof(incoming));
        in_cursor = 0;
    }
    for (unsigned i = 0; i < 1500; ++i) {
        now += 100;
        transfer_step();
        loader_step();
        if (out_count > 2 && outgoing[out_count - 1] == 0) {
            WireFrame response;
            assert(wire_decode(outgoing + 1, out_count - 2, &response));
            out_count = 0;
            return response;
        }
    }
    assert(!"transfer response timed out");
    return (WireFrame){0};
}

static void transfer_checks(const uint8_t *file, uint32_t size) {
    Program previous = program;
    uint32_t previous_crc = crc32_update(0, physical(GUEST_BASE), GUEST_END - GUEST_BASE);
    transfer_arm(true);
    WireFrame response = request(NULL);
    assert(response.type == WIRE_READY);
    uint64_t session = response.session;
    uint8_t payload[WIRE_PAYLOAD];
    WireFrame frame = {.type = WIRE_BEGIN, .session = session, .length = 10, .payload = payload};
    write_le32(payload, size);
    write_le32(payload + 4, crc32_update(0, file, size));
    payload[8] = 1;
    payload[9] = 'x';
    response = request(&frame);
    assert(response.type == WIRE_ACK && response.sequence == 1);
    assert(!memcmp(&previous, &program, sizeof(program)));
    frame = (WireFrame){.type = WIRE_DATA, .session = session, .sequence = 1};
    response = request(&frame);
    assert(response.type == WIRE_NAK);
    assert(!memcmp(&previous, &program, sizeof(program)));
    assert(previous_crc == crc32_update(0, physical(GUEST_BASE), GUEST_END - GUEST_BASE));
    uint32_t offset = 0, sequence = 1;
    while (offset < size) {
        uint32_t count = size - offset;
        if (count > 1024)
            count = 1024;
        frame = (WireFrame){.type = WIRE_DATA,
                            .session = session,
                            .sequence = sequence,
                            .offset = offset,
                            .length = (uint16_t)count,
                            .payload = file + offset};
        response = request(&frame);
        assert(response.type == WIRE_ACK && response.offset == offset + count);
        assert(!program.valid && program.state == EMPTY);
        if (sequence == 6) {
            response = request(&frame);
            assert(response.type == WIRE_ACK && response.offset == offset + count);
        }
        offset += count;
        ++sequence;
    }
    frame = (WireFrame){.type = WIRE_END, .session = session, .sequence = sequence, .offset = size};
    response = request(&frame);
    assert(response.type == WIRE_RESULT && response.payload[0] == 0 && program.state == READY);
    response = request(&frame);
    assert(response.type == WIRE_RESULT && response.payload[0] == 0);
    transfer_arm(true);
    response = request(NULL);
    frame = (WireFrame){.type = WIRE_ABORT, .session = response.session};
    response = request(&frame);
    assert(response.type == WIRE_RESULT && response.payload[0] == 1 && response.payload[1] == 1);
    program.state = ACTIVE;
    WireFrame claim = {.type = WIRE_CLAIM, .session = 123};
    assert(request(&claim).type == WIRE_ERROR);
    program.state = READY;
    response = request(&frame);
    assert(response.type == WIRE_RESULT && response.payload[0] == 1 && response.payload[1] == 1);
}

/* Ownership retries do not repeat transitions, and loss waits for both cores
 * before redirecting guest I/O. The fake clock makes lease expiry deterministic. */
static void guest_session_checks(const uint8_t *file, uint32_t size) {
    program_forget();
    WireFrame frame = {.type = WIRE_CLAIM, .session = 123};
    assert(request(&frame).type == WIRE_ATTACHED && guest_attached());
    assert(guest_take_notice().kind == GUEST_NOTICE_ATTACHED);
    assert(request(&frame).type == WIRE_ATTACHED && guest_take_notice().kind == GUEST_NOTICE_NONE);
    frame.session = 456;
    assert(request(&frame).type == WIRE_ERROR && guest_attached());
    program.state = ACTIVE;
    program.threads[0].executing = program.threads[1].executing = true;
    assert(guest_merge() != NULL);
    now += UINT64_C(5000001);
    transfer_step();
    assert(program.state == STOPPING && guest_attached());
    program.threads[0].executing = false;
    program_step();
    transfer_step();
    assert(program.state == STOPPING && guest_attached());
    program.threads[1].executing = false;
    program_step();
    assert(program.state == STOPPED);
    transfer_step();
    assert(!guest_attached() && guest_take_notice().kind == GUEST_NOTICE_MERGED);
    assert(request(NULL).type == WIRE_RELEASED);
    frame.session = 123;
    assert(request(&frame).type == WIRE_ERROR);
    WireFrame release = {.type = WIRE_RELEASE, .session = 123};
    assert(request(&release).type == WIRE_RELEASED &&
           guest_take_notice().kind == GUEST_NOTICE_NONE);
    frame.session = 789;
    assert(request(&frame).type == WIRE_ATTACHED);
    assert(guest_take_notice().kind == GUEST_NOTICE_ATTACHED);
    assert(guest_merge() == NULL);
    assert(request(NULL).type == WIRE_RELEASED);
    assert(!guest_attached() && guest_take_notice().kind == GUEST_NOTICE_MERGED);
    program_forget();

    /* Recovery commits even if the monitor has not read the attachment notice.
     * Both configuration changes remain queued in their original order. */
    frame.session = 1000;
    assert(request(&frame).type == WIRE_ATTACHED);
    upload_online = false;
    transfer_step();
    assert(!guest_attached());
    assert(guest_take_notice().kind == GUEST_NOTICE_ATTACHED);
    assert(guest_take_notice().kind == GUEST_NOTICE_MERGED);
    assert(guest_take_notice().kind == GUEST_NOTICE_NONE);
    upload_online = true;
    release.session = 1000;
    assert(request(&release).type == WIRE_RELEASED);

    /* A retained owner can upload through its descriptor, acknowledge RESULT,
     * and return to guest traffic without another ownership change. */
    frame.session = 2000;
    assert(request(&frame).type == WIRE_ATTACHED);
    assert(guest_take_notice().kind == GUEST_NOTICE_ATTACHED);
    transfer_arm(true);
    WireFrame ready = request(NULL);
    assert(ready.type == WIRE_READY);
    assert(request(&frame).type == WIRE_ERROR && transfer_active());
    WireFrame abort = {.type = WIRE_ABORT, .session = ready.session};
    assert(request(&abort).type == WIRE_RESULT);
    assert(request(&frame).type == WIRE_ATTACHED);
    transfer_checks(file, size);
    assert(guest_attached() && request(&frame).type == WIRE_ATTACHED);
    assert(guest_take_notice().kind == GUEST_NOTICE_NONE);
    assert(guest_merge() == NULL);
    assert(request(NULL).type == WIRE_RELEASED);
    assert(guest_take_notice().kind == GUEST_NOTICE_MERGED);
    program_forget();
    console_online = false;
    frame.session = 3000;
    assert(request(&frame).type == WIRE_ERROR && !guest_attached());
    console_online = true;
}

/* A synthetic map exercises exact ELF edges within PMP granules, overlapping
 * requirements, first-match entries, heap accounting, and the system denial. */
static void map_checks(void) {
    program_forget();
    Image image = {.segment_count = 3};
    image.segments[0] =
        (Segment){.address = GUEST_BASE + 33, .memory_size = 63, .permissions = PERM_R | PERM_X};
    image.segments[1] =
        (Segment){.address = GUEST_BASE + 64, .memory_size = 64, .permissions = PERM_R | PERM_W};
    image.segments[2] =
        (Segment){.address = GUEST_BASE + 128, .memory_size = 32, .permissions = PERM_X};
    MemoryPlan *p = &image.plan;
    p->heap_base = GUEST_BASE + 192;
    p->snapshot_base = p->stack_top = GUEST_END - 0x4000;
    p->stack_base = p->stack_top - STACK_SIZE;
    p->heap_limit = p->stack_base - 1024;
    p->snapshot = true;
    p->snapshot_size = 64;
    p->region_count = 5;
    p->regions[0] = (PmpRegion){GUEST_BASE + 64, 32, 7};
    p->regions[1] = (PmpRegion){GUEST_BASE, 0x40000, 7};
    p->regions[2] = (PmpRegion){GUEST_BASE + 0x40000, 0x20000, 6};
    p->regions[3] = (PmpRegion){GUEST_BASE + 0x60000, 0x10000, 6};
    p->regions[4] = (PmpRegion){GUEST_BASE + 0x70000, 0x8000, 6};
    program_install(&image);
    program.current_break = GUEST_BASE + 224;
    capture_console = true;
    console_size = 0;
    memory_map_start();
    console_available = 511;
    memory_map_step();
    assert(!console_size && memory_map_busy());
    console_available = 8192;
    for (unsigned i = 0; memory_map_busy() && i < 100; ++i) {
        size_t previous = console_size;
        memory_map_step();
        assert(console_size - previous <= 512);
    }
    assert(!memory_map_busy());
    assert(strstr(console_text, "Required  Effective  PMP"));
    assert(strstr(console_text, "20000021..20000040  ELF            r-x       rwx        1"));
    assert(strstr(console_text, "20000040..20000060  ELF            rwx       rwx        0"));
    assert(strstr(console_text, "20000080..200000a0  ELF            --x       rwx        1"));
    assert(strstr(console_text, "200000c0..200000e0  heap used      rw-       rwx        1"));
    assert(strstr(console_text, "200000e0..20040000  heap capacity  rw-       rwx        1"));
    assert(strstr(console_text, "20073c00..20074000  stack guard    ---       rw-        4"));
    assert(strstr(console_text, "20074000..20078000  stack          rw-       rw-        4"));
    assert(strstr(console_text, "20078000..2007c000  reset reserve  ---       ---        7"));
    assert(strstr(console_text, "2007c000..20080000  monitor        ---       ---        7"));
    assert(strstr(console_text, "PMP 0  20000040..20000060  rwx     NAPOT  08000013"));
    assert(strstr(console_text, "PMP 5  (unused)"));
    assert(strstr(console_text, "3fffffff (system deny)"));
    capture_console = false;
    program_forget();
    memory_map_start();
    assert(!memory_map_busy());
}

static int32_t invoke(unsigned core, uint32_t number, uint32_t a0, uint32_t a1, uint32_t a2) {
    GuestFrame *f = &program.threads[core].frame;
    f->cause = 8;
    f->x[17] = number;
    f->x[10] = a0;
    f->x[11] = a1;
    f->x[12] = a2;
    program_after_trap(core);
    program_step();
    return (int32_t)f->x[10];
}

/* Exercise lifecycle transitions and shared requests without assuming which
 * hardware core next acquires the monitor gate. */
static void thread_checks(void) {
    program_forget();
    Image image = {.entry = GUEST_BASE, .segment_count = 1};
    image.segments[0] = (Segment){
        .address = GUEST_BASE, .memory_size = 32, .file_size = 32, .permissions = PERM_R | PERM_X};
    assert(!memory_plan_build(&image, SNAP_OFF));
    program_install(&image);
    assert(!program_launch(0, NULL));
    assert(invoke(0, SYS_THREAD_CREATE, GUEST_BASE, 0x76543210, 0) == 1);
    GuestThread *second = &program.threads[1];
    assert(second->state == THREAD_RUNNABLE && second->frame.pc == GUEST_BASE);
    for (unsigned i = 0; i < 32; ++i)
        assert(second->frame.x[i] == (i == 10 ? 0x76543210u : 0));
    assert(invoke(0, SYS_THREAD_CREATE, GUEST_BASE, 0, 0) == -11);
    invoke(0, SYS_EXIT, 0, 0, 0);
    assert(program.threads[0].state == THREAD_UNUSED && program.state == ACTIVE &&
           !strcmp(program_state_name(), "running"));
    assert(invoke(1, SYS_THREAD_CREATE, GUEST_BASE + 1, 0, 0) == -14);
    assert(invoke(1, SYS_THREAD_CREATE, GUEST_END, 0, 0) == -14);
    assert(invoke(1, SYS_THREAD_CREATE, GUEST_BASE, 123, 0) == 0);

    /* Two heap expansions complete in request order with one published break. */
    uint32_t base = program.current_break;
    memset(physical(base), 0xcc, 3072);
    invoke(0, SYS_BRK, base + 2048, 0, 0);
    invoke(1, SYS_BRK, base + 3072, 0, 0);
    assert(program.state == ACTIVE && !strcmp(program_state_name(), "waiting"));
    program_suspend("heap growth");
    assert(program.state == STOPPED);
    PendingCall growth = program.threads[0].pending;
    program_step();
    assert(program.current_break == base);
    assert(program.threads[0].pending.kind == CALL_BRK);
    assert(program.threads[0].pending.heap.target == growth.heap.target &&
           program.threads[0].pending.heap.cursor == growth.heap.cursor &&
           program.threads[0].pending.heap.started == growth.heap.started);
    program_resume();
    assert(program.state == ACTIVE && !strcmp(program_state_name(), "waiting"));
    while (program.threads[0].state == THREAD_BLOCKED || second->state == THREAD_BLOCKED)
        program_step();
    assert(program.current_break == base + 3072);
    for (unsigned i = 0; i < 3072; ++i)
        assert(!physical(base)[i]);

    /* Read order follows requests rather than slot numbers or service order. */
    invoke(1, SYS_READ, 0, base + 1, 1);
    assert(program.state == ACTIVE && !strcmp(program_state_name(), "running"));
    invoke(0, SYS_READ, 0, base, 1);
    assert(program.state == ACTIVE && !strcmp(program_state_name(), "waiting"));
    program_suspend("pending reads");
    program_step();
    assert(program.state == STOPPED && program.threads[0].pending.kind == CALL_READ &&
           second->pending.kind == CALL_READ);
    program_resume();
    assert(program.state == ACTIVE && !strcmp(program_state_name(), "waiting"));
    memcpy(read_bytes, "xy", 2);
    read_count = 2;
    read_cursor = 0;
    program_step();
    program_step();
    assert(*physical(base + 1) == 'x' && *physical(base) == 'y');
    read_count = read_cursor = 0;

    /* A blocked write freezes during suspension and revalidates its buffer
     * after another thread shrinks the heap. */
    guest_output_credit = 0;
    invoke(0, SYS_WRITE, 1, base, 128);
    second->executing = true;
    program_suspend("test");
    assert(program.state == STOPPING);
    program_step();
    assert(program.threads[0].pending.io.remaining == 128);
    /* A syscall trapped during the barrier stays unconsumed until resume. */
    uint32_t pc = second->frame.pc;
    second->frame.cause = 8;
    second->frame.x[17] = SYS_YIELD;
    program_after_trap(1);
    assert(program.state == STOPPED && program_quiescent());
    program_step();
    assert(second->trap_pending && second->frame.pc == pc);
    program_resume();
    assert(program.state == ACTIVE && !strcmp(program_state_name(), "running"));
    program_step();
    assert(!second->trap_pending && second->frame.pc == pc + 4 && second->frame.x[10] == 0);
    invoke(1, SYS_BRK, base, 0, 0);
    program_step();
    program_step();
    assert((int32_t)program.threads[0].frame.x[10] == -14);
    guest_output_credit = SIZE_MAX;

    /* Both breakpoint widths stop the peer before exposing the prompt. A
     * peer breakpoint during the barrier is consumed without another stop. */
    const uint8_t breaks[] = {0x73, 0x00, 0x10, 0x00, 0x02, 0x90};
    memcpy(physical(GUEST_BASE), breaks, sizeof(breaks));
    for (unsigned core = 0; core < 2; ++core) {
        GuestThread *thread = &program.threads[core];
        GuestThread *peer = &program.threads[core ^ 1u];
        thread->frame.pc = GUEST_BASE;
        thread->frame.cause = 3;
        thread->frame.x[10] = 123;
        peer->executing = true;
        program_after_trap(core);
        assert(program.state == STOPPING && thread->frame.pc == GUEST_BASE + 4);
        assert(!thread->trap_pending && thread->frame.x[10] == 123);
        peer->frame.pc = GUEST_BASE + 4;
        peer->frame.cause = 3;
        program_after_trap(core ^ 1u);
        assert(program.state == STOPPED && program_quiescent());
        assert(peer->frame.pc == GUEST_BASE + 6 && !peer->trap_pending);
        program_step();
        program_resume();
        program_step();
        assert(program.state == ACTIVE && thread->frame.pc == GUEST_BASE + 4 &&
               peer->frame.pc == GUEST_BASE + 6);
    }

    /* Group exit waits for the other core, while a fault preserves both
     * frames and prohibits resuming either thread. */
    program.threads[0].executing = true;
    invoke(1, SYS_EXIT_GROUP, 7, 0, 0);
    assert(program.state == STOPPING);
    program.threads[0].frame.cause = UINT32_C(0x80000007);
    program_after_trap(0);
    assert(program.state == EXITED && program.exit_status == 7);
    program_forget();
    program_install(&image);
    assert(!program_launch(0, NULL));
    assert(invoke(0, SYS_THREAD_CREATE, GUEST_BASE, 0, 0) == 1);
    program.threads[0].executing = true;
    second->frame.cause = 2;
    second->frame.pc = GUEST_BASE + 2;
    program_after_trap(1);
    assert(program.state == STOPPING);
    program.threads[0].frame.cause = UINT32_C(0x80000007);
    program_after_trap(0);
    assert(program.state == FAULTED && second->frame.pc == GUEST_BASE + 2);
    program_resume();
    assert(program.state == FAULTED);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *stream = fopen(argv[1], "rb");
    assert(stream && fseek(stream, 0, SEEK_END) == 0);
    long length = ftell(stream);
    assert(length >= 4096 && length <= ELF_FILE_LIMIT);
    rewind(stream);
    uint8_t *file = malloc((size_t)length);
    assert(file && fread(file, 1, (size_t)length, stream) == (size_t)length);
    fclose(stream);
    wire_checks();
    validate_headers(file, (uint32_t)length);
    load_and_reset(file, (uint32_t)length);
    flexible_checks();
    solver_oracle();
    transfer_checks(file, (uint32_t)length);
    guest_session_checks(file, (uint32_t)length);
    assert(!memory_map_busy());
    map_checks();
    thread_checks();
    free(file);
    puts("native checks: ELF rejection, streaming, reset, framing, retries, guest sessions passed");
    return 0;
}
