/*
 * touch.c — XPT2046 resistive touch over bit-banged SPI (softspi.c).
 *
 * GPL-3.0. See LICENSE.
 *
 * Shares the software SPI bus with the LCD, on its own CS. Calibration constants
 * map raw 12-bit ADC to 320x240 landscape; adjust if taps land off-target.
 */
#include "touch.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "pins.h"
#include "softspi.h"
#include "lcd.h"

static const char *TAG = "touch";

#define RAW_X_MIN 300
#define RAW_X_MAX 3800
#define RAW_Y_MIN 300
#define RAW_Y_MAX 3800
#define SWAP_XY   1
#define INVERT_X  0
#define INVERT_Y  1

static inline void cs_low(void)  { gpio_set_level(TOUCH_CS, 0); }
static inline void cs_high(void) { gpio_set_level(TOUCH_CS, 1); }

bool touch_init(void)
{
    softspi_init(); /* idempotent-ish; shares pins with the LCD */

    gpio_config_t cs = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << TOUCH_CS,
    };
    gpio_config(&cs);
    cs_high();

    gpio_config_t irq = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pin_bit_mask = 1ULL << TOUCH_IRQ,
    };
    gpio_config(&irq);

    ESP_LOGI(TAG, "XPT2046 ready (soft-SPI)");
    return true;
}

static int read_axis(uint8_t cmd)
{
    cs_low();
    softspi_rw8(cmd);
    uint8_t hi = softspi_rw8(0x00);
    uint8_t lo = softspi_rw8(0x00);
    cs_high();
    return ((hi << 8) | lo) >> 3; /* 12-bit */
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

bool touch_get(int *x, int *y)
{
    if (gpio_get_level(TOUCH_IRQ) != 0) {
        return false;
    }

    int ax = 0, ay = 0, n = 0;
    for (int i = 0; i < 4; i++) {
        int rx = read_axis(0x90);
        int ry = read_axis(0xD0);
        if (rx > 100 && ry > 100) { ax += rx; ay += ry; n++; }
    }
    if (n == 0) return false;
    ax /= n; ay /= n;

    int sx = (ax - RAW_X_MIN) * (LCD_W - 1) / (RAW_X_MAX - RAW_X_MIN);
    int sy = (ay - RAW_Y_MIN) * (LCD_H - 1) / (RAW_Y_MAX - RAW_Y_MIN);
    sx = clampi(sx, 0, LCD_W - 1);
    sy = clampi(sy, 0, LCD_H - 1);

#if SWAP_XY
    int t = sx; sx = sy * (LCD_W - 1) / (LCD_H - 1);
    sy = t * (LCD_H - 1) / (LCD_W - 1);
#endif
#if INVERT_X
    sx = LCD_W - 1 - sx;
#endif
#if INVERT_Y
    sy = LCD_H - 1 - sy;
#endif

    *x = clampi(sx, 0, LCD_W - 1);
    *y = clampi(sy, 0, LCD_H - 1);
    return true;
}
