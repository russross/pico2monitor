#include "debug.h"
#include "monitor.h"
#include "transfer.h"

#include "tusb.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t read, write;
} OutputRing;
static OutputRing output, guest_output;
/* Only the payload occupies scratch SRAM; empty indices make old bytes inert. */
static uint8_t output_bytes[4096] __attribute__((section(".monitor_output")));
static uint8_t input[512];
static uint32_t input_read, input_write, input_lost, output_lost;
static bool was_connected;

void io_init(void) {
    tusb_init();
}
bool console_connected(void) {
    return tud_cdc_n_connected(0);
}
bool upload_connected(void) {
    return tud_cdc_n_connected(1);
}
size_t console_space(void) {
    return 2048 - (output.write - output.read);
}
bool console_empty(void) {
    return output.read == output.write && (guest_attached() || guest_output_empty()) &&
           tud_cdc_n_write_available(0) == CFG_TUD_CDC_TX_BUFSIZE;
}
void console_input_clear(void) {
    tud_cdc_n_read_flush(0);
}
bool guest_output_empty(void) {
    return guest_output.read == guest_output.write;
}
size_t guest_output_peek(const uint8_t **bytes) {
    size_t position = guest_output.read % 2048;
    size_t count = guest_output.write - guest_output.read;
    if (count > 2048 - position)
        count = 2048 - position;
    *bytes = output_bytes + 2048 + position;
    return count;
}
void guest_output_consume(size_t count) {
    guest_output.read += count;
}
void guest_output_discard(size_t staged) {
    output_lost += guest_output.write - guest_output.read + staged;
    guest_output.read = guest_output.write;
}

size_t console_write(const void *bytes, size_t count) {
    const uint8_t *p = bytes;
    if (count > console_space())
        count = console_space();
    for (size_t i = 0; i < count; ++i)
        output_bytes[output.write++ % 2048] = p[i];
    return count;
}

size_t guest_output_write(const void *bytes, size_t count) {
    /* Count guest bytes separately from the expanded terminal output. */
    const uint8_t *p = bytes;
    size_t space = 2048 - (guest_output.write - guest_output.read);
    size_t sent = 0;
    while (sent < count) {
        size_t needed = p[sent] == '\n' ? 2 : 1;
        if (space < needed)
            break;
        if (needed == 2)
            output_bytes[2048 + guest_output.write++ % 2048] = '\r';
        output_bytes[2048 + guest_output.write++ % 2048] = p[sent];
        space -= needed;
        ++sent;
    }
    return sent;
}

void console_printf(const char *format, ...) {
    char buffer[384];
    va_list args;
    va_start(args, format);
    int size = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (size < 0)
        return;
    size_t count = (size_t)size;
    if (count >= sizeof(buffer)) {
        output_lost += count - sizeof(buffer) + 1;
        count = sizeof(buffer) - 1;
    }
    output_lost += count - console_write(buffer, count);
}

void guest_input_clear(void) {
    input_read = input_write = 0;
}
bool guest_input_put(uint8_t byte) {
    /* Editing affects only the unfinished line; completed lines remain readable. */
    if (byte == 127 || byte == 8) {
        if (input_write != input_read && input[(input_write - 1) % sizeof(input)] != '\n' &&
            input[(input_write - 1) % sizeof(input)] != 4) {
            --input_write;
            guest_output_write("\b \b", 3);
        }
        return true;
    }
    if (byte == '\r')
        byte = '\n';
    if (input_write - input_read >= sizeof(input) - (byte == '\n' || byte == 4 ? 0 : 1)) {
        ++input_lost;
        return false;
    }
    input[input_write++ % sizeof(input)] = byte;
    if (byte == '\n')
        guest_output_write("\n", 1);
    else if (byte != 4)
        guest_output_write(&byte, 1);
    return true;
}
int32_t guest_input_read(void *bytes, size_t count) {
    uint32_t available = input_write - input_read;
    uint32_t line = 0;
    while (line < available && input[(input_read + line) % sizeof(input)] != '\n' &&
           input[(input_read + line) % sizeof(input)] != 4)
        ++line;
    if (line == available)
        return -1;
    bool eof = input[(input_read + line) % sizeof(input)] == 4;
    if (!eof)
        ++line;
    if (count > line)
        count = line;
    uint8_t *p = bytes;
    for (size_t i = 0; i < count; ++i)
        p[i] = input[input_read++ % sizeof(input)];
    if (eof && count == line)
        ++input_read;
    return (int32_t)count;
}

void io_status(void) {
    console_printf(
        "io: input-dropped=%lu output-dropped=%lu queued=%lu\r\n", (unsigned long)input_lost,
        (unsigned long)output_lost,
        (unsigned long)(output.write - output.read + guest_output.write - guest_output.read));
}
size_t upload_read(void *bytes, size_t count) {
    return tud_cdc_n_read(1, bytes, (uint32_t)count);
}
size_t upload_write(const void *bytes, size_t count) {
    return tud_cdc_n_write(1, bytes, (uint32_t)count);
}
void upload_flush_input(void) {
    tud_cdc_n_read_flush(1);
}

void io_service(void) {
    tud_task_ext(0, false);
    bool connected = console_connected();
    if (was_connected && !connected) {
        output_lost += output.write - output.read;
        output.read = output.write;
        tud_cdc_n_write_clear(0);
        if (!guest_attached())
            guest_output_discard(0);
        shell_disconnected();
    }
    if (!was_connected && connected)
        shell_init();
    was_connected = connected;

    /* Control keys precede ordinary input buffering, including under input pressure. */
    for (unsigned i = 0; i < 256; ++i) {
        bool active = program.state == ACTIVE;
        bool combined = active && !guest_attached();
        uint8_t byte;
        bool urgent =
            (active || debug_active()) && tud_cdc_n_peek(0, &byte) && (byte == 3 || byte == 27);
        if (!urgent &&
            ((!combined && !transfer_active() && !shell_accepting()) || console_space() < 32))
            break;
        if (!tud_cdc_n_read(0, &byte, 1))
            break;
        if (active && (byte == 3 || byte == 27)) {
            program_suspend(byte == 3 ? "Ctrl-C" : "Escape");
            tud_cdc_n_read_flush(0);
            break;
        } else if (combined)
            guest_input_put(byte);
        else
            shell_input(byte);
    }

    /* Endpoint writes are short and never wait for an absent terminal reader. */
    size_t pending = output.write - output.read;
    if (pending && connected) {
        size_t position = output.read % 2048;
        if (pending > 2048 - position)
            pending = 2048 - position;
        if (pending > 256)
            pending = 256;
        output.read += tud_cdc_n_write(0, output_bytes + position, (uint32_t)pending);
    }
    if (!pending && connected && !guest_attached()) {
        const uint8_t *data;
        size_t count = guest_output_peek(&data);
        if (count > 256)
            count = 256;
        guest_output_consume(tud_cdc_n_write(0, data, (uint32_t)count));
    }
    tud_cdc_n_write_flush(0);
    tud_cdc_n_write_flush(1);
}
