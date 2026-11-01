/*
 * sdcard.c — microSD mount + xeBuild data-package access for TrinityWeb360.
 *
 * GPL-3.0. See LICENSE.
 */
#include "sdcard.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

#include "pins.h"
#include "board_spi.h"

/* SD shares the SPI3 bus with the LCD and touch (bus owned by board_spi.c). */
#define SD_SPI_HOST BOARD_SD_LCD_SPI_HOST

static const char *TAG = "sd";
static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

bool sd_mount(void)
{
    if (s_mounted) {
        return true;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    /* The V474-style breakout is SPI-only, so drive the card over SDSPI. */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;

    /* Shared bus is brought up once by board_spi_init(). */
    if (board_spi_init() != ESP_OK) {
        return false;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = SD_CS;
    slot_cfg.host_id = SD_SPI_HOST;

    esp_err_t err = esp_vfs_fat_sdspi_mount(
        SD_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        s_card = NULL;
        return false;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "card mounted (%llu MB)",
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20);
    return true;
}

void sd_unmount(void)
{
    if (s_mounted) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        /* bus stays up — shared with LCD/touch, owned by board_spi.c */
        s_mounted = false;
        s_card = NULL;
    }
}

bool sd_is_mounted(void) { return s_mounted; }

uint64_t sd_total_bytes(void)
{
    if (!s_card) return 0;
    return (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
}

uint64_t sd_free_bytes(void)
{
    /* FATFS free-cluster query is available via f_getfree, but esp_vfs_fat
     * exposes it indirectly. For the UI a total-size readout is enough; a
     * precise free figure is a nice-to-have, returned 0 (unknown) for now. */
    return 0;
}

static void join_path(char *out, size_t out_sz, const char *rel)
{
    snprintf(out, out_sz, "%s/%s", XEBUILD_DATA_ROOT, rel);
}

int sd_check_package(xebuild_ingredient_t *list, size_t count)
{
    int missing_required = 0;
    for (size_t i = 0; i < count; i++) {
        char full[256];
        join_path(full, sizeof(full), list[i].rel_path);

        struct stat st;
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
            list[i].present = true;
            list[i].size = (uint32_t)st.st_size;
        } else {
            list[i].present = false;
            list[i].size = 0;
            if (list[i].required) {
                missing_required++;
                ESP_LOGW(TAG, "missing required ingredient: %s", list[i].rel_path);
            }
        }
    }
    return missing_required;
}

void *sd_open_ingredient(const char *rel_path)
{
    char full[256];
    join_path(full, sizeof(full), rel_path);
    FILE *f = fopen(full, "rb");
    if (!f) {
        ESP_LOGW(TAG, "cannot open ingredient: %s", full);
    }
    return f;
}
