#ifndef LCD_DRIVER_H
#define LCD_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

/* Landscape 320x240. */
#define LCD_W 320
#define LCD_H 240

/* RGB565 helpers / palette. */
#define RGB565(r, g, b) ((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)))
#define C_BLACK  0x0000
#define C_WHITE  0xFFFF
#define C_GRAY   0x8410
#define C_DGRAY  0x4208
#define C_GREEN  0x07E0
#define C_RED    0xF800
#define C_AMBER  0xFD20
#define C_BLUE   0x2D7F
#define C_PURPLE 0x8017

bool lcd_init(void);

void lcd_fill_rect(int x, int y, int w, int h, uint16_t color);
void lcd_fill(uint16_t color);
void lcd_rect(int x, int y, int w, int h, uint16_t color); /* 1px outline */

/* Draw text with the 8x8 font scaled by `scale` (1 = 8px tall). */
void lcd_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale);

/* Backlight on/off. */
void lcd_backlight(bool on);

#endif
