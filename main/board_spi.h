#ifndef BOARD_SPI_H
#define BOARD_SPI_H

#include "esp_err.h"
#include "driver/spi_common.h"

/* SD card's own hardware SPI bus. (LCD+touch are bit-banged; Xbox is SPI2.) */
#define BOARD_SD_LCD_SPI_HOST SPI3_HOST

/* Idempotent SD-bus init. */
esp_err_t board_spi_init(void);

#endif
