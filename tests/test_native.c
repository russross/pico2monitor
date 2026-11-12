#include "elf.h"
#include "loader.h"
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
static bool simulate_guest;
static size_t guest_output_credit = SIZE_MAX, guest_output_count;
static uint8_t guest_output[2048];
static uint8_t incoming[8192], outgoing[8192];
static size_t in_count, in_cursor, out_count;
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
    return 8192;
}
void guest_enter(GuestFrame *frame) {
    assert(simulate_guest);
    frame->cause = UINT32_C(0x80000007);
}
void shell_prompt(void) {}
void shell_notice(const char *message) {
    (void)message;
}
void guest_input_clear(void) {}
int32_t guest_input_read(void *bytes, size_t count) {
    (void)bytes;
    (void)count;
    return -1;
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
    (void)format;
}
bool upload_connected(void) {
    return true;
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
    for (unsigned i = 0; loader_busy() && i < 1000; ++i)
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
    write_le32(prefix + text + 8, 0x20000000);
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
    assert(program.frame.x[2] == program.image.plan.stack_top &&
           program.frame.pc == program.image.entry);
    for (uint32_t i = 0; i < program.image.segment_count; ++i) {
        const Segment *s = &program.image.segments[i];
        assert(memcmp(physical(s->address), file + s->offset, s->file_size) == 0);
        for (uint32_t j = s->file_size; j < s->memory_size; ++j)
            assert(physical(s->address)[j] == 0);
    }
    for (uint32_t i = 0; i < GUEST_BASE - 0x20000000; ++i)
        assert(ram[i] == 0xa5);
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
    program.frame.x[5] = 0x87654321;
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
    WireFrame frame = {.type = WIRE_DATA,
                       .session = 0x123456789abcdef0,
                       .sequence = 4,
                       .offset = 3072,
                       .length = 1024},
              decoded;
    for (unsigned i = 0; i < 1024; ++i)
        frame.payload[i] = (uint8_t)i;
    uint8_t encoded[WIRE_LIMIT];
    size_t size = wire_encode(&frame, encoded);
    assert(size < WIRE_LIMIT && encoded[0] == 0 && encoded[size - 1] == 0);
    assert(wire_decode(encoded + 1, size - 2, &decoded));
    assert(decoded.session == frame.session && decoded.offset == 3072 && decoded.length == 1024);
    assert(memcmp(decoded.payload, frame.payload, 1024) == 0);
    encoded[size / 2] ^= 1;
    assert(!wire_decode(encoded + 1, size - 2, &decoded));
    assert(!wire_decode((const uint8_t *)"\xff", 1, &decoded));
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

    const char *args[] = {"one", "two words", ""};
    assert(!program_launch(3, args));
    uint32_t sp = program.frame.x[2];
    assert(!(sp & 15) && read_le32(physical(sp)) == 4);
    assert(!strcmp((char *)physical(read_le32(physical(sp + 12))), "two words"));
    assert(read_le32(physical(sp + 24)) == 0);
    assert(program.frame.x[12] == sp + 24);
    uint32_t base = program.current_break;
    program.frame.cause = 8;
    program.frame.x[17] = 214;
    program.frame.x[10] = base + 1500;
    memset(physical(base), 0xaa, 1500);
    program_after_trap();
    assert(program.state == WAITING);
    assert(!guest_range(base, 1, true));
    simulate_guest = true;
    program_step();
    assert(program.state == WAITING);
    program_step();
    simulate_guest = false;
    assert(program.current_break == base + 1500 && guest_range(base, 1500, true));
    for (unsigned i = 0; i < 1500; ++i)
        assert(!*physical(base + i));
    program.frame.cause = 8;
    program.frame.x[17] = 214;
    program.frame.x[10] = UINT32_MAX;
    program_after_trap();
    assert(program.frame.x[10] == base + 1500);

    /* A large write waits across queue pressure and resumes without duplicating bytes. */
    for (unsigned i = 0; i < 700; ++i)
        physical(base)[i] = (uint8_t)i;
    guest_output_count = 0;
    guest_output_credit = 300;
    program.frame.cause = 8;
    program.frame.x[17] = 64;
    program.frame.x[10] = 1;
    program.frame.x[11] = base;
    program.frame.x[12] = 700;
    program_after_trap();
    assert(program.state == WAITING && guest_output_count == 256);
    program_step();
    assert(program.state == WAITING && guest_output_count == 300);
    program_suspend("test");
    assert(program.state == STOPPED && program.stopped_state == WAITING);
    program.state = program.stopped_state;
    program_step();
    assert(program.state == WAITING && guest_output_count == 300);
    guest_output_credit = 400;
    simulate_guest = true;
    program_step();
    program_step();
    simulate_guest = false;
    assert(program.frame.x[10] == 700 && guest_output_count == 700);
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
        in_count = wire_encode(frame, incoming);
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
    WireFrame frame = {.type = WIRE_BEGIN, .session = session, .length = 10};
    write_le32(frame.payload, size);
    write_le32(frame.payload + 4, crc32_update(0, file, size));
    frame.payload[8] = 1;
    frame.payload[9] = 'x';
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
                            .length = (uint16_t)count};
        memcpy(frame.payload, file + offset, count);
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
    free(file);
    puts("native checks: ELF rejection, streaming, reset, framing, retries, cancellation passed");
    return 0;
}
