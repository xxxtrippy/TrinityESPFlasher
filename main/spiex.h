#ifndef SPIEX_H
#define SPIEX_H

#include <stdint.h>

void spiex_init(void);

/* Park the bus for the SMC-stop handshake: SS high, CLK/MOSI driven low. */
void spiex_bus_hold(void);

/* Bit-bang half-clock delay in microseconds (default 1). */
void spiex_set_half_us(int us);
void spiex_deinit(void);

uint32_t spiex_read_reg(uint8_t reg);

/* Diagnostics for a dead link (call after spiex_init()).
 * spiex_probe_miso(): with SS deasserted, is MISO 'F' floating (follows the
 * pull resistor), 'L' held low or 'H' held high by something else?
 * spiex_read_raw(): the 6 raw bytes clocked in while reading a register. */
char spiex_probe_miso(void);
void spiex_read_raw(uint8_t reg, uint8_t rx[6]);
void spiex_write_reg(uint8_t reg, uint32_t val);

#endif
