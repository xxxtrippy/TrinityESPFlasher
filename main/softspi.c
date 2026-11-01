/*
 * softspi.c — bit-banged SPI mode-0 for the LCD + touch bus.
 *
 * GPL-3.0. See LICENSE.
 */
#include "softspi.h"

#include "driver/gpio.h"
#include "pins.h"

static inline void clk_low(void)  { gpio_set_level(LCD_CLK, 0); }
static inline void clk_high(void) { gpio_set_level(LCD_CLK, 1); }

void softspi_init(void)
{
    gpio_config_t out = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LCD_CLK) | (1ULL << LCD_MOSI),
    };
    gpio_config(&out);

    gpio_config_t in = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pin_bit_mask = (1ULL << LCD_MISO),
    };
    gpio_config(&in);

    gpio_set_level(LCD_CLK, 0);
    gpio_set_level(LCD_MOSI, 0);
}

uint8_t softspi_rw8(uint8_t v)
{
    uint8_t in = 0;
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(LCD_MOSI, (v >> i) & 1);
        clk_high();
        in = (in << 1) | (gpio_get_level(LCD_MISO) & 1);
        clk_low();
    }
    return in;
}

void softspi_w8(uint8_t v)
{
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(LCD_MOSI, (v >> i) & 1);
        clk_high();
        clk_low();
    }
}
