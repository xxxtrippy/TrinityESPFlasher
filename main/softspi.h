#ifndef SOFTSPI_H
#define SOFTSPI_H

#include <stdint.h>

/* Bit-banged SPI mode-0 shared by the LCD and the XPT2046 touch controller
 * (CLK=LCD_CLK, MOSI=LCD_MOSI, MISO=LCD_MISO in pins.h). Each device toggles
 * its own CS. */
void softspi_init(void);

/* Write one byte (MSB first). */
void softspi_w8(uint8_t v);

/* Write then read: clock out `v`, return the byte shifted in on MISO. */
uint8_t softspi_rw8(uint8_t v);

#endif
