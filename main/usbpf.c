/*
 * PicoFlasher USB protocol on the ESP32-S3 native USB port (GPIO19/20).
 * Enumerates as VID 0x600D / PID 0x7001 so J-Runner picks it up as a
 * PicoFlasher. Command set and replies follow balika011/PicoFlasher (GPL-2.0):
 *   5-byte packed command {u8 cmd; u32 lba}, little endian.
 * Only the NAND commands are implemented; eMMC detect answers "no eMMC".
 *
 * The console is claimed on the first command and released when the host
 * closes the port or the cable is pulled, so the touch/web UI keep working
 * whenever J-Runner isn't actually using the board.
 */
#include "usbpf.h"
#include "jobs.h"
#include "xbox.h"
#include <string.h>
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "usbpf";

#define GET_VERSION       0x00
#define GET_FLASH_CONFIG  0x01
#define READ_FLASH        0x02
#define WRITE_FLASH       0x03
#define READ_FLASH_STREAM 0x04
#define EMMC_DETECT       0x50

#define PAGE_SZ 0x210            /* 512 data + 16 spare */

/* ---------- descriptors ---------- */
static const tusb_desc_device_t dev_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x600D,
    .idProduct = 0x7001,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

#define CFG_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)
static const uint8_t cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CFG_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, 64),
};

static const char *str_desc[] = {
    (const char[]){0x09, 0x04},
    "PicoFlasher",
    "PicoFlasher Device",
    "TrinityESP",
    "PicoFlasher CDC",
};

/* ---------- console claim ---------- */
static volatile bool claimed;
static volatile bool streaming;

static bool claim(void)
{
    if (claimed) return true;
    if (!jobs_usb_claim()) return false;
    xbox_stop_smc();
    claimed = true;
    ESP_LOGI(TAG, "J-Runner connected, flash config 0x%08lx",
             (unsigned long)xbox_get_flash_config());
    return true;
}

static void release(void)
{
    streaming = false;
    if (!claimed) return;
    claimed = false;
    xbox_start_smc();
    jobs_usb_release();
    ESP_LOGI(TAG, "J-Runner released the console");
}

static void usb_event(tinyusb_event_t *ev, void *arg)
{
    (void)arg;
    if (ev->id == TINYUSB_EVENT_DETACHED || ev->id == TINYUSB_EVENT_SUSPENDED)
        release();
}

/* ---------- protocol ---------- */
static void wr(const void *p, uint32_t n)
{
    const uint8_t *b = p;
    while (n) {
        uint32_t k = tud_cdc_write(b, n);
        b += k; n -= k;
        if (n) { tud_cdc_write_flush(); vTaskDelay(1); }
        if (!tud_cdc_connected()) return;
    }
}

static void usbpf_task(void *arg)
{
    static uint8_t rx[5 + PAGE_SZ];
    static uint8_t pg[4 + PAGE_SZ];
    uint32_t have = 0, s_off = 0, s_end = 0;

    for (;;) {
        if (!tud_cdc_connected()) {
            have = 0;
            release();
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* Stream mode: push pages while there's TX room. */
        if (streaming) {
            if (s_off >= s_end) { streaming = false; tud_cdc_write_flush(); }
            else if (tud_cdc_write_available() >= sizeof(pg)) {
                uint32_t ret = xbox_nand_read_block(s_off, &pg[4], &pg[4 + 0x200]);
                memcpy(pg, &ret, 4);
                if (ret == 0) { tud_cdc_write(pg, sizeof(pg)); s_off++; }
                else { tud_cdc_write(&ret, 4); streaming = false; }
                tud_cdc_write_flush();
                continue;
            } else { tud_cdc_write_flush(); vTaskDelay(1); continue; }
        }

        uint32_t avail = tud_cdc_available();
        if (avail && have < sizeof(rx)) {
            uint32_t want = (have < 5) ? 5 - have
                          : (rx[0] == WRITE_FLASH ? 5 + PAGE_SZ - have : 0);
            if (want) have += tud_cdc_read(&rx[have], want);
        }
        if (have < 5) { if (!avail) vTaskDelay(1); continue; }
        if (rx[0] == WRITE_FLASH && have < 5 + PAGE_SZ) { if (!avail) vTaskDelay(1); continue; }

        uint8_t cmd = rx[0];
        uint32_t lba;
        memcpy(&lba, &rx[1], 4);
        have = 0;

        if (cmd != GET_VERSION && cmd != EMMC_DETECT && !claim()) {
            /* Touch/web job running: report failure instead of colliding. */
            uint32_t err = 0xFFFFFFFFu;
            if (cmd != READ_FLASH_STREAM) wr(&err, 4);
            tud_cdc_write_flush();
            continue;
        }

        switch (cmd) {
        case GET_VERSION: { uint32_t v = 3; wr(&v, 4); break; }
        case GET_FLASH_CONFIG: { uint32_t fc = xbox_get_flash_config(); wr(&fc, 4); break; }
        case READ_FLASH: {
            uint32_t ret = xbox_nand_read_block(lba, &pg[4], &pg[4 + 0x200]);
            wr(&ret, 4);
            if (ret == 0) wr(&pg[4], PAGE_SZ);
            break;
        }
        case WRITE_FLASH: {
            uint32_t ret = xbox_nand_write_block(lba, &rx[5], &rx[5 + 0x200]);
            wr(&ret, 4);
            break;
        }
        case READ_FLASH_STREAM:
            s_off = 0; s_end = lba; streaming = true;
            break;
        case EMMC_DETECT: { uint8_t no = 0; wr(&no, 1); break; }
        default:
            ESP_LOGW(TAG, "unsupported command 0x%02x", cmd);
            break;
        }
        tud_cdc_write_flush();
    }
}

void usbpf_start(void)
{
    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG(usb_event);
    cfg.descriptor.device = &dev_desc;
    cfg.descriptor.string = str_desc;
    cfg.descriptor.string_count = sizeof(str_desc) / sizeof(str_desc[0]);
    cfg.descriptor.full_speed_config = cfg_desc;
    if (tinyusb_driver_install(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB install failed");
        return;
    }
    xTaskCreate(usbpf_task, "usbpf", 4096, NULL, 5, NULL);
}
