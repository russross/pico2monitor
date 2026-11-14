#include "wire.h"

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

size_t wire_encode(const WireFrame *frame, uint8_t *output, size_t capacity) {
    size_t size = 24 + frame->length;
    if (frame->length > WIRE_PAYLOAD || (frame->length && !frame->payload) ||
        capacity < size + size / 254 + 3)
        return 0;
    uint8_t header[20], checksum[4];
    header[0] = 1;
    header[1] = (uint8_t)frame->type;
    write_le32(header + 2, (uint32_t)frame->session);
    write_le32(header + 6, (uint32_t)(frame->session >> 32));
    write_le32(header + 10, frame->sequence);
    write_le32(header + 14, frame->offset);
    header[18] = (uint8_t)frame->length;
    header[19] = (uint8_t)(frame->length >> 8);
    uint32_t crc = crc32_update(0, header, sizeof(header));
    write_le32(checksum, crc32_update(crc, frame->payload, frame->length));

    /* Include a leading delimiter so reconnecting readers discard partial frames. */
    output[0] = 0;
    size_t out = 2, code_at = 1;
    uint8_t code = 1;
    for (size_t i = 0; i < size; ++i) {
        uint8_t byte = i < 20                    ? header[i]
                       : i < 20u + frame->length ? frame->payload[i - 20]
                                                 : checksum[i - 20 - frame->length];
        if (byte == 0) {
            output[code_at] = code;
            code_at = out++;
            code = 1;
        } else {
            output[out++] = byte;
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

bool wire_decode(uint8_t *input, size_t size, WireFrame *frame) {
    size_t out = 0, in = 0;
    while (in < size) {
        uint8_t code = input[in++];
        if (code == 0 || (size_t)(code - 1) > size - in)
            return false;
        for (unsigned i = 1; i < code; ++i) {
            if (out == 24 + WIRE_PAYLOAD)
                return false;
            input[out++] = input[in++];
        }
        if (code != 255 && in < size) {
            if (out == 24 + WIRE_PAYLOAD)
                return false;
            input[out++] = 0;
        }
    }

    /* All lengths are checked before a decoded payload reaches a consumer. */
    if (out < 24 || input[0] != 1 || input[1] < WIRE_READY || input[1] > WIRE_RESULT)
        return false;
    uint16_t count = read_le16(input + 18);
    if (count > WIRE_PAYLOAD || out != (size_t)count + 24)
        return false;
    uint32_t crc = read_le32(input + out - 4);
    if (crc != crc32_update(0, input, out - 4))
        return false;
    frame->type = (WireType)input[1];
    frame->session = read_le32(input + 2) | (uint64_t)read_le32(input + 6) << 32;
    frame->sequence = read_le32(input + 10);
    frame->offset = read_le32(input + 14);
    frame->length = count;
    frame->payload = input + 20;
    frame->crc = crc;
    return true;
}
