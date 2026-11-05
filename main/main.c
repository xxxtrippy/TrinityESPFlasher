
/*
 * main.c — TrinityWeb360 entry point (standalone touch-screen build).
 *
 * GPL-3.0. See LICENSE.
 *
 * No WiFi, no web UI, no PC: the 2.4" ILI9341 touch screen is the whole
 * interface. Dump / Build / Flash are driven from on-screen buttons; the CPU
 * key for Build is read from /sd/cpukey.txt.
 */
#include "kuart.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "board_spi.h"
#include "sdcard.h"
#include "lcd.h"
#include "touch.h"
#include "xbox.h"
#include "jobs.h"
#include "ui.h"
#include "wifi.h"
#include "rgb.h"

static const char *TAG = "main";

void web_server_start(void);

void app_main(void)
{
    /* Initialize onboard RGB LED first. */
    rgb_init();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Shared SPI3 bus first, then the devices that sit on it. */
    board_spi_init();

    if (!lcd_init()) {
        ESP_LOGE(TAG, "LCD init failed");
    }
    if (!touch_init()) {
        ESP_LOGE(TAG, "touch init failed");
    }
    if (!sd_mount()) {
        ESP_LOGW(TAG, "no SD card (dump/flash still work; build needs the card)");
    }

    xbox_init();
    kuart_start();
    jobs_init();
    ui_start();

    /* Optional WiFi web UI alongside the touch screen. Set your SSID/pass in
     * wifi.c; if it can't join it falls back to its own AP. The flasher works
     * fully standalone without any of this. */
    wifi_init();
    web_server_start();

    ESP_LOGI(TAG, "flasher ready (touch + web)");
}
