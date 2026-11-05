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
#include "kuart.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_http_server.h"

#include "jobs.h"
#include "sdcard.h"
#include "sdfm.h"
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

/* ---------------- SD file manager (confined to /sd) ----------------
 * Every path is resolved through sdfm_resolve(), which rejects anything that
 * would escape the /sd mount, so the web UI can only ever touch the SD card —
 * never the firmware or anything in ESP flash. */

static bool q_param(httpd_req_t *r, const char *key, char *buf, size_t sz)
{
    char q[256];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK) return false;
    if (httpd_query_key_value(q, key, buf, sz) != ESP_OK) return false;
    char *w = buf;                       /* minimal percent-decode in place */
    for (char *p = buf; *p; p++) {
        if (p[0] == '%' && p[1] && p[2]) {
            char h[3] = { p[1], p[2], 0 };
            *w++ = (char)strtol(h, NULL, 16);
            p += 2;
        } else if (*p == '+') {
            *w++ = ' ';
        } else {
            *w++ = *p;
        }
    }
    *w = 0;
    return true;
}

static esp_err_t sd_guard(httpd_req_t *r)
{
    if (sd_is_mounted() || sd_mount()) return ESP_OK;
    httpd_resp_set_status(r, "503 Service Unavailable");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, "{\"error\":\"no SD card\"}");
    return ESP_FAIL;
}

static esp_err_t bad_path(httpd_req_t *r)
{
    httpd_resp_set_status(r, "400 Bad Request");
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"error\":\"bad path\"}");
}

/* Refuse SD mutations while a dump/build/flash is running — that job may be
 * reading or writing the card (e.g. streaming NANDDUMP.BIN), and a concurrent
 * write would corrupt the filesystem. Returns ESP_FAIL if busy. */
static esp_err_t fs_not_busy(httpd_req_t *r)
{
    job_status_t s; jobs_get(&s);
    if (!s.busy) return ESP_OK;
    httpd_resp_set_status(r, "409 Conflict");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, "{\"error\":\"busy - a job is using the SD card\"}");
    return ESP_FAIL;
}

/* GET /api/fs/list?path=rel -> {entries:[{name,dir,size}]} */
static esp_err_t h_fs_list(httpd_req_t *r)
{
    if (sd_guard(r) != ESP_OK) return ESP_OK;
    char rel[200] = ""; q_param(r, "path", rel, sizeof(rel));
    char abs[256];
    if (!sdfm_resolve(rel, abs, sizeof(abs))) return bad_path(r);
    DIR *d = opendir(abs);
    if (!d) { httpd_resp_set_status(r, "404 Not Found");
        return httpd_resp_sendstr(r, "{\"error\":\"not a directory\"}"); }
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr_chunk(r, "{\"entries\":[");
    struct dirent *e; bool first = true;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[520]; snprintf(full, sizeof(full), "%s/%s", abs, e->d_name);
        struct stat st; long sz = 0; int isdir = 0;
        if (stat(full, &st) == 0) { isdir = S_ISDIR(st.st_mode); sz = st.st_size; }
        char line[600];
        snprintf(line, sizeof(line), "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%ld}",
                 first ? "" : ",", e->d_name, isdir ? "true" : "false", sz);
        httpd_resp_sendstr_chunk(r, line);
        first = false;
    }
    closedir(d);
    httpd_resp_sendstr_chunk(r, "]}");
    return httpd_resp_sendstr_chunk(r, NULL);
}

/* GET /api/fs/get?path=rel -> download */
static esp_err_t h_fs_get(httpd_req_t *r)
{
    if (sd_guard(r) != ESP_OK) return ESP_OK;
    char rel[200] = ""; q_param(r, "path", rel, sizeof(rel));
    char abs[256];
    if (!sdfm_resolve(rel, abs, sizeof(abs))) return bad_path(r);
    FILE *f = fopen(abs, "rb");
    if (!f) { httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "not found"); return ESP_OK; }
    const char *base = strrchr(abs, '/'); base = base ? base + 1 : abs;
    char disp[300]; snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", base);
    httpd_resp_set_type(r, "application/octet-stream");
    httpd_resp_set_hdr(r, "Content-Disposition", disp);
    char *buf = malloc(8192);
    if (!buf) { fclose(f); httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_OK; }
    size_t n; esp_err_t res = ESP_OK;
    while ((n = fread(buf, 1, 8192, f)) > 0) {
        if (httpd_resp_send_chunk(r, buf, n) != ESP_OK) { res = ESP_FAIL; break; }
    }
    free(buf); fclose(f);
    if (res != ESP_OK) return res;
    return httpd_resp_send_chunk(r, NULL, 0);
}

/* POST /api/fs/upload?path=rel  body=raw bytes (rel = destination file path) */
static esp_err_t h_fs_upload(httpd_req_t *r)
{
    if (sd_guard(r) != ESP_OK) return ESP_OK;
    if (fs_not_busy(r) != ESP_OK) return ESP_OK;
    char rel[200] = ""; q_param(r, "path", rel, sizeof(rel));
    char abs[256];
    if (!sdfm_resolve(rel, abs, sizeof(abs)) || rel[0] == '\0') return bad_path(r);
    FILE *f = fopen(abs, "wb");
    if (!f) { httpd_resp_set_status(r, "500 Internal Server Error");
        return httpd_resp_sendstr(r, "{\"error\":\"cannot create\"}"); }
    char *buf = malloc(8192);
    if (!buf) { fclose(f); httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_OK; }
    int remaining = r->content_len, rec;
    while (remaining > 0) {
        rec = httpd_req_recv(r, buf, remaining < 8192 ? remaining : 8192);
        if (rec <= 0) { free(buf); fclose(f); httpd_resp_set_status(r, "500 Internal Server Error");
            return httpd_resp_sendstr(r, "{\"error\":\"recv failed\"}"); }
        fwrite(buf, 1, rec, f);
        remaining -= rec;
    }
    free(buf); fclose(f);
    return httpd_resp_sendstr(r, "{\"ok\":true}");
}

/* POST /api/fs/mkdir?path=rel */
static esp_err_t h_fs_mkdir(httpd_req_t *r)
{
    if (sd_guard(r) != ESP_OK) return ESP_OK;
    if (fs_not_busy(r) != ESP_OK) return ESP_OK;
    char rel[200] = ""; q_param(r, "path", rel, sizeof(rel));
    char abs[256];
    if (!sdfm_resolve(rel, abs, sizeof(abs)) || rel[0] == '\0') return bad_path(r);
    if (mkdir(abs, 0777) != 0) { httpd_resp_set_status(r, "500 Internal Server Error");
        return httpd_resp_sendstr(r, "{\"error\":\"mkdir failed\"}"); }
    return httpd_resp_sendstr(r, "{\"ok\":true}");
}

static void rm_recursive(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char child[520]; snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
                rm_recursive(child);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

/* POST /api/fs/delete?path=rel */
static esp_err_t h_fs_delete(httpd_req_t *r)
{
    if (sd_guard(r) != ESP_OK) return ESP_OK;
    if (fs_not_busy(r) != ESP_OK) return ESP_OK;
    char rel[200] = ""; q_param(r, "path", rel, sizeof(rel));
    char abs[256];
    if (!sdfm_resolve(rel, abs, sizeof(abs)) || rel[0] == '\0') return bad_path(r);
    rm_recursive(abs);
    return httpd_resp_sendstr(r, "{\"ok\":true}");
}

/* POST /api/fs/rename?path=rel&to=rel2 */
static esp_err_t h_fs_rename(httpd_req_t *r)
{
    if (sd_guard(r) != ESP_OK) return ESP_OK;
    if (fs_not_busy(r) != ESP_OK) return ESP_OK;
    char rel[200] = "", rel2[200] = "";
    q_param(r, "path", rel, sizeof(rel));
    q_param(r, "to", rel2, sizeof(rel2));
    char a[256], b[256];
    if (!sdfm_resolve(rel, a, sizeof(a)) || !sdfm_resolve(rel2, b, sizeof(b)) ||
        rel[0] == '\0' || rel2[0] == '\0') return bad_path(r);
    if (rename(a, b) != 0) { httpd_resp_set_status(r, "500 Internal Server Error");
        return httpd_resp_sendstr(r, "{\"error\":\"rename failed\"}"); }
    return httpd_resp_sendstr(r, "{\"ok\":true}");
}

static esp_err_t h_uart(httpd_req_t *r)
{
    static char buf[KUART_LOG_SZ + 1];
    char key[33];
    size_t n = kuart_log_copy(buf, sizeof(buf));
    buf[n] = 0;
    httpd_resp_set_type(r, "text/plain");
    httpd_resp_set_hdr(r, "X-Cpu-Key", kuart_cpukey(key) ? key : "");
    return httpd_resp_send(r, buf, n);
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
    cfg.max_uri_handlers = 20;
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
    reg(srv, "/api/uart",     HTTP_GET,  h_uart);
    reg(srv, "/dump.bin",     HTTP_GET,  h_dump_bin);
    /* SD file manager (confined to /sd) */
    reg(srv, "/api/fs/list",   HTTP_GET,  h_fs_list);
    reg(srv, "/api/fs/get",    HTTP_GET,  h_fs_get);
    reg(srv, "/api/fs/upload", HTTP_POST, h_fs_upload);
    reg(srv, "/api/fs/mkdir",  HTTP_POST, h_fs_mkdir);
    reg(srv, "/api/fs/delete", HTTP_POST, h_fs_delete);
    reg(srv, "/api/fs/rename", HTTP_POST, h_fs_rename);
    ESP_LOGI(TAG, "web UI up");
}
