#include "transfer.h"
#include "loader.h"
#include "wire.h"

#include <string.h>

typedef enum {
    TRANSFER_OFF,
    TRANSFER_ARMED,
    TRANSFER_DATA,
    TRANSFER_FINAL,
    TRANSFER_DONE
} TransferState;
static struct {
    TransferState state;
    uint64_t session, deadline, ready_at;
    uint32_t sequence, offset, file_size, expected_crc, running_crc, last_crc;
    bool previous_connected, ack_pending, overflow;
    size_t input_size, output_size, output_cursor;
    uint8_t input[WIRE_LIMIT], output[WIRE_LIMIT];
    WireFrame frame, result;
} transfer;

bool transfer_active(void) {
    return transfer.state == TRANSFER_ARMED || transfer.state == TRANSFER_DATA ||
           transfer.state == TRANSFER_FINAL;
}

static void send_frame(const WireFrame *frame) {
    transfer.output_size = wire_encode(frame, transfer.output);
    transfer.output_cursor = 0;
}

static void reply(WireType type) {
    WireFrame frame = {.type = type,
                       .session = transfer.session,
                       .sequence = transfer.sequence,
                       .offset = transfer.offset};
    send_frame(&frame);
}

static void result(void) {
    WireFrame *frame = &transfer.result;
    memset(frame, 0, sizeof(*frame));
    frame->type = WIRE_RESULT;
    frame->session = transfer.session;
    frame->sequence = transfer.sequence;
    frame->offset = transfer.offset;
    frame->payload[0] = loader.error != NULL;
    frame->payload[1] = !loader.destructive && program.valid;
    const char *message = loader.error ? loader.error : "loaded";
    size_t length = strlen(message);
    if (length > 160)
        length = 160;
    memcpy(frame->payload + 2, message, length);
    frame->length = (uint16_t)(length + 2);
    transfer.state = TRANSFER_DONE;
    transfer.ack_pending = false;
    send_frame(frame);
    if (loader.error)
        console_printf("load failed: %s (%s)\r\n", loader.error,
                       frame->payload[1] ? "previous image preserved" : "no runnable image");
    else {
        console_printf("loaded: %s; ready\r\n", program.image.name);
        memory_map_start();
    }
    shell_prompt();
}

void transfer_arm(void) {
    static uint32_t generation;
    memset(&transfer, 0, sizeof(transfer));
    transfer.state = TRANSFER_ARMED;
    transfer.session = (monitor_time_us() << 16) ^ ++generation;
    transfer.deadline = monitor_time_us() + UINT64_C(30000000);
    transfer.previous_connected = upload_connected();
    upload_flush_input();
    console_printf("waiting for ELF on upload port; Ctrl-C/Escape cancels\r\n");
}

void transfer_cancel(const char *reason) {
    if (!transfer_active())
        return;
    if (transfer.state == TRANSFER_ARMED)
        loader_begin(0, 0, "");
    loader_abort(reason);
    result();
}

static void begin(const WireFrame *frame) {
    if (frame->sequence != 0 || frame->offset != 0 || frame->length < 9 || frame->payload[8] > 63 ||
        frame->length != 9u + frame->payload[8]) {
        reply(WIRE_NAK);
        return;
    }
    uint32_t size = read_le32(frame->payload), crc = read_le32(frame->payload + 4);
    char name[64] = {0};
    for (uint32_t i = 0; i < frame->payload[8]; ++i) {
        uint8_t c = frame->payload[9 + i];
        name[i] = c >= 32 && c < 127 ? (char)c : '?';
    }
    loader_begin(size, crc, name);
    if (size < 52 || size > ELF_FILE_LIMIT) {
        loader_abort("file size outside ELF limits");
        result();
        return;
    }
    transfer.file_size = size;
    transfer.expected_crc = crc;
    transfer.sequence = 1;
    transfer.state = TRANSFER_DATA;
    transfer.deadline = monitor_time_us() + UINT64_C(10000000);
    reply(WIRE_ACK);
}

static void accept(const WireFrame *frame) {
    if (frame->session != transfer.session)
        return;
    if (transfer.state == TRANSFER_DONE) {
        if (frame->type == WIRE_END || frame->type == WIRE_ABORT)
            send_frame(&transfer.result);
        return;
    }
    if (!transfer_active())
        return;
    if (frame->type == WIRE_ABORT) {
        transfer_cancel("sender aborted upload");
        return;
    }
    if (transfer.state == TRANSFER_ARMED) {
        if (frame->type == WIRE_BEGIN)
            begin(frame);
        else
            reply(WIRE_NAK);
        return;
    }
    if (frame->type == WIRE_BEGIN && transfer.offset == 0 && frame->sequence == 0 &&
        frame->length >= 9 && read_le32(frame->payload) == transfer.file_size &&
        read_le32(frame->payload + 4) == transfer.expected_crc) {
        reply(WIRE_ACK);
        return;
    }

    /* Retries are acknowledged without writing memory or updating the file CRC twice. */
    if (frame->type == WIRE_DATA && frame->sequence + 1 == transfer.sequence &&
        frame->offset <= transfer.offset && frame->length == transfer.offset - frame->offset &&
        frame->crc == transfer.last_crc) {
        transfer.deadline = monitor_time_us() + UINT64_C(10000000);
        reply(WIRE_ACK);
        return;
    }
    if (frame->sequence != transfer.sequence || frame->offset != transfer.offset) {
        reply(WIRE_NAK);
        return;
    }
    if (frame->type == WIRE_DATA && frame->length &&
        frame->length <= transfer.file_size - transfer.offset) {
        loader_feed(frame->offset, frame->payload, frame->length);
        transfer.running_crc = crc32_update(transfer.running_crc, frame->payload, frame->length);
        transfer.offset += frame->length;
        ++transfer.sequence;
        transfer.last_crc = frame->crc;
        transfer.deadline = monitor_time_us() + UINT64_C(10000000);
        if (loader_busy())
            transfer.ack_pending = true;
        else
            reply(WIRE_ACK);
    } else if (frame->type == WIRE_END && frame->length == 0 &&
               transfer.offset == transfer.file_size) {
        if (transfer.running_crc != transfer.expected_crc)
            loader_abort("whole-file CRC mismatch");
        loader_end();
        transfer.state = TRANSFER_FINAL;
        transfer.deadline = monitor_time_us() + UINT64_C(10000000);
    } else
        reply(WIRE_NAK);
}

void transfer_step(void) {
    bool connected = upload_connected();
    if (transfer.previous_connected && !connected)
        transfer_cancel("upload port disconnected");
    transfer.previous_connected = connected;
    if (transfer_active() && monitor_time_us() >= transfer.deadline)
        transfer_cancel("upload timed out");
    if (transfer.state == TRANSFER_FINAL && !loader_busy())
        result();
    if (transfer.ack_pending && !loader_busy()) {
        transfer.ack_pending = false;
        reply(WIRE_ACK);
    }

    /* A partially written reply must finish before consuming another request. */
    if (transfer.output_cursor < transfer.output_size && connected) {
        transfer.output_cursor += upload_write(transfer.output + transfer.output_cursor,
                                               transfer.output_size - transfer.output_cursor);
    }
    if (transfer.output_cursor < transfer.output_size || transfer.ack_pending)
        return;
    if (transfer.state == TRANSFER_ARMED && connected && monitor_time_us() >= transfer.ready_at) {
        transfer.ready_at = monitor_time_us() + UINT64_C(1000000);
        reply(WIRE_READY);
        return;
    }
    for (unsigned budget = 0; budget < 256; ++budget) {
        uint8_t byte;
        if (!upload_read(&byte, 1))
            break;
        if (byte == 0) {
            bool valid = !transfer.overflow && transfer.input_size &&
                         wire_decode(transfer.input, transfer.input_size, &transfer.frame);
            transfer.input_size = 0;
            transfer.overflow = false;
            if (valid) {
                accept(&transfer.frame);
                break;
            }
        } else if (transfer.input_size < sizeof(transfer.input)) {
            transfer.input[transfer.input_size++] = byte;
        } else
            transfer.overflow = true;
    }
}
