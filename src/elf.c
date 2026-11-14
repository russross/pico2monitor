#include "elf.h"
#include "wire.h"

#include <string.h>

static bool file_range(uint32_t offset, uint32_t count, uint32_t size) {
    return offset <= size && count <= size - offset;
}

const char *elf_decode(const uint8_t *p, uint32_t size, Image *image) {
    if (size < 52 || size > ELF_FILE_LIMIT)
        return "file size outside ELF limits";
    if (memcmp(p, "\177ELF", 4) || p[4] != 1 || p[5] != 1 || p[6] != 1 || p[7] || p[8])
        return "expected little-endian ELF32 System V";
    if (read_le16(p + 16) != 2 || read_le16(p + 18) != 243 || read_le32(p + 20) != 1)
        return "expected static RISC-V ET_EXEC";
    if (read_le16(p + 40) != 52 || read_le16(p + 42) != 32 || (read_le32(p + 36) & ~1u))
        return "unsupported ELF headers or ABI flags";

    /* Header parsing finishes before segment initialization. Metadata may share file
     * bytes with a load segment; the streaming loader replays the saved prefix. */
    uint32_t table = read_le32(p + 28), count = read_le16(p + 44);
    uint32_t available = size < ELF_PREFIX_SIZE ? size : ELF_PREFIX_SIZE;
    if (!count || count > MAX_SEGMENTS || table < 52 || !file_range(table, count * 32, available))
        return "program headers must fit in first 4096 bytes";
    image->segment_count = 0;
    image->entry = read_le32(p + 24);
    image->file_size = size;
    image->phoff = table;
    image->phnum = count;
    bool entry = false, stack = false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *h = p + table + i * 32;
        uint32_t type = read_le32(h), offset = read_le32(h + 4);
        uint32_t address = read_le32(h + 8), files = read_le32(h + 16);
        uint32_t memory = read_le32(h + 20), flags = read_le32(h + 24);
        uint32_t alignment = read_le32(h + 28);
        if (!file_range(offset, files, size))
            return "ELF file range overflows file";
        if (alignment > 1 &&
            ((alignment & (alignment - 1)) || ((address - offset) & (alignment - 1))))
            return "invalid ELF segment alignment";
        if (type == UINT32_C(0x6474e551)) {
            if (stack || flags != 6 || memory > STACK_SIZE)
                return "unsupported GNU_STACK request";
            stack = true;
            continue;
        }
        if (type != 1) {
            if (type != 0 && type != 4 && type != 6 && type != UINT32_C(0x70000003) &&
                type != UINT32_C(0x6474e552) && type != UINT32_C(0x6474e553))
                return "unsupported runtime program header (dynamic linking or TLS)";
            continue;
        }
        if (flags > 7)
            return "unsupported ELF permissions";
        if (files > memory || !range_contains(GUEST_BASE, GUEST_END - GUEST_BASE, address, memory))
            return "load segment outside guest SRAM";
        if (!memory)
            continue;
        Segment s = {.address = address,
                     .memory_size = memory,
                     .offset = offset,
                     .file_size = files,
                     .permissions = (uint8_t)flags};
        for (uint32_t j = 0; j < image->segment_count; ++j) {
            const Segment *t = &image->segments[j];
            uint32_t begin = address > t->address ? address : t->address;
            uint32_t end = address + files < t->address + t->file_size ? address + files
                                                                       : t->address + t->file_size;
            if (begin < end && offset + begin - address != t->offset + begin - t->address)
                return "overlapping segments have conflicting initialization";
        }
        image->segments[image->segment_count++] = s;
        if ((flags & PERM_X) && !(image->entry & 1) &&
            range_contains(address, files, image->entry, 2))
            entry = true;
    }
    if (!entry)
        return "entry is not in initialized executable text";
    return NULL;
}

const char *elf_validate(const uint8_t *p, uint32_t size, Image *image) {
    const char *error = elf_decode(p, size, image);
    return error ? error : memory_plan_build(image, image->plan.requested);
}
