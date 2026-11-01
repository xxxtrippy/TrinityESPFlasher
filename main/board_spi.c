/*
 * board_spi.c — SD card's own hardware SPI3 bus.
 *
 * GPL-3.0. See LICENSE.
 *
 * SD is alone on SPI3. The LCD + touch use a separate bit-banged bus
 * (softspi.c). The Xbox SB has its own isolated bus on SPI2 (spiex.c).
 */
#include "board_spi.h"

#include "esp_log.h"
#include "pins.h"

static const char *TAG = "spi3";
static bool s_inited = false;

esp_err_t board_spi_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    spi_bus_config_t bus = {
        .mosi_io_num = SD_MOSI,
        .miso_io_num = SD_MISO,
        .sclk_io_num = SD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    esp_err_t err = spi_bus_initialize(BOARD_SD_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD SPI3 bus init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_inited = true;
    ESP_LOGI(TAG, "SD SPI3 bus ready");
    return ESP_OK;
}
