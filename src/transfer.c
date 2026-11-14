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
    bool previous_connected, ack_pending, overflow, reset, report_pending, result_preserved;
    size_t input_size, output_size, output_cursor;
    uint16_t guest_output_pending;
    uint8_t input[WIRE_LIMIT], output[192];
    WireFrame frame;
} transfer;

/* Upload and guest frames share storage; ownership survives program replacement. */
static struct {
    uint64_t session, deadline;
    GuestNotice notice, next_notice;
    const char *release_reason;
    bool attached, releasing;
} guest;

bool guest_attached(void) {
    return guest.attached;
}
bool guest_transport_idle(void) {
    return guest_output_empty() && transfer.output_cursor == transfer.output_size;
}
bool guest_handoff_pending(void) {
    return guest.releasing || guest.notice.kind != GUEST_NOTICE_NONE;
}
GuestNotice guest_take_notice(void) {
    GuestNotice notice = guest.notice;
    guest.notice = guest.next_notice;
    guest.next_notice = (GuestNotice){0};
    return notice;
}
void guest_disconnect(const char *reason) {
    if (!guest.attached || guest.releasing)
        return;
    guest.releasing = true;
    guest.release_reason = reason;
    program_suspend(reason);
}
const char *guest_merge(void) {
    if (!guest.attached)
        return NULL;
    if (program.state == ACTIVE || program.state == STOPPING || !program_quiescent())
        return "suspend the guest before merging I/O";
    if (!guest_transport_idle())
        return "guest output is draining; retry io merge";
    guest_disconnect("guest terminal merged");
    return NULL;
}

bool transfer_active(void) {
    return transfer.state == TRANSFER_ARMED || transfer.state == TRANSFER_DATA ||
           transfer.state == TRANSFER_FINAL;
}

static void send_frame(const WireFrame *frame) {
    transfer.output_size = wire_encode(frame, transfer.output, sizeof(transfer.output));
    transfer.output_cursor = 0;
}

static void guest_reply(WireType type, uint64_t session, const char *message) {
    WireFrame frame = {.type = type,
                       .session = session,
                       .payload = (const uint8_t *)message,
                       .length = message ? (uint16_t)strlen(message) : 0};
    send_frame(&frame);
}

/* A claim acknowledges the final upload result and retires its replay buffer. */
static bool guest_accept(const WireFrame *frame) {
    if (frame->type == WIRE_CLAIM) {
        const char *error = NULL;
        if (!frame->session || frame->length)
            error = "invalid guest claim";
        else if (guest.attached && guest.session != frame->session)
            error = "guest I/O already attached";
        else if (!guest.attached && guest.session == frame->session)
            error = "guest session expired";
        else if (guest.releasing || (!guest.attached && guest.notice.kind != GUEST_NOTICE_NONE))
            error = "I/O change pending";
        else if (guest.attached && transfer_active())
            error = "upload in progress";
        else if (!guest.attached &&
                 (transfer_active() || program.reset_busy || program.state == ACTIVE ||
                  program.state == STOPPING || !program_quiescent() || !shell_idle() ||
                  !console_connected() || !console_empty() || !guest_output_empty()))
            error = "guest claim requires an idle monitor and suspended guest";
        if (error) {
            guest_reply(WIRE_ERROR, frame->session, error);
            return true;
        }
        if (!guest.attached) {
            guest.session = frame->session;
            guest.attached = true;
            guest_input_clear();
            guest.notice = (GuestNotice){.kind = GUEST_NOTICE_ATTACHED};
        }
        transfer.state = TRANSFER_OFF;
        guest.deadline = monitor_time_us() + UINT64_C(5000000);
        guest_reply(WIRE_ATTACHED, guest.session, NULL);
        return true;
    }
    if (frame->type < WIRE_INPUT)
        return false;
    if (!guest.attached || frame->session != guest.session) {
        if (frame->type == WIRE_RELEASE && guest.session && frame->session == guest.session)
            guest_reply(WIRE_RELEASED, frame->session, NULL);
        return true;
    }
    guest.deadline = monitor_time_us() + UINT64_C(5000000);
    if (frame->type == WIRE_RELEASE)
        guest_disconnect("guest terminal detached");
    else if (frame->type == WIRE_INPUT && !transfer_active() && !guest.releasing &&
             program.state == ACTIVE) {
        for (unsigned i = 0; i < frame->length; ++i) {
            if (frame->payload[i] == 3 || frame->payload[i] == 27) {
                program_suspend(frame->payload[i] == 3 ? "Ctrl-C" : "Escape");
                break;
            }
            guest_input_put(frame->payload[i]);
        }
    }
    return true;
}

static void reply(WireType type) {
    WireFrame frame = {.type = type,
                       .session = transfer.session,
                       .sequence = transfer.sequence,
                       .offset = transfer.offset};
    send_frame(&frame);
}

static void encode_result(void) {
    uint8_t payload[162];
    payload[0] = loader.error != NULL;
    payload[1] = transfer.result_preserved;
    const char *message = loader.error ? loader.error : "loaded";
    size_t length = strlen(message);
    if (length > 160)
        length = 160;
    memcpy(payload + 2, message, length);
    WireFrame frame = {.type = WIRE_RESULT,
                       .session = transfer.session,
                       .sequence = transfer.sequence,
                       .offset = transfer.offset,
                       .payload = payload,
                       .length = (uint16_t)(length + 2)};
    send_frame(&frame);
}

bool transfer_report(void) {
    if (!transfer.report_pending)
        return false;
    transfer.report_pending = false;
    if (loader.error)
        console_printf("load failed: %s (%s)\r\n", loader.error,
                       transfer.result_preserved ? "previous image preserved"
                                                 : "no runnable image");
    else {
        const MemoryPlan *p = &program.image.plan;
        console_printf(
            "loaded: %s; ready; file=%lu bytes, heap=%lu KiB, stack=%lu KiB, reset=%s\r\n",
            program.image.name, (unsigned long)program.image.file_size,
            (unsigned long)((p->heap_limit - p->heap_base) / 1024),
            (unsigned long)((p->stack_top - p->stack_base) / 1024),
            p->snapshot ? "available" : "unavailable");
    }
    return true;
}

static void result(void) {
    transfer.state = TRANSFER_DONE;
    transfer.ack_pending = false;
    transfer.result_preserved = !loader.destructive && program.valid;
    transfer.report_pending = true;
    encode_result();
    shell_prompt();
}

void transfer_arm(bool reset) {
    static uint32_t generation;
    memset(&transfer, 0, sizeof(transfer));
    transfer.state = TRANSFER_ARMED;
    transfer.reset = reset;
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
    loader.image.plan.requested = transfer.reset ? SNAP_AUTO : SNAP_OFF;
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
    if (guest_accept(frame))
        return;
    if (frame->session != transfer.session)
        return;
    if (transfer.state == TRANSFER_DONE) {
        /* Rebuild RESULT if an intervening control reply reused its encoded storage. */
        if (frame->type == WIRE_END || frame->type == WIRE_ABORT)
            encode_result();
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
    if (guest.attached &&
        (!connected || !console_connected() || monitor_time_us() >= guest.deadline))
        guest_disconnect(!connected             ? "guest connection lost"
                         : !console_connected() ? "monitor connection lost"
                                                : "guest heartbeat expired");
    if (guest.releasing && program_quiescent() && program.state != STOPPING) {
        transfer_cancel("guest terminal disconnected during upload");
        guest_output_discard(transfer.guest_output_pending);
        transfer.guest_output_pending = 0;
        guest_input_clear();
        guest.attached = guest.releasing = false;
        GuestNotice notice = {.kind = GUEST_NOTICE_MERGED, .reason = guest.release_reason};
        if (guest.notice.kind != GUEST_NOTICE_NONE)
            guest.next_notice = notice;
        else
            guest.notice = notice;
        transfer.state = TRANSFER_OFF;
        transfer.input_size = 0;
        transfer.overflow = false;
        upload_flush_input();
        transfer.output_size = transfer.output_cursor = 0;
        if (connected)
            guest_reply(WIRE_RELEASED, guest.session, NULL);
    }
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
        if (transfer.output_cursor == transfer.output_size)
            transfer.guest_output_pending = 0;
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
    /* Guest output uses the upload reply buffer only after the upload phase ends. */
    if (guest.attached && !guest.releasing && transfer.state == TRANSFER_OFF &&
        transfer.output_cursor == transfer.output_size) {
        const uint8_t *bytes;
        size_t count = guest_output_peek(&bytes);
        if (count > 128)
            count = 128;
        if (count) {
            WireFrame frame = {.type = WIRE_OUTPUT,
                               .session = guest.session,
                               .payload = bytes,
                               .length = (uint16_t)count};
            send_frame(&frame);
            transfer.guest_output_pending = (uint16_t)count;
            guest_output_consume(count);
        }
    }
}
