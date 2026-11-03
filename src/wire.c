#include "wire.h"

#include <string.h>

uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}
uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
void write_le32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(value >> (8 * i));
}

/* The public CRC is finalized, matching binascii.crc32 across incremental calls. */
uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t count) {
    crc = ~crc;
    for (size_t i = 0; i < count; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
    }
    return ~crc;
}

size_t wire_encode(const WireFrame *frame, uint8_t *output) {
    uint8_t raw[24 + WIRE_PAYLOAD];
    if (frame->length > WIRE_PAYLOAD)
        return 0;
    raw[0] = 1;
    raw[1] = (uint8_t)frame->type;
    write_le32(raw + 2, (uint32_t)frame->session);
    write_le32(raw + 6, (uint32_t)(frame->session >> 32));
    write_le32(raw + 10, frame->sequence);
    write_le32(raw + 14, frame->offset);
    raw[18] = (uint8_t)frame->length;
    raw[19] = (uint8_t)(frame->length >> 8);
    memcpy(raw + 20, frame->payload, frame->length);
    size_t size = 20 + frame->length;
    write_le32(raw + size, crc32_update(0, raw, size));
    size += 4;

    /* Include a leading delimiter so reconnecting readers discard partial frames. */
    output[0] = 0;
    size_t out = 2, code_at = 1;
    uint8_t code = 1;
    for (size_t i = 0; i < size; ++i) {
        if (raw[i] == 0) {
            output[code_at] = code;
            code_at = out++;
            code = 1;
        } else {
            output[out++] = raw[i];
            if (++code == 255) {
                output[code_at] = code;
                code_at = out++;
                code = 1;
            }
        }
    }
    output[code_at] = code;
    output[out++] = 0;
    return out;
}

bool wire_decode(const uint8_t *input, size_t size, WireFrame *frame) {
    uint8_t raw[24 + WIRE_PAYLOAD];
    size_t out = 0, in = 0;
    while (in < size) {
        uint8_t code = input[in++];
        if (code == 0 || (size_t)(code - 1) > size - in)
            return false;
        for (unsigned i = 1; i < code; ++i) {
            if (out == sizeof(raw))
                return false;
            raw[out++] = input[in++];
        }
        if (code != 255 && in < size) {
            if (out == sizeof(raw))
                return false;
            raw[out++] = 0;
        }
    }

    /* All lengths are checked before a decoded payload reaches a consumer. */
    if (out < 24 || raw[0] != 1 || raw[1] < WIRE_READY || raw[1] > WIRE_RESULT)
        return false;
    uint16_t count = read_le16(raw + 18);
    if (count > WIRE_PAYLOAD || out != (size_t)count + 24)
        return false;
    uint32_t crc = read_le32(raw + out - 4);
    if (crc != crc32_update(0, raw, out - 4))
        return false;
    frame->type = (WireType)raw[1];
    frame->session = read_le32(raw + 2) | (uint64_t)read_le32(raw + 6) << 32;
    frame->sequence = read_le32(raw + 10);
    frame->offset = read_le32(raw + 14);
    frame->length = count;
    memcpy(frame->payload, raw + 20, count);
    frame->crc = crc;
    return true;
}
