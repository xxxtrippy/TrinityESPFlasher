/*
 * picolink.h — talk to a PicoFlasher over a USB-C OTG cable.
 *
 * The ESP32-S3 acts as a USB host and speaks the PicoFlasher's simple CDC
 * command protocol (VID 600D / PID 7001), so all the fragile Xbox SB wiring is
 * handled by the proven PicoFlasher instead of the ESP's own pins.
 *
 * GPL-3.0. See LICENSE.
 */
#ifndef PICOLINK_H
#define PICOLINK_H

#include <stdbool.h>
#include <stdint.h>

/* Bring up the USB host stack and start watching for a PicoFlasher. Safe to
 * call once at boot; returns false only if the host stack itself won't install
 * (e.g. the OTG peripheral is already owned by the TinyUSB device driver). */
bool picolink_start(void);

/* True once a PicoFlasher has enumerated and its data endpoints are open. */
bool picolink_present(void);

/* PicoFlasher firmware version (GET_VERSION). */
bool picolink_get_version(uint32_t *ver);

/* Flash config word (GET_FLASH_CONFIG). The PicoFlasher halts the console SMC
 * itself at power-up, so this is just a read. */
bool picolink_get_flash_config(uint32_t *cfg);

/* One 16 KB-page read/write: buf must hold 0x210 bytes (0x200 data + 0x10
 * spare), exactly as xbox_nand_*_block lay it out. Returns the PicoFlasher's
 * status word (0 = OK) or a negative value on a USB/transport failure. */
int picolink_read_block(uint32_t lba, uint8_t *buf210);
int picolink_write_block(uint32_t lba, const uint8_t *buf210);

/* Short human-readable state for the screen ("USB: PicoFlasher v3", ...). */
const char *picolink_status(void);

#endif /* PICOLINK_H */
