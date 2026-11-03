#include "monitor.h"

#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "tusb.h"

#include <stdio.h>

/* This optional SRAM-only target diagnoses startup without replacing board flash. */
int main(void) {
    uint32_t reason = watchdog_hw->reason;
    uint32_t saved[4];
    for (unsigned i = 0; i < 4; ++i)
        saved[i] = watchdog_hw->scratch[i];
    watchdog_enable(3000, true);
    tusb_init();
    bool greeted = false;
    for (;;) {
        tud_task_ext(0, false);
        watchdog_update();
        if (tud_cdc_n_connected(0) && !greeted) {
            uint32_t status, pmpm, cfg0, cfg1;
            __asm volatile("csrr %0, mstatus\ncsrr %1, 0xbd0\ncsrr %2, pmpcfg0\ncsrr %3, pmpcfg1"
                           : "=r"(status), "=r"(pmpm), "=r"(cfg0), "=r"(cfg1));
            char text[384];
            int count = snprintf(
                text, sizeof(text),
                "SRAM startup diagnostic; watchdog=%08lx scratch=%08lx %08lx %08lx %08lx\r\n"
                "mstatus=%08lx pmpm=%08lx cfg0=%08lx cfg1=%08lx\r\n"
                "p: apply PMP; b: BOOTSEL\r\n",
                (unsigned long)reason, (unsigned long)saved[0], (unsigned long)saved[1],
                (unsigned long)saved[2], (unsigned long)saved[3], (unsigned long)status,
                (unsigned long)pmpm, (unsigned long)cfg0, (unsigned long)cfg1);
            tud_cdc_n_write(0, text, (uint32_t)count);
            greeted = true;
        }
        char byte;
        if (tud_cdc_n_read(0, &byte, 1)) {
            if (byte == 'b')
                reset_usb_boot(0, 0);
            if (byte == 'p') {
                watchdog_hw->scratch[3] = 0xd1000001;
                pmp_init();
                watchdog_hw->scratch[3] = 0xd1000002;
                tud_cdc_n_write_str(0, "PMP initialization returned\r\n");
            }
        }
        tud_cdc_n_write_flush(0);
    }
}
