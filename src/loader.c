#include "loader.h"
#include "elf.h"

#include <string.h>

Loader loader;

bool loader_busy(void) {
    return loader.state == LOAD_PLAN || loader.state == LOAD_CLEAR || loader.state == LOAD_SNAPSHOT;
}

void loader_begin(uint32_t size, uint32_t crc, const char *name) {
    memset(&loader, 0, sizeof(loader));
    loader.state = LOAD_HEADERS;
    loader.image.file_size = size;
    loader.image.crc = crc;
    size_t length = strlen(name);
    if (length > 63)
        length = 63;
    memcpy(loader.image.name, name, length);
}

/* Every file chunk, including the header prefix, uses this same intersection
 * path. Shared source bytes may legitimately initialize multiple destinations. */
static void copy_intersections(uint32_t offset, const uint8_t *bytes, uint32_t count) {
    for (uint32_t i = 0; i < loader.image.segment_count; ++i) {
        Segment *s = &loader.image.segments[i];
        uint32_t begin = offset > s->offset ? offset : s->offset;
        uint32_t end = offset + count;
        if (end > s->offset + s->file_size)
            end = s->offset + s->file_size;
        if (begin < end) {
            memcpy(physical(s->address + begin - s->offset), bytes + begin - offset, end - begin);
            s->received += end - begin;
        }
    }
}

void loader_feed(uint32_t offset, const uint8_t *bytes, uint32_t count) {
    if (loader.error) {
        loader.cursor += count;
        return;
    }
    if (offset != loader.cursor || !count || count > 1024 ||
        !range_contains(0, loader.image.file_size, offset, count)) {
        loader_abort("invalid loader stream position");
        return;
    }
    /* The first accepted payload owns the arena. Arming and BEGIN alone never
     * disturb the old program or its suspended execution context. */
    if (!loader.destructive) {
        program_forget();
        loader.destructive = true;
        loader.prefix = physical(GUEST_BASE);
        loader.upload = loader.prefix + ELF_PREFIX_SIZE;
        console_printf("replacing previous SRAM image\r\n");
    }
    memcpy(loader.upload, bytes, count);
    loader.cursor += count;
    if (loader.state == LOAD_HEADERS) {
        uint32_t target =
            loader.image.file_size < ELF_PREFIX_SIZE ? loader.image.file_size : ELF_PREFIX_SIZE;
        uint32_t prefix = target - offset;
        if (prefix > count)
            prefix = count;
        memcpy(loader.prefix + offset, loader.upload, prefix);
        if (offset + prefix < target)
            return;
        loader.error = elf_decode(loader.prefix, loader.image.file_size, &loader.image);
        if (loader.error) {
            loader.state = LOAD_DISCARD;
            return;
        }

        loader.replay_target = target;
        loader.replay_prefix = prefix;
        loader.replay_count = count;
        memory_plan_start(&loader.image, loader.image.plan.requested);
        loader.state = LOAD_PLAN;
    } else if (loader.state == LOAD_STREAM)
        copy_intersections(offset, loader.upload, count);
}

/* Clearing preserves the union of initialized destinations, including where
 * one segment's BSS overlaps another segment's file-backed content. */
static void clear_chunk(void) {
    uint32_t end = loader.work_cursor + 1024;
    if (end > GUEST_END)
        end = GUEST_END;
    for (uint32_t address = loader.work_cursor; address < end; ++address) {
        bool initialized = false;
        for (uint32_t i = 0; i < loader.image.segment_count; ++i) {
            const Segment *s = &loader.image.segments[i];
            initialized = initialized || range_contains(s->address, s->file_size, address, 1);
        }
        if (!initialized)
            *physical(address) = 0;
    }
    loader.work_cursor = end;
}

static void finish(void) {
    monitor_fence_i();
    program_install(&loader.image);
    loader.state = LOAD_DONE;
}

void loader_step(void) {
    if (loader.state == LOAD_PLAN) {
        if (!memory_plan_step(&loader.error))
            return;
        if (loader.error) {
            loader.state = LOAD_DISCARD;
            return;
        }
        /* Live input moves above every segment destination only after the
         * planner has finished using disposable arena memory. */
        uint8_t *buffers = physical(loader.image.plan.stack_base);
        memmove(buffers, loader.prefix, ELF_PREFIX_SIZE + 1024);
        loader.prefix = buffers;
        loader.upload = buffers + ELF_PREFIX_SIZE;
        loader.state = LOAD_STREAM;
        copy_intersections(0, loader.prefix, loader.replay_target);
        copy_intersections(loader.replay_target, loader.upload + loader.replay_prefix,
                           loader.replay_count - loader.replay_prefix);
    } else if (loader.state == LOAD_CLEAR) {
        clear_chunk();
        if (loader.work_cursor == GUEST_END) {
            loader.prefix = loader.upload = NULL;
            if (loader.image.plan.snapshot) {
                loader.state = LOAD_SNAPSHOT;
                loader.work_cursor = 0;
            } else
                finish();
        }
    } else if (loader.state == LOAD_SNAPSHOT) {
        uint32_t position = loader.work_cursor;
        for (uint32_t i = 0; i < loader.image.segment_count; ++i) {
            const Segment *s = &loader.image.segments[i];
            if (!(s->permissions & PERM_W))
                continue;
            if (position >= s->file_size) {
                position -= s->file_size;
                continue;
            }
            uint32_t count = s->file_size - position;
            if (count > 1024)
                count = 1024;
            memcpy(physical(loader.image.plan.snapshot_base + loader.work_cursor),
                   physical(s->address + position), count);
            loader.work_cursor += count;
            return;
        }
        finish();
    }
}

void loader_abort(const char *error) {
    if (!loader.error)
        loader.error = error;
    if (loader.destructive)
        program_forget();
    loader.state = LOAD_DISCARD;
}

void loader_end(void) {
    if (!loader.error && loader.cursor != loader.image.file_size)
        loader_abort("truncated ELF stream");
    if (!loader.error && loader.state != LOAD_STREAM)
        loader_abort("incomplete ELF initialization");
    for (uint32_t i = 0; i < loader.image.segment_count && !loader.error; ++i)
        if (loader.image.segments[i].received != loader.image.segments[i].file_size)
            loader_abort("incomplete ELF segment");
    if (loader.error)
        loader.state = LOAD_DONE;
    else {
        loader.state = LOAD_CLEAR;
        loader.work_cursor = GUEST_BASE;
    }
}
