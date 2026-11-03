#include "loader.h"
#include "elf.h"

#include <string.h>

Loader loader;

bool loader_busy(void) {
    return loader.state == LOAD_CLEAR || loader.state == LOAD_SNAPSHOT;
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
    if (offset != loader.cursor || count > 1024 ||
        !range_contains(0, loader.image.file_size, offset, count)) {
        loader_abort("invalid loader stream position");
        return;
    }
    loader.cursor += count;
    if (loader.state == LOAD_HEADERS) {
        uint32_t target =
            loader.image.file_size < ELF_PREFIX_SIZE ? loader.image.file_size : ELF_PREFIX_SIZE;
        uint32_t prefix = target - offset;
        if (prefix > count)
            prefix = count;
        memcpy(loader.prefix + offset, bytes, prefix);
        if (offset + prefix < target)
            return;
        loader.error = elf_validate(loader.prefix, loader.image.file_size, &loader.image);
        if (loader.error) {
            loader.state = LOAD_DISCARD;
            return;
        }

        /* Only a fully verified layout can replace the suspended image. Clear
         * the whole guest arena so inaccessible padding cannot retain old data. */
        program_forget();
        loader.destructive = true;
        loader.work_cursor = GUEST_BASE;
        loader.state = LOAD_CLEAR;
        loader.pending_size = count - prefix;
        memcpy(loader.pending, bytes + prefix, loader.pending_size);
        console_printf("replacing previous SRAM image\r\n");
    } else if (loader.state == LOAD_STREAM)
        copy_intersections(offset, bytes, count);
}

void loader_step(void) {
    if (loader.state == LOAD_CLEAR) {
        memset(physical(loader.work_cursor), 0, 1024);
        loader.work_cursor += 1024;
        if (loader.work_cursor == GUEST_END) {
            loader.state = LOAD_STREAM;
            uint32_t prefix =
                loader.image.file_size < ELF_PREFIX_SIZE ? loader.image.file_size : ELF_PREFIX_SIZE;
            copy_intersections(0, loader.prefix, prefix);
            copy_intersections(prefix, loader.pending, loader.pending_size);
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
        monitor_fence_i();
        program_install(&loader.image);
        loader.state = LOAD_DONE;
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
    else if (loader.image.plan.snapshot) {
        loader.state = LOAD_SNAPSHOT;
        loader.work_cursor = 0;
    } else {
        monitor_fence_i();
        program_install(&loader.image);
        loader.state = LOAD_DONE;
    }
}
