#define _GNU_SOURCE
#include "rp2350js-c.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

/* This host adapter drives two real emulated USB CDC functions, not firmware hooks. */
typedef struct {
    int master, slave;
    char name[128];
    uint8_t input[65536], output[65536];
    uint32_t in_read, in_write, out_read, out_write;
    int pending;
} Port;
static RP2350 *chip;
static Port ports[2];
static volatile sig_atomic_t stopping;
static unsigned enumeration_state, descriptor_size, descriptor_count;

static void stop(int signal_number) {
    (void)signal_number;
    stopping = 1;
}

static void setup(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint16_t length) {
    uint8_t packet[8] = {type, request, value, value >> 8, index, index >> 8, length, length >> 8};
    RPUSBController_sendSetupPacket(chip->usbCtrl, packet);
}

static void enabled(void *context) {
    (void)context;
    RPUSBController_resetDevice(chip->usbCtrl);
}
static void reset_received(void *context) {
    (void)context;
    enumeration_state = 1;
    descriptor_count = descriptor_size = 0;
    setup(0, 5, 1, 0, 0);
}

static void deliver(unsigned index) {
    Port *p = &ports[index];
    if (!p->pending || p->in_write == p->in_read)
        return;
    uint8_t packet[64];
    unsigned size = p->in_write - p->in_read;
    if (size > sizeof(packet))
        size = sizeof(packet);
    if (size > (unsigned)p->pending)
        size = (unsigned)p->pending;
    for (unsigned i = 0; i < size; ++i)
        packet[i] = p->input[p->in_read++ % sizeof(p->input)];
    p->pending = 0;
    RPUSBController_endpointReadDone(chip->usbCtrl, index ? 4 : 2, packet, (int32_t)size, 1000);
}

static void endpoint_read(void *context, int32_t endpoint, int32_t length) {
    (void)context;
    if (endpoint == 2 || endpoint == 4) {
        unsigned index = endpoint == 4;
        ports[index].pending = length;
        deliver(index);
    }
}

static void endpoint_write(void *context, int32_t endpoint, uint8_t *bytes, int32_t length) {
    (void)context;
    if (endpoint == 2 || endpoint == 4) {
        Port *p = &ports[endpoint == 4];
        if ((unsigned)length > sizeof(p->output) - (p->out_write - p->out_read)) {
            fprintf(stderr, "emulator host output overflow; terminal is not draining\n");
            stopping = 1;
            return;
        }
        for (int i = 0; i < length; ++i)
            p->output[p->out_write++ % sizeof(p->output)] = bytes[i];
        return;
    }
    if (endpoint != 0)
        return;

    /* Enumeration mirrors the emulator's CDC helper but addresses both functions. */
    if (enumeration_state == 1 && length == 0) {
        enumeration_state = 2;
        setup(0x80, 6, 0x200, 0, 9);
    } else if (enumeration_state == 2 && length == 9) {
        descriptor_size = bytes[2] | (unsigned)bytes[3] << 8;
        if (descriptor_size > 256) {
            stopping = 1;
            return;
        }
        enumeration_state = 3;
        setup(0x80, 6, 0x200, 0, (uint16_t)descriptor_size);
    } else if (enumeration_state == 3 && length > 0) {
        descriptor_count += (unsigned)length;
        if (descriptor_count == descriptor_size) {
            enumeration_state = 4;
            setup(0, 9, 1, 0, 0);
        }
    } else if (enumeration_state == 4 && length == 0) {
        enumeration_state = 5;
        setup(0x21, 0x22, 3, 0, 0);
    } else if (enumeration_state == 5 && length == 0) {
        enumeration_state = 6;
        setup(0x21, 0x22, 3, 2, 0);
    } else if (enumeration_state == 6 && length == 0) {
        enumeration_state = 7;
        fprintf(stderr, "USB: both CDC interfaces enumerated\n");
    }
}

static bool port_open(Port *p) {
    p->master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (p->master < 0 || grantpt(p->master) || unlockpt(p->master))
        return false;
    const char *name = ptsname(p->master);
    if (!name || strlen(name) >= sizeof(p->name))
        return false;
    strcpy(p->name, name);
    p->slave = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (p->slave < 0)
        return false;
    struct termios attributes;
    if (tcgetattr(p->slave, &attributes))
        return false;
    cfmakeraw(&attributes);
    return tcsetattr(p->slave, TCSANOW, &attributes) == 0;
}

static void port_poll(Port *p, unsigned index) {
    uint8_t buffer[256];
    size_t space = sizeof(p->input) - (p->in_write - p->in_read);
    if (space > sizeof(buffer))
        space = sizeof(buffer);
    ssize_t count = space ? read(p->master, buffer, space) : 0;
    if (count > 0)
        for (ssize_t i = 0; i < count; ++i)
            p->input[p->in_write++ % sizeof(p->input)] = buffer[i];
    deliver(index);
    size_t size = p->out_write - p->out_read;
    size_t position = p->out_read % sizeof(p->output);
    if (size > sizeof(p->output) - position)
        size = sizeof(p->output) - position;
    count = size ? write(p->master, p->output + position, size) : 0;
    if (count > 0)
        p->out_read += (uint32_t)count;
}

static bool instruction_check(void) {
    /* ORC.B must test every bit of each byte, including ordinary ASCII low bits. */
    static CPU cpu;
    const uint32_t inputs[] = {0, 0x01020304, 0x00010000, 0x80000000, 0x00ff007f};
    const uint32_t expected[] = {0, 0xffffffff, 0x00ff0000, 0xff000000, 0x00ff00ff};
    for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        cpu.regs[10] = (int32_t)inputs[i];
        CPU_execOpImm(&cpu, T_ORC_B, 11, 10, 0);
        if ((uint32_t)cpu.regs[11] != expected[i])
            return false;
    }
    return true;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s firmware.hex\n", argv[0]);
        return 2;
    }
    if (!instruction_check()) {
        fprintf(stderr, "emulator ORC.B regression: apply rp2350js-c.patch\n");
        return 1;
    }
    if (!port_open(&ports[0]) || !port_open(&ports[1])) {
        perror("create emulator serial ports");
        return 1;
    }
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    RP2350Options options = {.coreArch = "riscv", .loadFirmware = argv[1]};
    chip = RP2350_new(&options);
    chip->usbCtrl->onUSBEnabled_fn = enabled;
    chip->usbCtrl->onResetReceived_fn = reset_received;
    chip->usbCtrl->onEndpointRead_fn = endpoint_read;
    chip->usbCtrl->onEndpointWrite_fn = endpoint_write;
    printf("{\"console\":\"%s\",\"upload\":\"%s\"}\n", ports[0].name, ports[1].name);
    fflush(stdout);
    while (!stopping) {
        for (unsigned i = 0; i < 1000; ++i)
            RP2350_step(chip);
        port_poll(&ports[0], 0);
        port_poll(&ports[1], 1);
    }
    for (unsigned i = 0; i < 2; ++i) {
        close(ports[i].master);
        close(ports[i].slave);
    }
    return 0;
}
