/*
 * rgb.c — onboard WS2812 RGB status LED with non-blocking animations.
 *
 * TrinityWeb360
 * ESP32-S3 GPIO48
 */

#include "rgb.h"

#include <stdint.h>

#include "esp_log.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RGB_GPIO             48
#define RGB_LED_COUNT        1
#define RGB_BRIGHTNESS       32
#define RGB_ACTIVITY_MS      333
#define RGB_FAILURE_MS       1000

static const char *TAG = "rgb";
static led_strip_handle_t rgb_strip = NULL;

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
    if (rgb_strip == NULL) return;

    esp_err_t ret = led_strip_set_pixel(rgb_strip, 0, r, g, b);
    if (ret == ESP_OK) ret = led_strip_refresh(rgb_strip);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RGB update failed: %s", esp_err_to_name(ret));
    }
}

static void rgb_animation_task(void *arg)
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
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_GPIO,
        .max_leds = RGB_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };

    esp_err_t ret = led_strip_new_rmt_device(
        &strip_config, &rmt_config, &rgb_strip);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RGB initialization failed: %s", esp_err_to_name(ret));
        return false;
    }

    if (xTaskCreate(rgb_animation_task, "rgb_anim", 2048, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not create RGB animation task");
        led_strip_clear(rgb_strip);
        rgb_strip = NULL;
        return false;
    }

    ESP_LOGI(TAG, "RGB LED initialized on GPIO%d", RGB_GPIO);
    rgb_ready();
    return true;
}

void rgb_ready(void)      { rgb_state = RGB_STATE_READY; }
void rgb_dumping(void)    { rgb_state = RGB_STATE_DUMPING; }
void rgb_building(void)   { rgb_state = RGB_STATE_BUILDING; }
void rgb_flashing(void)   { rgb_state = RGB_STATE_FLASHING; }
void rgb_verifying(void)  { rgb_state = RGB_STATE_VERIFYING; }
void rgb_error(void)      { rgb_state = RGB_STATE_ERROR; }
void rgb_complete(void)   { rgb_state = RGB_STATE_COMPLETE; }
void rgb_off(void)        { rgb_state = RGB_STATE_OFF; }
