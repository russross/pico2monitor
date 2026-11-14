#ifndef MONITOR_WIRE_H
#define MONITOR_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* COBS gives a bounded recovery point after any damaged binary packet. */
#define WIRE_PAYLOAD 1024u
#define WIRE_LIMIT 1056u
typedef enum {
    WIRE_READY = 1,
    WIRE_BEGIN,
    WIRE_DATA,
    WIRE_END,
    WIRE_ABORT,
    WIRE_ACK,
    WIRE_NAK,
    WIRE_RESULT
} WireType;
/* Payload storage is borrowed; encoding consumes it synchronously. */
typedef struct {
    WireType type;
    uint64_t session;
    uint32_t sequence, offset;
    uint16_t length;
    const uint8_t *payload;
    uint32_t crc;
} WireFrame;
uint16_t read_le16(const uint8_t *p);
uint32_t read_le32(const uint8_t *p);
void write_le32(uint8_t *p, uint32_t value);
uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t count);
size_t wire_encode(const WireFrame *frame, uint8_t *output, size_t capacity);
/* Decoding consumes the encoded bytes; the payload view lasts until buffer reuse. */
bool wire_decode(uint8_t *input, size_t size, WireFrame *frame);

#endif
