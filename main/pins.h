#ifndef PFC_PINS_H
#define PFC_PINS_H

/*
 * Pin map for ESP32-S3 N16R8.
 *
 * IMPORTANT constraints on this module:
 *  - GPIO35, GPIO36, GPIO37 are used by the octal PSRAM on N16R8 — DO NOT USE.
 *  - GPIO0 (boot), GPIO45 (VDD_SPI), GPIO46 (strap) avoided.
 *  - GPIO19/20 are native USB D-/D+ — reserved for the Phase-2 J-Runner USB
 *    device emulation; do not reuse for the Xbox header.
 *
 * Xbox 360 Trinity connects via the J2C1 header (same points the RP2040
 * PicoFlasher uses). Logic is 3.3V on both sides, so no level shifting.
 * Common ground is mandatory.
 */

/* Status LED (S3-DevKitC-1 addressable LED is GPIO48; a plain LED works too) */
#define PFC_LED_PIN 2

/* --- Xbox 360 SB flash SPI (J2C1 header) --- */
#define SPI_MISO 13
#define SPI_MOSI 11
#define SPI_CLK  12
#define SPI_SS_N 10

/* --- Xbox 360 SMC control --- */
#define SMC_DBG_EN    14 /* SMC_DBG_EN  (J2C1) */
#define SMC_RST_XDK_N 21 /* SMC_RST_XDK_N (J2C1) */

/*
 * --- microSD card (SPI mode) ---
 * The common blue "V474"-style microSD breakout exposes only the SPI pins
 * (3v3, CS, MOSI, CLK, MISO, GND) — no SDMMC data lines — so the card is
 * driven over a dedicated SPI bus (SPI3_HOST), separate from the Xbox SB bus
 * on SPI2_HOST. The module is 3.3V native: power its 3v3 pin from ESP 3V3.
 * Plenty fast for sequential reads of the xeBuild ingredient files.
 *
 * Module pin -> ESP32-S3 GPIO:
 *   3v3  -> 3V3
 *   CS   -> GPIO4
 *   MOSI -> GPIO6
 *   CLK  -> GPIO5
 *   MISO -> GPIO7
 *   GND  -> GND
 */
/* SD card: own hardware SPI bus (SPI3). */
#define SD_CS    4
#define SD_CLK   5
#define SD_MOSI  6
#define SD_MISO  7

/*
 * --- 2.4" ILI9341 touch LCD on its OWN software (bit-banged) SPI bus ---
 * The S3 has only two hardware SPI hosts (Xbox SB = SPI2, SD = SPI3), so the
 * LCD and its XPT2046 touch run on a separate bit-banged bus with its own
 * CLK/MOSI/MISO lines, shared only between the two of them. All 3.3V.
 */
#define LCD_CLK   39   /* software SPI clock  (LCD + touch) */
#define LCD_MOSI  40   /* software SPI MOSI   (LCD + touch) */
#define LCD_MISO  41   /* software SPI MISO   (touch only)  */
#define LCD_CS    15
#define LCD_DC    16
#define LCD_RST   17
#define LCD_BL    18

#define TOUCH_CS  8
#define TOUCH_IRQ 9

/*
 * --- Two push buttons (replace the touch screen) ---
 * Wire each button between its GPIO and GND. No resistor needed: the internal
 * pull-up is enabled, so a press reads LOW.
 *   RIGHT  : moves the highlight FND -> DMP -> BLD -> FLSH -> back to FND
 *   SELECT : runs the highlighted action
 */
#define PIN_BTN_RIGHT  47
#define PIN_BTN_SELECT 48

#endif
