/*
 * rgb.c — onboard WS2812 RGB status LED with non-blocking animations.
 *
 * TrinityWeb360
 * ESP32-S3 GPIO48
 */

#include "rgb.h"
#include "pins.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * Drives the onboard WS2812 directly with the ESP-IDF core RMT TX driver
 * (esp_driver_rmt) — no external led_strip component, so it builds natively
 * everywhere with no component-registry fetch.
 */

#define RGB_GPIO             48
#define RGB_BRIGHTNESS       32
#define RGB_ACTIVITY_MS      333
#define RGB_FAILURE_MS       1000

/* RMT at 10 MHz -> 0.1 us per tick. WS2812 bit timings (ticks). */
#define RMT_RES_HZ           (10 * 1000 * 1000)
#define T0H 4   /* 0.4us */
#define T0L 8   /* 0.8us */
#define T1H 8   /* 0.8us */
#define T1L 4   /* 0.4us */

static const char *TAG = "rgb";
static rmt_channel_handle_t rgb_chan = NULL;
static rmt_encoder_handle_t rgb_enc  = NULL;
static bool rgb_ok = false;

typedef enum {
    RGB_STATE_READY,
    RGB_STATE_DUMPING,
    RGB_STATE_BUILDING,
    RGB_STATE_FLASHING,
    RGB_STATE_VERIFYING,
    RGB_STATE_ERROR,
    RGB_STATE_COMPLETE,
    RGB_STATE_OFF
} rgb_state_t;

/* Written by the job worker, read by the LED task. */
static volatile rgb_state_t rgb_state = RGB_STATE_READY;

static void rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!rgb_ok) return;

    /* WS2812 wants GRB order, MSB first. */
    uint8_t grb[3] = { g, r, b };
    rmt_transmit_config_t tx = { .loop_count = 0 };
    esp_err_t ret = rmt_transmit(rgb_chan, rgb_enc, grb, sizeof(grb), &tx);
    if (ret == ESP_OK) ret = rmt_tx_wait_all_done(rgb_chan, 100);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RGB update failed: %s", esp_err_to_name(ret));
    }
}

__attribute__((unused)) static void rgb_animation_task(void *arg)
{
    rgb_state_t previous = (rgb_state_t)-1;
    bool pulse_on = true;

    for (;;) {
        rgb_state_t current = rgb_state;

        if (current != previous) {
            previous = current;
            pulse_on = true;
        }

        switch (current) {
            case RGB_STATE_READY:
                rgb_set(RGB_BRIGHTNESS, RGB_BRIGHTNESS, RGB_BRIGHTNESS);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;

            case RGB_STATE_DUMPING:
                rgb_set(pulse_on ? 0 : 0,
                        pulse_on ? 0 : 0,
                        pulse_on ? RGB_BRIGHTNESS : 0);
                pulse_on = !pulse_on;
                vTaskDelay(pdMS_TO_TICKS(RGB_ACTIVITY_MS));
                break;

            case RGB_STATE_BUILDING:
                rgb_set(RGB_BRIGHTNESS, 0, RGB_BRIGHTNESS);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;

            case RGB_STATE_FLASHING:
                rgb_set(pulse_on ? RGB_BRIGHTNESS : 0,
                        pulse_on ? (RGB_BRIGHTNESS / 2) : 0,
                        0);
                pulse_on = !pulse_on;
                vTaskDelay(pdMS_TO_TICKS(RGB_ACTIVITY_MS));
                break;

            case RGB_STATE_VERIFYING:
                rgb_set(RGB_BRIGHTNESS, RGB_BRIGHTNESS, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;

            case RGB_STATE_ERROR:
                rgb_set(pulse_on ? RGB_BRIGHTNESS : 0, 0, 0);
                pulse_on = !pulse_on;
                vTaskDelay(pdMS_TO_TICKS(RGB_FAILURE_MS));
                break;

            case RGB_STATE_COMPLETE:
                rgb_set(0, RGB_BRIGHTNESS, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;

            case RGB_STATE_OFF:
            default:
                rgb_set(0, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
        }
    }
}

bool rgb_init(void)
{
#if RGB_GPIO == PIN_BTN_SELECT
    /* The onboard WS2812 shares GPIO48 with the SELECT button. The button wins:
     * driving the LED here would steal the pin and the button would never read.
     * Status is shown on the LCD instead. All rgb_* calls become no-ops. */
    ESP_LOGW(TAG, "onboard RGB disabled: GPIO%d is the SELECT button", RGB_GPIO);
    rgb_ok = false;
    return false;
#else
    rmt_tx_channel_config_t chan_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = RGB_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = RMT_RES_HZ,
        .trans_queue_depth = 4,
    };
    esp_err_t ret = rmt_new_tx_channel(&chan_cfg, &rgb_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RGB RMT channel failed: %s", esp_err_to_name(ret));
        return false;
    }

    /* Bytes encoder: map each 0/1 bit to its WS2812 pulse. */
    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .level0 = 1, .duration0 = T0H, .level1 = 0, .duration1 = T0L },
        .bit1 = { .level0 = 1, .duration0 = T1H, .level1 = 0, .duration1 = T1L },
        .flags.msb_first = 1,
    };
    ret = rmt_new_bytes_encoder(&enc_cfg, &rgb_enc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RGB encoder failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = rmt_enable(rgb_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RGB enable failed: %s", esp_err_to_name(ret));
        return false;
    }

    rgb_ok = true;

    if (xTaskCreate(rgb_animation_task, "rgb_anim", 2048, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not create RGB animation task");
        rgb_ok = false;
        return false;
    }

    ESP_LOGI(TAG, "RGB LED initialized on GPIO%d (core RMT)", RGB_GPIO);
    rgb_ready();
    return true;
#endif
}

void rgb_ready(void)      { rgb_state = RGB_STATE_READY; }
void rgb_dumping(void)    { rgb_state = RGB_STATE_DUMPING; }
void rgb_building(void)   { rgb_state = RGB_STATE_BUILDING; }
void rgb_flashing(void)   { rgb_state = RGB_STATE_FLASHING; }
void rgb_verifying(void)  { rgb_state = RGB_STATE_VERIFYING; }
void rgb_error(void)      { rgb_state = RGB_STATE_ERROR; }
void rgb_complete(void)   { rgb_state = RGB_STATE_COMPLETE; }
void rgb_off(void)        { rgb_state = RGB_STATE_OFF; }
