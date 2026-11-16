/*
 * xbox.c — Xbox 360 Southbridge NAND + eMMC flash controller protocol.
 *
 * Ported to ESP32-S3 for TrinityWeb360. The register command sequences,
 * block-size detection, status polling, and SMC stop/start timing below are
 * the known-good values from:
 *   GxOSS/espflasher (GPL-3.0)  <- balika011/PicoFlasher
 * Credits: balika011, 15432 (eMMC SPI), Hax360, ExposureMG (ESP32 port).
 *
 * This file is GPL-3.0. See LICENSE.
 */
#include "xbox.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pins.h"

static const char *XTAG = "xbox";
#include "spiex.h"

static void sleep_ms(int ms)
{
	if (ms <= 0) {
		return;
	}
	vTaskDelay(pdMS_TO_TICKS(ms));
}

void xbox_init(void)
{
	/*
	 * Match the PROVEN X360Tools/PicoFlasher (RP2040) idle state exactly — it is
	 * known-good on this console/wiring:
	 *   SMC_DBG_EN    = 1
	 *   SMC_RST_XDK_N = 1
	 *   SPI_SS_N      = 1   (configured as an output HERE so it is driven during
	 *                        the xbox_stop_smc() handshake, before spiex_init()).
	 * The SPI peripheral itself is brought up at the END of xbox_stop_smc().
	 */
	/* INPUT_OUTPUT so we can read the real pad level back for diagnostics. */
	gpio_config_t cfg = {
		.intr_type = GPIO_INTR_DISABLE,
		.mode = GPIO_MODE_INPUT_OUTPUT,
		.pin_bit_mask = (1ULL << SMC_DBG_EN) | (1ULL << SMC_RST_XDK_N) | (1ULL << SPI_SS_N),
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.pull_up_en = GPIO_PULLUP_DISABLE,
	};
	gpio_config(&cfg);

	gpio_set_level(SMC_DBG_EN, 1);
	gpio_set_level(SMC_RST_XDK_N, 1);
	gpio_set_level(SPI_SS_N, 1);

	/* Diagnostic: confirm the chip can actually drive these high. If a readback
	 * is 0 right after driving 1, the pad is held low externally (short / strong
	 * pulldown) or the pin can't drive. */
	ESP_LOGW(XTAG, "init readback DBG_EN(%d)=%d RST(%d)=%d SS(%d)=%d (want 1,1,1)",
	         SMC_DBG_EN, gpio_get_level(SMC_DBG_EN),
	         SMC_RST_XDK_N, gpio_get_level(SMC_RST_XDK_N),
	         SPI_SS_N, gpio_get_level(SPI_SS_N));
}

bool xbox_smc_stopped = false;

void xbox_start_smc(void)
{
	/* Release the SPI bus first (PicoFlasher does this) so the ESP stops
	 * driving CLK/MOSI before the SMC takes the bus back — otherwise the two
	 * contend and the console can't boot after a flash. */
	spiex_deinit();

	gpio_set_level(SMC_DBG_EN, 0);
	gpio_set_level(SMC_RST_XDK_N, 0);

	sleep_ms(50);

	gpio_set_level(SMC_RST_XDK_N, 1);

	xbox_smc_stopped = false;
}

void xbox_stop_smc(void)
{
	/* X360Tools/PicoFlasher sequence. First make sure SS is a driven output
	 * (spiex_deinit() leaves it hi-Z after a flash) and CLK/MOSI are held low
	 * instead of floating while SS is asserted. */
	spiex_bus_hold();

	gpio_set_level(SMC_DBG_EN, 0);
	sleep_ms(50);

	gpio_set_level(SPI_SS_N, 0);
	gpio_set_level(SMC_RST_XDK_N, 0);
	sleep_ms(50);

	gpio_set_level(SMC_DBG_EN, 1);
	gpio_set_level(SMC_RST_XDK_N, 1);
	sleep_ms(50);

	gpio_set_level(SPI_SS_N, 1);
	sleep_ms(50);

	/* Diagnostic: after the handshake DBG_EN and RST must be high (SMC halted).
	 * If either reads 0 here, the pad is held low externally or isn't driving. */
	ESP_LOGW(XTAG, "stop_smc readback DBG_EN=%d RST=%d (want 1,1)",
	         gpio_get_level(SMC_DBG_EN), gpio_get_level(SMC_RST_XDK_N));

	spiex_init();

	xbox_smc_stopped = true;
}

static uint32_t xbox_cached_flash_config;

uint32_t xbox_get_flash_config(void)
{
	if (!xbox_smc_stopped) {
		xbox_cached_flash_config = 0;
		return 0;
	}
	xbox_cached_flash_config = spiex_read_reg(0);
	if ((xbox_cached_flash_config & 0xF0000000) == 0xC0000000) {
		xbox_cached_flash_config = 0xC0462002;
	}
	return xbox_cached_flash_config;
}

/* Is this wire tied to something? Release the pin and see whether it follows
 * the ESP's own weak pulls ('F': floating - not connected, or the far end is
 * hi-Z) or is held 'L' / 'H' by the console. */
static char probe_wire(int pin)
{
	gpio_set_direction(pin, GPIO_MODE_INPUT);
	gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
	esp_rom_delay_us(500);
	int lo = gpio_get_level(pin);
	gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
	esp_rom_delay_us(500);
	int hi = gpio_get_level(pin);
	gpio_set_pull_mode(pin, GPIO_FLOATING);

	if (!lo && hi) return 'F';
	if (!lo && !hi) return 'L';
	if (lo && hi) return 'H';
	return '?';
}

void xbox_link_diag(char *out, size_t out_sz)
{
	if (!xbox_smc_stopped) {
		snprintf(out, out_sz, "no link: SMC not stopped");
		return;
	}
	uint8_t rx[6];
	spiex_read_raw(0, rx);
	uint32_t cfg = (uint32_t)rx[2] | ((uint32_t)rx[3] << 8) | ((uint32_t)rx[4] << 16) | ((uint32_t)rx[5] << 24);
	int dbg = gpio_get_level(SMC_DBG_EN), rst = gpio_get_level(SMC_RST_XDK_N);

	/* Bench self-test: with the console unplugged and MOSI jumpered to MISO the
	 * ESP reads back exactly what it sent (01 FF 00 00 00 00). */
	if (rx[0] == 0x01 && rx[1] == 0xFF && cfg == 0) {
		snprintf(out, out_sz, "loopback OK: ESP pins work");
		ESP_LOGW(XTAG, "link diag: MOSI->MISO loopback detected, ESP side is fine");
		return;
	}

	/* Every wire released in turn: SS, CLK, MOSI, MISO, DBG_EN, RST. The SMC
	 * reset line has a pull-up on the console, so RST must read 'H' if that
	 * wire really reaches the board. */
	static const int pins[6] = { SPI_SS_N, SPI_CLK, SPI_MOSI, SPI_MISO, SMC_DBG_EN, SMC_RST_XDK_N };
	char w[7];
	for (int i = 0; i < 6; i++) w[i] = probe_wire(pins[i]);
	w[6] = 0;

	/* Can each output pin really drive its pad? Drive 1 then 0 and read the pad
	 * back: 'K' ok, 'h' stuck high, 'l' stuck low. MISO is an input: its slot
	 * is 'J' if it follows MOSI (jumper fitted), else '-'. */
	char d[7];
	for (int i = 0; i < 6; i++) {
		int p = pins[i];
		if (p == SPI_MISO) { d[i] = '-'; continue; }
		gpio_set_pull_mode(p, GPIO_FLOATING);
		gpio_set_direction(p, GPIO_MODE_INPUT_OUTPUT);
		gpio_set_level(p, 1); esp_rom_delay_us(200); int hi = gpio_get_level(p);
		int m1 = gpio_get_level(SPI_MISO);
		gpio_set_level(p, 0); esp_rom_delay_us(200); int lo = gpio_get_level(p);
		int m0 = gpio_get_level(SPI_MISO);
		d[i] = (hi && !lo) ? 'K' : (hi ? 'h' : 'l');
		if (p == SPI_MOSI && m1 && !m0) d[3] = 'J';
		gpio_set_direction(p, GPIO_MODE_INPUT);
	}
	if (d[3] != 'J') d[3] = '-';
	d[6] = 0;

	/* Put the pins back and repeat the handshake so the console is left in the
	 * same state as after a normal detect (SMC halted, if the wiring allows). */
	xbox_init();
	xbox_stop_smc();

	/* Idle levels the firmware leaves on each pad, read back in the same order
	 * (SS CLK MOSI MISO DBG RST). The control lines SS and RST are active-low,
	 * so a healthy idle is 1 0 0 1 1 1: SS deasserted and RST released high. If
	 * SS or RST read 0 here the SMC is held selected/in-reset and cannot answer. */
	char idle[7];
	for (int i = 0; i < 6; i++) idle[i] = gpio_get_level(pins[i]) ? '1' : '0';
	idle[6] = 0;

	(void)dbg; (void)rst; (void)w;
	snprintf(out, out_sz, "nl %02X%02X %08lX d=%s i=%s", rx[0], rx[1], (unsigned long)cfg, d, idle);
	ESP_LOGW(XTAG, "link diag: drive=%s idle(SS CLK MOSI MISO DBG RST)=%s", d, idle);
}

static uint16_t xbox_nand_get_status(void)
{
	return spiex_read_reg(0x04);
}

static void xbox_nand_clear_status(void)
{
	spiex_write_reg(0x04, spiex_read_reg(0x04));
}

static int xbox_nand_wait_ready(uint16_t timeout)
{
	do {
		if (!(xbox_nand_get_status() & 0x01)) {
			return 0;
		}
	} while (timeout--);

	return 1;
}

int xbox_nand_read_block(uint32_t lba, uint8_t *buffer, uint8_t *spare)
{
	if (!xbox_smc_stopped || !xbox_cached_flash_config) {
		return 0x8000;  /* not ready: fail loudly instead of returning silent zeros */
	}

	xbox_nand_clear_status();

	spiex_write_reg(0x0C, lba << 9);
	spiex_write_reg(0x08, 0x03);

	if (xbox_nand_wait_ready(0x1000)) {
		return 0x8000 | xbox_nand_get_status();
	}

	spiex_write_reg(0x0C, 0);

	uint8_t *end = buffer + 0x200;
	while (buffer < end) {
		spiex_write_reg(0x08, 0x00);

		uint32_t v = spiex_read_reg(0x10);
		memcpy(buffer, &v, 4);
		buffer += 4;
	}

	end = spare + 0x10;
	while (spare < end) {
		spiex_write_reg(0x08, 0x00);

		uint32_t v = spiex_read_reg(0x10);
		memcpy(spare, &v, 4);
		spare += 4;
	}

	return 0;
}

int xbox_nand_erase_block(uint32_t lba)
{
	if (!xbox_smc_stopped || !xbox_cached_flash_config) {
		return 0x8000;  /* not ready: fail loudly instead of returning silent zeros */
	}

	xbox_nand_clear_status();

	spiex_write_reg(0x00, spiex_read_reg(0x00) | 0x08);

	spiex_write_reg(0x0C, lba << 9);

	spiex_write_reg(0x08, 0xAA);
	spiex_write_reg(0x08, 0x55);
	spiex_write_reg(0x08, 0x05);

	if (xbox_nand_wait_ready(0x1000)) {
		return 0x8000 | xbox_nand_get_status();
	}

	return 0;
}

int xbox_nand_write_block(uint32_t lba, uint8_t *buffer, uint8_t *spare)
{
	if (!xbox_smc_stopped || !xbox_cached_flash_config) {
		return 0x8000;  /* not ready: fail loudly instead of returning silent zeros */
	}

	int major = (xbox_cached_flash_config >> 17) & 3;
	int minor = (xbox_cached_flash_config >> 4) & 3;

	int blocksize = 0x4000;
	if (major >= 1) {
		if (minor == 2) {
			blocksize = 0x20000;
		} else if (minor == 3) {
			blocksize = 0x40000;
		}
	}

	int sectors_in_block = blocksize / 0x200;

	if (lba % sectors_in_block == 0) {
		int ret = xbox_nand_erase_block(lba);
		if (ret) {
			return ret;
		}
	}

	xbox_nand_clear_status();

	spiex_write_reg(0x0C, 0);

	uint8_t *end = buffer + 0x200;
	while (buffer < end) {
		uint32_t v;
		memcpy(&v, buffer, 4);
		spiex_write_reg(0x10, v);
		spiex_write_reg(0x08, 0x01);
		buffer += 4;
	}

	end = spare + 0x10;
	while (spare < end) {
		uint32_t v;
		memcpy(&v, spare, 4);
		spiex_write_reg(0x10, v);
		spiex_write_reg(0x08, 0x01);
		spare += 4;
	}

	if (xbox_nand_wait_ready(0x1000)) {
		return 0x8000 | xbox_nand_get_status();
	}

	spiex_write_reg(0x0C, lba << 9);

	if (xbox_nand_wait_ready(0x1000)) {
		return 0x8000 | xbox_nand_get_status();
	}

	spiex_write_reg(0x08, 0x55);
	spiex_write_reg(0x08, 0xAA);
	spiex_write_reg(0x08, 0x04);

	if (xbox_nand_wait_ready(0x1000)) {
		return 0x8000 | xbox_nand_get_status();
	}

	return 0;
}

#define SD_OK (0)
#define SD_ERR_TIMEOUT (-1)

static uint32_t xbox_emmc_get_ints(void)
{
	return spiex_read_reg(0x30);
}

static void xbox_emmc_clear_ints(uint32_t value)
{
	spiex_write_reg(0x30, value);
}

static void xbox_emmc_clear_all_ints(void)
{
	xbox_emmc_clear_ints(xbox_emmc_get_ints());
}

static int xbox_emmc_wait_ints(uint32_t value, int timeout_ms)
{
	int64_t start = esp_timer_get_time();
	while ((esp_timer_get_time() - start) < (int64_t)timeout_ms * 1000) {
		uint32_t ints = xbox_emmc_get_ints();
		if ((ints & value) == value) {
			return SD_OK;
		}
	}
	return SD_ERR_TIMEOUT;
}

static void xbox_emmc_execute(uint32_t reg_4, uint32_t reg_8, uint32_t reg_c)
{
	xbox_emmc_clear_all_ints();
	spiex_write_reg(0x04, reg_4);
	spiex_write_reg(0x08, reg_8);
	spiex_write_reg(0x0C, reg_c);
}

int xbox_emmc_init(void)
{
	spiex_write_reg(0x2C, spiex_read_reg(0x2C) | (1 << 24));
	int64_t start = esp_timer_get_time();
	while ((esp_timer_get_time() - start) < 5000LL * 1000) {
		if (spiex_read_reg(0x3C) & 0x1000000) {
			return SD_OK;
		}
	}
	return SD_ERR_TIMEOUT;
}

static int xbox_emmc_deselect_card(void)
{
	xbox_emmc_execute(0, 0, 0x7000000);
	return xbox_emmc_wait_ints(1, 100);
}

static int xbox_emmc_read_cid_csd(uint8_t *buf, int is_cid)
{
	xbox_emmc_execute(0, 0xffff0000, is_cid ? 0x9010000 : 0xA010000);
	int ret = xbox_emmc_wait_ints(1, 100);
	if (!ret) {
		for (int i = 0x10; i < 0x20; i += 4) {
			uint32_t data = spiex_read_reg(i);
			memcpy(buf, &data, 4);
			buf += 4;
		}
	}
	return ret;
}

int xbox_emmc_read_cid(uint8_t *cid)
{
	return xbox_emmc_read_cid_csd(cid, 1);
}

int xbox_emmc_read_csd(uint8_t *csd)
{
	return xbox_emmc_read_cid_csd(csd, 0);
}

static int xbox_emmc_select_card(void)
{
	xbox_emmc_execute(0, 0xffff0000, 0x71a0000);
	return xbox_emmc_wait_ints(1, 100);
}

static int xbox_emmc_set_blocklen(int blocklen)
{
	xbox_emmc_execute(0x200, blocklen, 0x101a0000);
	return xbox_emmc_wait_ints(1, 100);
}

static int xbox_emmc_read_block_ext_csd(uint8_t *buf, int block, int is_block)
{
	int ret = xbox_emmc_select_card();
	if (ret) {
		return ret;
	}
	if (is_block) {
		ret = xbox_emmc_set_blocklen(0x200);
		if (ret) {
			xbox_emmc_deselect_card();
			return ret;
		}
	}
	xbox_emmc_execute(0x10200, block << 9, is_block ? 0x113a0010 : 0x83A0010);
	ret = xbox_emmc_wait_ints(0x21, 1500);
	if (!ret) {
		for (int i = 0; i < 0x200; i += 4) {
			uint32_t data = spiex_read_reg(0x20);
			memcpy(buf + i, &data, 4);
		}
	}
	xbox_emmc_deselect_card();
	return ret;
}

int xbox_emmc_read_ext_csd(uint8_t *ext_csd)
{
	return xbox_emmc_read_block_ext_csd(ext_csd, 0, 0);
}

int xbox_emmc_read_block(int lba, uint8_t *buf)
{
	return xbox_emmc_read_block_ext_csd(buf, lba, 1);
}

int xbox_emmc_write_block(int lba, uint8_t *buf)
{
	int ret = xbox_emmc_select_card();
	if (ret) {
		return ret;
	}
	ret = xbox_emmc_set_blocklen(0x200);
	if (ret) {
		return ret;
	}
	xbox_emmc_execute(0x10200, lba << 9, 0x183a0000);
	ret = xbox_emmc_wait_ints(1, 100);
	if (!ret) {
		for (int i = 0; i < 0x200; i += 4) {
			uint32_t data;
			memcpy(&data, buf + i, 4);
			spiex_write_reg(0x20, data);
		}
		ret = xbox_emmc_wait_ints(0x12, 1500);
	}
	xbox_emmc_deselect_card();
	return ret;
}
