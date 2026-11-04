/*
 * web_server.c — optional WiFi web UI, as a thin layer over the shared jobs
 * engine (jobs.c). Runs alongside the on-screen touch UI.
 *
 * GPL-3.0. See LICENSE.
 *
 *   GET  /              the web app (web_index.h)
 *   GET  /api/detect    stop SMC + read flash config
 *   GET  /api/sd        SD xeBuild-package presence
 *   POST /api/dump      start a NAND dump
 *   POST /api/build     start a build (JSON {cpu_key} optional; else cpukey.txt)
 *   POST /api/flash     start a flash
 *   GET  /api/progress  SSE live progress from jobs_get()
 *   GET  /dump.bin      download the last verified dump (NANDDUMP.BIN)
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_http_server.h"

#include "jobs.h"
#include "sdcard.h"
#include "web_index.h"

static const char *TAG = "web";

static esp_err_t resp_busy(httpd_req_t *r)
{
    httpd_resp_set_status(r, "409 Conflict");
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"error\":\"busy\"}");
}

static esp_err_t h_index(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_detect(httpd_req_t *r)
{
    job_status_t s; jobs_get(&s);
    if (s.busy) return resp_busy(r);   /* never touch the SMC mid-job */
    jobs_detect();
    jobs_get(&s);
    char buf[160];
    snprintf(buf, sizeof(buf),
        "{\"linked\":%s,\"flash_config\":\"0x%08lX\"}",
        s.linked ? "true" : "false", (unsigned long)s.flash_config);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, buf);
}

static esp_err_t h_sd(httpd_req_t *r)
{
    httpd_resp_set_type(r, "application/json");
    if (!sd_is_mounted() && !sd_mount())
        return httpd_resp_sendstr(r, "{\"ready\":false,\"msg\":\"no card\"}");
    void *f = sd_open_ingredient("cb.bin");
    if (f) { fclose((FILE *)f); return httpd_resp_sendstr(r, "{\"ready\":true}"); }
    return httpd_resp_sendstr(r, "{\"ready\":false,\"msg\":\"xebuild/data not found\"}");
}

static esp_err_t h_dump(httpd_req_t *r)
{
    return jobs_start(JOB_DUMP) ? httpd_resp_sendstr(r, "{\"started\":true}")
                                : resp_busy(r);
}

static esp_err_t h_build(httpd_req_t *r)
{
    char body[128] = { 0 };
    int n = httpd_req_recv(r, body, sizeof(body) - 1);
    bool keyed = false;
    if (n > 0) {
        char *p = strstr(body, "cpu_key");
        if (p && (p = strchr(p, ':')) && (p = strchr(p, '"'))) {
            p++; char *e = strchr(p, '"');
            if (e && (e - p) == 32) {
                char hex[33]; memcpy(hex, p, 32); hex[32] = 0;
                keyed = jobs_set_cpukey_hex(hex);
            }
        }
    }
    if (!keyed) jobs_load_cpukey_from_sd(); /* fall back to SD */
    return jobs_start(JOB_BUILD) ? httpd_resp_sendstr(r, "{\"started\":true}")
                                 : resp_busy(r);
}

static esp_err_t h_flash(httpd_req_t *r)
{
    return jobs_start(JOB_FLASH) ? httpd_resp_sendstr(r, "{\"started\":true}")
                                 : resp_busy(r);
}

static esp_err_t h_progress(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/event-stream");
    httpd_resp_set_hdr(r, "Cache-Control", "no-cache");
    for (int i = 0; i < 300; i++) {
        job_status_t s; jobs_get(&s);
        const char *cls = s.done ? (s.ok ? "ok" : "err") : "";
        char line[200];
        int m = snprintf(line, sizeof(line),
            "data: {\"percent\":%d,\"stage\":\"%s\",\"cls\":\"%s\",\"done\":%s,"
            "\"flashable\":%s}\n\n",
            s.percent, s.stage, cls, s.done ? "true" : "false",
            s.flashable ? "true" : "false");
        if (httpd_resp_send_chunk(r, line, m) != ESP_OK) return ESP_FAIL;
        if (s.done) break;
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    return httpd_resp_send_chunk(r, NULL, 0);
}

/* Stream the last verified dump from the SD card in 16 KB pieces. */
static esp_err_t h_dump_bin(httpd_req_t *r)
{
    job_status_t s; jobs_get(&s);
    if (s.busy) return resp_busy(r);   /* the card is being written */

    FILE *f = fopen(SD_MOUNT_POINT "/NANDDUMP.BIN", "rb");
    if (!f) {
        httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "no verified dump on the SD card yet");
        return ESP_OK;
    }
    const size_t CH = 16 * 1024;
    char *buf = malloc(CH);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }

    httpd_resp_set_type(r, "application/octet-stream");
    httpd_resp_set_hdr(r, "Content-Disposition", "attachment; filename=\"nand_trinity.bin\"");

    esp_err_t res = ESP_OK;
    size_t n;
    while ((n = fread(buf, 1, CH, f)) > 0) {
        if (httpd_resp_send_chunk(r, buf, n) != ESP_OK) { res = ESP_FAIL; break; }
    }
    free(buf);
    fclose(f);
    if (res != ESP_OK) return res;           /* client went away */
    return httpd_resp_send_chunk(r, NULL, 0);
}

static void reg(httpd_handle_t s, const char *uri, httpd_method_t m,
                esp_err_t (*fn)(httpd_req_t *))
{
    httpd_uri_t u = { .uri = uri, .method = m, .handler = fn };
    httpd_register_uri_handler(s, &u);
}

void web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 12;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.send_wait_timeout = 15;   /* big download over WiFi */

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }
    reg(srv, "/",             HTTP_GET,  h_index);
    reg(srv, "/api/detect",   HTTP_GET,  h_detect);
    reg(srv, "/api/sd",       HTTP_GET,  h_sd);
    reg(srv, "/api/dump",     HTTP_POST, h_dump);
    reg(srv, "/api/build",    HTTP_POST, h_build);
    reg(srv, "/api/flash",    HTTP_POST, h_flash);
    reg(srv, "/api/progress", HTTP_GET,  h_progress);
    reg(srv, "/dump.bin",     HTTP_GET,  h_dump_bin);
    ESP_LOGI(TAG, "web UI up");
}
