/*
 * Kernel debug UART (KER_DBG_TXD/RXD on the Xbox debug header).
 * Keeps a rolling log for the web UI and watches for xeLL's "CPU Key" line.
 * A new key is written to /sd/cpukey.txt if that file doesn't exist yet; if
 * it exists with a different key, the new one goes to /sd/cpukey_new.txt and
 * the original is left untouched.
 */
#include "kuart.h"
#include "pins.h"
#include "sdcard.h"
#include "jobs.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#define KU_PORT UART_NUM_1
static const char *TAG = "kuart";

static char ring[KUART_LOG_SZ];
static size_t ring_head, ring_len;
static char cpukey[33];
static SemaphoreHandle_t lock;

static void ring_put(char c)
{
    ring[ring_head] = c;
    ring_head = (ring_head + 1) % KUART_LOG_SZ;
    if (ring_len < KUART_LOG_SZ) ring_len++;
}

size_t kuart_log_copy(char *dst, size_t cap)
{
    if (!lock || cap == 0) return 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    size_t n = ring_len < cap ? ring_len : cap;
    size_t start = (ring_head + KUART_LOG_SZ - n) % KUART_LOG_SZ;
    for (size_t i = 0; i < n; i++) dst[i] = ring[(start + i) % KUART_LOG_SZ];
    xSemaphoreGive(lock);
    return n;
}

bool kuart_cpukey(char out[33])
{
    if (!lock) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = cpukey[0] != 0;
    if (ok) memcpy(out, cpukey, 33);
    xSemaphoreGive(lock);
    return ok;
}

/* Find "cpu key" (any case) followed by exactly 32 contiguous hex digits. */
static bool parse_key(const char *line, char out[33])
{
    char low[160];
    size_t n = strlen(line);
    if (n >= sizeof(low)) n = sizeof(low) - 1;
    for (size_t i = 0; i < n; i++) low[i] = (char)tolower((unsigned char)line[i]);
    low[n] = 0;
    const char *p = strstr(low, "cpu key");
    if (!p) p = strstr(low, "cpukey");
    if (!p) return false;
    for (p = line + (p - low); *p; p++) {
        size_t k = 0;
        while (isxdigit((unsigned char)p[k])) k++;
        if (k == 32) {
            for (int i = 0; i < 32; i++) out[i] = (char)toupper((unsigned char)p[i]);
            out[32] = 0;
            return true;
        }
        if (k) p += k - 1;
    }
    return false;
}

static void save_key(const char *key)
{
    if (!sd_is_mounted()) { ESP_LOGW(TAG, "CPU key seen, no SD to save it"); return; }
    char old[40] = {0};
    FILE *f = fopen(SD_MOUNT_POINT "/cpukey.txt", "rb");
    if (f) { fread(old, 1, 32, f); fclose(f); }
    const char *path = SD_MOUNT_POINT "/cpukey.txt";
    if (old[0]) {
        if (strncasecmp(old, key, 32) == 0) return;
        path = SD_MOUNT_POINT "/cpukey_new.txt";
        ESP_LOGW(TAG, "key differs from cpukey.txt; saved to cpukey_new.txt");
    }
    f = fopen(path, "wb");
    if (f) { fwrite(key, 1, 32, f); fputc('\n', f); fclose(f); }
    ESP_LOGI(TAG, "CPU key saved to %s", path);
}

static void kuart_task(void *arg)
{
    uint8_t buf[128];
    char line[160];
    size_t ll = 0;
    for (;;) {
        int n = uart_read_bytes(KU_PORT, buf, sizeof(buf), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            xSemaphoreTake(lock, portMAX_DELAY);
            ring_put(c);
            xSemaphoreGive(lock);
            if (c == '\n' || c == '\r' || ll == sizeof(line) - 1) {
                line[ll] = 0;
                char key[33];
                if (ll && parse_key(line, key)) {
                    bool fresh;
                    xSemaphoreTake(lock, portMAX_DELAY);
                    fresh = strcmp(cpukey, key) != 0;
                    memcpy(cpukey, key, 33);
                    xSemaphoreGive(lock);
                    if (fresh) {
                        ESP_LOGI(TAG, "CPU key captured");
                        jobs_set_cpukey_hex(key);
                        save_key(key);
                    }
                }
                ll = 0;
            } else {
                line[ll++] = c;
            }
        }
    }
}

void kuart_start(void)
{
    lock = xSemaphoreCreateMutex();
    uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(KU_PORT, 2048, 0, 0, NULL, 0) != ESP_OK ||
        uart_param_config(KU_PORT, &cfg) != ESP_OK ||
        uart_set_pin(KU_PORT, KUART_TX, KUART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "UART init failed");
        return;
    }
    gpio_pullup_en(KUART_RX);   /* idle-high when the header is unplugged */
    xTaskCreate(kuart_task, "kuart", 4096, NULL, 4, NULL);
}
