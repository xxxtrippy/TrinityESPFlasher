/*
 * spiex.c — Xbox 360 Southbridge flash SPI, BIT-BANGED on GPIO.
 *
 * The ESP32-S3 hardware SPI path (SPI2/FSPI + polling transfers) read back all
 * zeros on this board, so the Xbox SB bus is driven by a plain GPIO bit-bang
 * instead. This removes every hardware-SPI variable (IOMUX routing, DMA vs
 * FIFO, input-delay/sampling) and mirrors how the RP2040 PicoFlasher drives the
 * bus. The register protocol is identical to PicoFlasher's.
 *
 * Wire format: SPI mode 0 (CLK idles low; MOSI set up while CLK low; both ends
 * sample on the rising edge; slave shifts on the falling edge) and LSB-FIRST
 * on the wire. We shift LSB-first directly, so no bit-reversal table is needed:
 * sending byte B bit0-first puts B on the wire LSB-first, and assembling the
 * received bits bit0-first yields the real byte — exactly what the hardware
 * path produced after its lsb2msb round-trip.
 *
 * GPL-3.0. See LICENSE.
 */
#include "spiex.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"   /* esp_rom_delay_us */

#include "pins.h"

static bool pfc_spi_inited;

/* Half-clock delay. 0 relies on gpio call overhead (~1-2 MHz, reliable on short
 * dupont). Raise (e.g. 1) if long/marginal wiring needs a slower, safer clock;
 * the flash-config read is only 6 bytes so speed only matters for full dumps. */
#ifndef SPIEX_BB_HALF_US
#define SPIEX_BB_HALF_US 0
#endif

static inline void bb_delay(void)
{
#if SPIEX_BB_HALF_US > 0
	esp_rom_delay_us(SPIEX_BB_HALF_US);
#endif
}

void spiex_init(void)
{
	gpio_config_t out = {
		.intr_type = GPIO_INTR_DISABLE,
		.mode = GPIO_MODE_OUTPUT,
		.pin_bit_mask = (1ULL << SPI_SS_N) | (1ULL << SPI_MOSI) | (1ULL << SPI_CLK),
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.pull_up_en = GPIO_PULLUP_DISABLE,
	};
	gpio_config(&out);

	gpio_config_t in = {
		.intr_type = GPIO_INTR_DISABLE,
		.mode = GPIO_MODE_INPUT,
		.pin_bit_mask = (1ULL << SPI_MISO),
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.pull_up_en = GPIO_PULLUP_ENABLE,   /* so an undriven line never reads 0 */
	};
	gpio_config(&in);

	gpio_set_level(SPI_CLK, 0);    /* mode 0: idle low */
	gpio_set_level(SPI_MOSI, 0);
	gpio_set_level(SPI_SS_N, 1);   /* deasserted */

	pfc_spi_inited = true;
}

void spiex_deinit(void)
{
	if (!pfc_spi_inited) {
		return;
	}
	/* Release the bus: everything back to hi-Z inputs so the SMC owns it. */
	gpio_config_t hiz = {
		.intr_type = GPIO_INTR_DISABLE,
		.mode = GPIO_MODE_INPUT,
		.pin_bit_mask = (1ULL << SPI_SS_N) | (1ULL << SPI_MOSI) |
		                (1ULL << SPI_CLK) | (1ULL << SPI_MISO),
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.pull_up_en = GPIO_PULLUP_DISABLE,
	};
	gpio_config(&hiz);
	pfc_spi_inited = false;
}

/* Full-duplex transfer of one byte, LSB-first, SPI mode 0. */
static inline uint8_t bb_xfer(uint8_t out)
{
	uint8_t in = 0;
	for (int i = 0; i < 8; i++) {
		gpio_set_level(SPI_MOSI, (out >> i) & 1);  /* set up while CLK low */
		bb_delay();
		gpio_set_level(SPI_CLK, 1);                /* rising edge: sample */
		bb_delay();
		if (gpio_get_level(SPI_MISO)) {
			in |= (uint8_t)(1u << i);
		}
		gpio_set_level(SPI_CLK, 0);                /* falling edge: slave shifts */
	}
	return in;
}

uint32_t spiex_read_reg(uint8_t reg)
{
	uint8_t txbuf[] = {(uint8_t)((reg << 2) | 1), 0xFF, 0x00, 0x00, 0x00, 0x00};
	uint8_t rxbuf[sizeof(txbuf)];

	gpio_set_level(SPI_SS_N, 0);
	for (int i = 0; i < (int)sizeof(txbuf); i++) {
		rxbuf[i] = bb_xfer(txbuf[i]);
	}
	gpio_set_level(SPI_SS_N, 1);

	uint32_t v;
	v = (uint32_t)rxbuf[2] | ((uint32_t)rxbuf[3] << 8) |
	    ((uint32_t)rxbuf[4] << 16) | ((uint32_t)rxbuf[5] << 24);
	return v;
}

void spiex_write_reg(uint8_t reg, uint32_t val)
{
	uint8_t txbuf[] = {
		(uint8_t)((reg << 2) | 2),
		(uint8_t)(val & 0xFF),
		(uint8_t)((val >> 8) & 0xFF),
		(uint8_t)((val >> 16) & 0xFF),
		(uint8_t)((val >> 24) & 0xFF),
	};

	gpio_set_level(SPI_SS_N, 0);
	for (int i = 0; i < (int)sizeof(txbuf); i++) {
		(void)bb_xfer(txbuf[i]);
	}
	gpio_set_level(SPI_SS_N, 1);
}
