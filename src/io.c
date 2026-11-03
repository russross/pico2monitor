#include "monitor.h"

#include "tusb.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t read, write;
    uint8_t bytes[8192];
} OutputRing;
static OutputRing output;
static uint8_t input[2048];
static uint32_t input_read, input_write, input_lost, output_lost;
static bool was_connected, discard_batch;

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
    return sizeof(output.bytes) - (output.write - output.read);
}
bool console_empty(void) {
    return output.read == output.write && tud_cdc_n_write_available(0) == CFG_TUD_CDC_TX_BUFSIZE;
}

size_t console_write(const void *bytes, size_t count) {
    const uint8_t *p = bytes;
    if (count > console_space())
        count = console_space();
    for (size_t i = 0; i < count; ++i)
        output.bytes[output.write++ % sizeof(output.bytes)] = p[i];
    return count;
}

size_t guest_output_write(const void *bytes, size_t count) {
    /* Count guest bytes separately from the expanded terminal output. */
    const uint8_t *p = bytes;
    size_t space = console_space();
    space = space > 2048 ? space - 2048 : 0;
    size_t sent = 0;
    while (sent < count) {
        size_t needed = p[sent] == '\n' ? 2 : 1;
        if (space < needed)
            break;
        if (needed == 2)
            console_write("\r", 1);
        console_write(p + sent, 1);
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
            console_write("\b \b", 3);
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
        console_write("\r\n", 2);
    else if (byte != 4)
        console_write(&byte, 1);
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
    console_printf("io: input-dropped=%lu output-dropped=%lu queued=%lu\r\n",
                   (unsigned long)input_lost, (unsigned long)output_lost,
                   (unsigned long)(output.write - output.read));
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
        shell_disconnected();
    }
    if (!was_connected && connected)
        shell_init();
    was_connected = connected;

    /* Control keys precede ordinary input buffering, including under input pressure. */
    uint8_t bytes[256];
    uint32_t count = tud_cdc_n_read(0, bytes, sizeof(bytes));
    discard_batch = false;
    for (uint32_t i = 0; i < count && !discard_batch; ++i) {
        bool active = program.state == RUNNING || program.state == WAITING;
        if (active && (bytes[i] == 3 || bytes[i] == 27)) {
            program_suspend(bytes[i] == 3 ? "Ctrl-C" : "Escape");
            discard_batch = true;
        } else if (active)
            guest_input_put(bytes[i]);
        else {
            shell_input(bytes[i]);
            if (program.state == RUNNING || program.state == WAITING)
                discard_batch = true;
        }
    }

    /* Endpoint writes are short and never wait for an absent terminal reader. */
    size_t pending = output.write - output.read;
    if (pending && connected) {
        size_t position = output.read % sizeof(output.bytes);
        if (pending > sizeof(output.bytes) - position)
            pending = sizeof(output.bytes) - position;
        if (pending > 256)
            pending = 256;
        output.read += tud_cdc_n_write(0, output.bytes + position, (uint32_t)pending);
    }
    tud_cdc_n_write_flush(0);
    tud_cdc_n_write_flush(1);
}
