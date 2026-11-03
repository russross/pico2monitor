#include "pico/unique_id.h"
#include "tusb.h"

#include <string.h>

/* Each CDC function has a distinct notification endpoint and a bulk pair. */
static const tusb_desc_device_t device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = 64,
    .idVendor = 0x2e8a,
    .idProduct = 0x000a,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};
enum { CONFIG_SIZE = TUD_CONFIG_DESC_LEN + 2 * TUD_CDC_DESC_LEN };
static const uint8_t configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, CONFIG_SIZE, 0, 100),
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, 64),
    TUD_CDC_DESCRIPTOR(2, 5, 0x83, 8, 0x04, 0x84, 64),
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&device;
}
const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return configuration;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t language) {
    (void)language;
    static uint16_t descriptor[64];
    static char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    const char *value;
    if (index == 0) {
        descriptor[0] = (TUSB_DESC_STRING << 8) | 4;
        descriptor[1] = 0x0409;
        return descriptor;
    }
    if (index == 1)
        value = "Pico monitor";
    else if (index == 2)
        value = "RV32 SRAM monitor";
    else if (index == 3) {
        pico_get_unique_board_id_string(serial, sizeof(serial));
        value = serial;
    } else if (index == 4)
        value = "Monitor console";
    else if (index == 5)
        value = "ELF upload";
    else
        return NULL;
    size_t length = strlen(value);
    if (length > 63)
        length = 63;
    descriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * length + 2));
    for (size_t i = 0; i < length; ++i)
        descriptor[i + 1] = (uint8_t)value[i];
    return descriptor;
}
