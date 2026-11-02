/*
 * lcd.c — minimal ILI9341 driver over bit-banged SPI (softspi.c).
 *
 * GPL-3.0. See LICENSE.
 *
 * The LCD runs on its own software SPI bus (the S3's two hardware SPI hosts are
 * taken by the Xbox SB and the SD card). DC selects command/data; CS is manual.
 */
#include "lcd.h"

#include <string.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "pins.h"
#include "softspi.h"
#include "font8x8.h"

static const char *TAG = "lcd";

static inline void cs_low(void)  { gpio_set_level(LCD_CS, 0); }
static inline void cs_high(void) { gpio_set_level(LCD_CS, 1); }
static inline void dc_cmd(void)  { gpio_set_level(LCD_DC, 0); }
static inline void dc_dat(void)  { gpio_set_level(LCD_DC, 1); }

static void wr_cmd(uint8_t c)  { dc_cmd(); cs_low(); softspi_w8(c);  cs_high(); }
static void wr_d8(uint8_t v)   { dc_dat(); cs_low(); softspi_w8(v);  cs_high(); }
static void wr_dn(const uint8_t *d, int n)
{ dc_dat(); cs_low(); for (int i = 0; i < n; i++) softspi_w8(d[i]); cs_high(); }

static void set_window(int x, int y, int w, int h)
{
    int x1 = x + w - 1, y1 = y + h - 1;
    uint8_t c[4] = { x >> 8, x & 0xFF, x1 >> 8, x1 & 0xFF };
    wr_cmd(0x2A); wr_dn(c, 4);
    uint8_t r[4] = { y >> 8, y & 0xFF, y1 >> 8, y1 & 0xFF };
    wr_cmd(0x2B); wr_dn(r, 4);
    wr_cmd(0x2C);
}

static void push_pixels(const uint16_t *px, int n)
{
    dc_dat(); cs_low();
    for (int i = 0; i < n; i++) {
        softspi_w8(px[i] >> 8);
        softspi_w8(px[i] & 0xFF);
    }
    cs_high();
}

bool lcd_init(void)
{
    softspi_init();

    gpio_config_t io = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LCD_CS) | (1ULL << LCD_DC) |
                        (1ULL << LCD_RST) | (1ULL << LCD_BL),
    };
    gpio_config(&io);
    cs_high();

    gpio_set_level(LCD_BL, 0);
    gpio_set_level(LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    wr_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(120));
    wr_cmd(0x3A); wr_d8(0x55);
    wr_cmd(0x36); wr_d8(0xE8); /* landscape, rotated 180 deg (was 0x28) */
    wr_cmd(0xB1); { uint8_t d[] = {0x00,0x18}; wr_dn(d,2); }
    wr_cmd(0xC0); wr_d8(0x23);
    wr_cmd(0xC1); wr_d8(0x10);
    wr_cmd(0xC5); { uint8_t d[] = {0x3E,0x28}; wr_dn(d,2); }
    wr_cmd(0xC7); wr_d8(0x86);
    wr_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120));
    wr_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(20));

    lcd_fill(C_BLACK);
    lcd_backlight(true);
    ESP_LOGI(TAG, "ILI9341 up (320x240, soft-SPI)");
    return true;
}

void lcd_backlight(bool on) { gpio_set_level(LCD_BL, on ? 1 : 0); }

void lcd_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    if (w <= 0 || h <= 0) return;

    set_window(x, y, w, h);
    dc_dat(); cs_low();
    for (int i = 0; i < w * h; i++) {
        softspi_w8(color >> 8);
        softspi_w8(color & 0xFF);
    }
    cs_high();
}

void lcd_fill(uint16_t color) { lcd_fill_rect(0, 0, LCD_W, LCD_H, color); }

void lcd_rect(int x, int y, int w, int h, uint16_t color)
{
    lcd_fill_rect(x, y, w, 1, color);
    lcd_fill_rect(x, y + h - 1, w, 1, color);
    lcd_fill_rect(x, y, 1, h, color);
    lcd_fill_rect(x + w - 1, y, 1, h, color);
}

void lcd_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    if (scale < 1) scale = 1;
    int cw = 8 * scale, ch = 8 * scale;
    static uint16_t cell[16 * 16];
    if (cw * ch > (int)(sizeof(cell) / sizeof(cell[0]))) return;

    for (const char *p = s; *p; p++) {
        char c = *p;
        if (c < 0x20 || c > 0x7E) c = '?';
        const uint8_t *g = FONT8X8[c - 0x20];
        for (int gy = 0; gy < 8; gy++) {
            uint8_t bits = g[gy];
            for (int sy = 0; sy < scale; sy++) {
                int ry = gy * scale + sy;
                for (int gx = 0; gx < 8; gx++) {
                    uint16_t col = (bits & (1 << gx)) ? fg : bg;
                    for (int sx = 0; sx < scale; sx++)
                        cell[ry * cw + gx * scale + sx] = col;
                }
            }
        }
        if (x >= 0 && y >= 0 && x + cw <= LCD_W && y + ch <= LCD_H) {
            set_window(x, y, cw, ch);
            push_pixels(cell, cw * ch);
        }
        x += cw;
        if (x + cw > LCD_W) break;
    }
}
