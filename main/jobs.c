/*
 * jobs.c — shared dump/build/flash engine for TrinityWeb360 (standalone).
 *
 * GPL-3.0. See LICENSE.
 */
#include "jobs.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "nand_geom.h"
#include "xbox.h"
#include "sdcard.h"
#include "build_image.h"

static const char *TAG = "jobs";

static struct {
    SemaphoreHandle_t lock;
    job_t   job;
    bool    busy;
    int     percent;
    char    stage[48];
    bool    done;
    bool    ok;
    bool    flashable;
    char    cpu_key_hex[33];

    uint8_t *dump;
    uint32_t dump_len;
    uint8_t *image;
    uint32_t image_len;

    uint32_t flash_config;
    bool     linked;
} S;

static void st(int pct, const char *stage, bool done, bool ok)
{
    xSemaphoreTake(S.lock, portMAX_DELAY);
    S.percent = pct;
    strncpy(S.stage, stage, sizeof(S.stage) - 1);
    S.stage[sizeof(S.stage) - 1] = '\0';
    S.done = done;
    if (done) S.ok = ok;
    xSemaphoreGive(S.lock);
}

static uint32_t trinity_pages(void) { return NAND_16MB_PAGES; }

static bool dump_once(uint8_t *dst)
{
    uint32_t pages = trinity_pages();
    for (uint32_t p = 0; p < pages; p++) {
        uint8_t *page = dst + (size_t)p * NAND_PAGE_RAW;
        uint32_t ret = xbox_nand_read_block(p, page, page + NAND_PAGE_DATA);
        if (ret != 0) {
            ESP_LOGE(TAG, "read error at page %lu: 0x%lx",
                     (unsigned long)p, (unsigned long)ret);
            return false;
        }
        if ((p & 0x3FF) == 0) {
            st(5 + (int)(p * 40 / pages), "reading NAND", false, false);
        }
    }
    return true;
}

static void job_dump(void)
{
    uint32_t pages = trinity_pages();
    uint32_t len = pages * NAND_PAGE_RAW;

    if (S.dump) { heap_caps_free(S.dump); S.dump = NULL; }
    uint8_t *a = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    uint8_t *b = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!a || !b) { st(0, "out of PSRAM", true, false); goto done; }

    st(5, "reading NAND (pass 1)", false, false);
    if (!dump_once(a)) { st(0, "read failed", true, false); goto done; }

    st(50, "reading NAND (pass 2)", false, false);
    if (!dump_once(b)) { st(0, "read failed", true, false); goto done; }

    st(92, "comparing reads", false, false);
    if (memcmp(a, b, len) != 0) {
        st(0, "reads differ - check wiring", true, false);
        goto done;
    }

    S.dump = a; S.dump_len = len; a = NULL;
    st(100, "dump verified", true, true);

done:
    if (a) heap_caps_free(a);
    if (b) heap_caps_free(b);
}

static int hex2bin(const char *hex, uint8_t *out, int out_len)
{
    if ((int)strlen(hex) != out_len * 2) return -1;
    for (int i = 0; i < out_len; i++) {
        char c[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        char *e; long v = strtol(c, &e, 16);
        if (*e) return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

static void job_build(void)
{
    if (!S.dump) { st(0, "dump first", true, false); return; }

    uint8_t cpu_key[16];
    if (hex2bin(S.cpu_key_hex, cpu_key, 16) != 0) {
        st(0, "need cpukey.txt on SD", true, false);
        return;
    }

    if (S.image) { heap_caps_free(S.image); S.image = NULL; }
    S.image = heap_caps_malloc(S.dump_len, MALLOC_CAP_SPIRAM);
    if (!S.image) { st(0, "out of PSRAM", true, false); return; }

    build_progress_t bp = { 0 };
    build_status_t bs = build_rgh_image(S.dump, S.dump_len, cpu_key, S.image, &bp);

    if (bs == BUILD_OK) {
        S.image_len = S.dump_len;
        S.flashable = true;
        st(100, "image ready to flash", true, true);
    } else {
        S.flashable = false;
        st(bp.percent ? bp.percent : 0, build_status_str(bs), true, false);
    }
}

static void job_flash(void)
{
    if (!S.flashable || !S.image) {
        st(0, "no verified image", true, false);
        return;
    }
    uint32_t pages = S.image_len / NAND_PAGE_RAW;
    for (uint32_t p = 0; p < pages; p++) {
        uint8_t *page = S.image + (size_t)p * NAND_PAGE_RAW;
        uint32_t ret = xbox_nand_write_block(p, page, page + NAND_PAGE_DATA);
        if (ret != 0) { st(0, "write error", true, false); return; }
        if ((p & 0x1FF) == 0) st((int)(p * 100 / pages), "writing NAND", false, false);
    }
    st(100, "flash complete", true, true);
}

static void worker_task(void *arg)
{
    for (;;) {
        job_t job = JOB_NONE;
        xSemaphoreTake(S.lock, portMAX_DELAY);
        if (S.busy && S.job != JOB_NONE) { job = S.job; S.job = JOB_NONE; }
        xSemaphoreGive(S.lock);

        if (job == JOB_NONE) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

        switch (job) {
            case JOB_DUMP:  job_dump();  break;
            case JOB_BUILD: job_build(); break;
            case JOB_FLASH: job_flash(); break;
            default: break;
        }
        xSemaphoreTake(S.lock, portMAX_DELAY);
        S.busy = false;
        xSemaphoreGive(S.lock);
    }
}

void jobs_init(void)
{
    S.lock = xSemaphoreCreateMutex();
    strcpy(S.stage, "idle");
    xTaskCreate(worker_task, "jobs", 8192, NULL, 5, NULL);
}

bool jobs_start(job_t job)
{
    bool ok = false;
    xSemaphoreTake(S.lock, portMAX_DELAY);
    if (!S.busy) {
        S.busy = true; S.job = job; S.done = false; S.percent = 0;
        strcpy(S.stage, "starting");
        ok = true;
    }
    xSemaphoreGive(S.lock);
    return ok;
}

void jobs_get(job_status_t *out)
{
    xSemaphoreTake(S.lock, portMAX_DELAY);
    out->busy = S.busy;
    out->percent = S.percent;
    strcpy(out->stage, S.stage);
    out->done = S.done;
    out->ok = S.ok;
    out->flashable = S.flashable;
    out->have_dump = (S.dump != NULL);
    out->flash_config = S.flash_config;
    out->linked = S.linked;
    xSemaphoreGive(S.lock);
}

bool jobs_set_cpukey_hex(const char *hex)
{
    uint8_t tmp[16];
    if (hex2bin(hex, tmp, 16) != 0) return false;
    xSemaphoreTake(S.lock, portMAX_DELAY);
    memcpy(S.cpu_key_hex, hex, 32);
    S.cpu_key_hex[32] = 0;
    xSemaphoreGive(S.lock);
    return true;
}

bool jobs_load_cpukey_from_sd(void)
{
    void *f = sd_open_ingredient("../../cpukey.txt"); /* /sd/cpukey.txt */
    if (!f) {
        FILE *g = fopen("/sd/cpukey.txt", "rb");
        if (!g) return false;
        f = g;
    }
    char buf[64] = { 0 };
    size_t n = fread(buf, 1, sizeof(buf) - 1, (FILE *)f);
    fclose((FILE *)f);
    if (n < 32) return false;

    /* pull the first 32 hex chars */
    char hex[33]; int k = 0;
    for (size_t i = 0; i < n && k < 32; i++) {
        char c = buf[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
            hex[k++] = c;
    }
    if (k < 32) return false;
    hex[32] = 0;
    return jobs_set_cpukey_hex(hex);
}

void jobs_detect(void)
{
    xbox_stop_smc();
    uint32_t fc = xbox_get_flash_config();
    xSemaphoreTake(S.lock, portMAX_DELAY);
    S.flash_config = fc;
    S.linked = xbox_smc_stopped;
    xSemaphoreGive(S.lock);
}
