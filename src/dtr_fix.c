/*
 * DTR Fix for TinyUSB CDC on ESP32-S3
 *
 * Problem: Some OBD2 scanning software (e.g., ScanMaster) opens the serial
 * port without asserting DTR. TinyUSB's tud_cdc_n_connected() returns
 * _cdcd_itf.dtr which is false, causing tud_cdc_n_write_flush() to
 * silently drop all outgoing data (SLCAN responses).
 *
 * Solution: Use linker --wrap to redirect tud_cdc_n_connected() to always
 * return true. This makes TinyUSB think the host is always connected,
 * allowing data flow regardless of DTR state.
 */

#include <stdint.h>
#include <stdbool.h>

bool __wrap_tud_cdc_n_connected(uint8_t itf) {
    (void)itf;
    return true;
}
