/*
 * jobs.c — shared dump/build/flash engine for TrinityWeb360 (standalone).
 *
 * GPL-3.0. See LICENSE.
 *
 * A raw Trinity dump is ~16.5 MiB but the N16R8 only has 8 MB of PSRAM, so
 * nothing here ever holds a full dump in RAM. Dump/build/flash all stream
 * through the SD card in small chunks. File names are 8.3 so they work with or
 * without FATFS long-filename support.
 */
#include "jobs.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"

#include "nand_geom.h"
#include "xbox.h"
#include "sdcard.h"
#include "build_image.h"
#include "rgb.h"
#include "nand_ecc.h"
#include "esp_heap_caps.h"

static const char *TAG = "jobs";

#define DUMP_FILE   SD_MOUNT_POINT "/NANDDUMP.BIN" /* last verified dump   */
#define BACKUP_FILE SD_MOUNT_POINT "/BACKUP.BIN"   /* 2nd copy of the dump */
#define DUMP_TMP_A  SD_MOUNT_POINT "/DUMP_A.BIN"   /* read pass 1 (temp)   */
#define DUMP_TMP_B  SD_MOUNT_POINT "/DUMP_B.BIN"   /* read pass 2 (temp)   */
#define IMAGE_FILE  SD_MOUNT_POINT "/IMAGE.BIN"    /* built image          */

#define DUMP_PASSES 2   /* every pass must be byte-identical */

#define CHUNK_PAGES 32u
#define CHUNK_BYTES ((size_t)CHUNK_PAGES * NAND_PAGE_RAW)

static struct {
    SemaphoreHandle_t lock;
    job_t   job;
    bool    busy;
    int     percent;
    char    stage[48];
    bool    done;
    bool    ok;
    bool    flashable;
    bool    have_dump;
    char    cpu_key_hex[33];

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

/* Read the whole NAND into `path`, one chunk at a time. */
static bool dump_once(const char *path, int pct0, int span, const char *stage)
{
    uint32_t pages = trinity_pages();
    uint8_t *buf = malloc(CHUNK_BYTES);
    FILE *f = fopen(path, "wb");
    bool ok = (buf && f);
    uint32_t n = 0;

    for (uint32_t p = 0; ok && p < pages; p++) {
        uint8_t *page = buf + (size_t)n * NAND_PAGE_RAW;
        uint32_t ret = xbox_nand_read_block(p, page, page + NAND_PAGE_DATA);
        if (ret != 0) {
            ESP_LOGE(TAG, "read error at page %lu: 0x%lx",
                     (unsigned long)p, (unsigned long)ret);
            ok = false;
            break;
        }
        if (++n == CHUNK_PAGES || p == pages - 1) {
            if (fwrite(buf, NAND_PAGE_RAW, n, f) != n) {
                ok = false;
                break;
            }
            n = 0;
            if (((p + 1) & 0x3FF) == 0) {
                st(pct0 + (int)((uint64_t)(p + 1) * span / pages),
                   stage, false, false);
            }
        }
    }

    if (f && fclose(f) != 0) ok = false;
    free(buf);
    return ok;
}

/* 1 = identical, 0 = differ, -1 = I/O error. */
static int files_equal(const char *a, const char *b)
{
    uint8_t *ba = malloc(CHUNK_BYTES);
    uint8_t *bb = malloc(CHUNK_BYTES);
    FILE *fa = fopen(a, "rb");
    FILE *fb = fopen(b, "rb");
    int res = -1;

    if (ba && bb && fa && fb) {
        res = 1;
        for (;;) {
            size_t na = fread(ba, 1, CHUNK_BYTES, fa);
            size_t nb = fread(bb, 1, CHUNK_BYTES, fb);
            if (na != nb || memcmp(ba, bb, na) != 0) {
                res = 0;
                break;
            }
            if (na == 0) break;
        }
    }

    if (fa) fclose(fa);
    if (fb) fclose(fb);
    free(ba);
    free(bb);
    return res;
}

/* The saved dump and its second copy must still be identical. */
static bool backup_ok(void)
{
    return files_equal(DUMP_FILE, BACKUP_FILE) == 1;
}

static void job_dump(void)
{
    static const char *const tmp[DUMP_PASSES] = {
        DUMP_TMP_A, DUMP_TMP_B
    };
    char label[16];
    uint8_t probe[NAND_PAGE_RAW];
    int i, eq;

    if (!sd_is_mounted()) {
        st(0, "No SD card", true, false);
        return;
    }

    /* Gate: never create a dump file unless a real console answered. */
    if (!S.linked || !xbox_smc_stopped) {
        st(0, "Detect first", true, false);
        return;
    }

    memset(probe, 0xA5, sizeof(probe));
    if (xbox_nand_read_block(0, probe, probe + NAND_PAGE_DATA) != 0 ||
        probe[0] != 0xFF || probe[1] != 0x4F) {
        /* 360 NAND header magic */
        ESP_LOGW(TAG, "page 0 has no 360 header (got %02x %02x)",
                 probe[0], probe[1]);
        st(0, "No console data", true, false);
        return;
    }

    for (i = 0; i < DUMP_PASSES; i++) {
        snprintf(label, sizeof(label), "Reading %d/%d",
                 i + 1, DUMP_PASSES);
        st(2 + i * (90 / DUMP_PASSES), label, false, false);

        if (!dump_once(tmp[i], 2 + i * (90 / DUMP_PASSES),
                       90 / DUMP_PASSES, label)) {
            st(0, "Read/SD error", true, false);
            goto done;
        }
    }

    st(94, "Comparing", false, false);

    for (i = 1; i < DUMP_PASSES; i++) {
        eq = files_equal(tmp[0], tmp[i]);
        if (eq < 0) {
            st(0, "SD error", true, false);
            goto done;
        }
        if (eq == 0) {
            st(0, "Reads differ", true, false);
            goto done;
        }
    }

    /* From here the old dump/image are being replaced. */
    xSemaphoreTake(S.lock, portMAX_DELAY);
    S.have_dump = false;
    S.flashable = false;
    xSemaphoreGive(S.lock);

    remove(DUMP_FILE);
    remove(BACKUP_FILE);
    remove(IMAGE_FILE);

    if (rename(tmp[0], DUMP_FILE) != 0 ||
        rename(tmp[1], BACKUP_FILE) != 0) {
        st(0, "SD error", true, false);
        goto done;
    }

    st(97, "Checking backup", false, false);
    if (!backup_ok()) {
        st(0, "Backup mismatch", true, false);
        goto done;
    }

    xSemaphoreTake(S.lock, portMAX_DELAY);
    S.have_dump = true;
    xSemaphoreGive(S.lock);

    st(100, "Dump verified", true, true);

done:
    for (i = 0; i < DUMP_PASSES; i++) remove(tmp[i]);
}

static int hex2bin(const char *hex, uint8_t *out, int out_len)
{
    if ((int)strlen(hex) != out_len * 2) return -1;

    for (int i = 0; i < out_len; i++) {
        char c[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        char *e;
        long v = strtol(c, &e, 16);
        if (*e) return -1;
        out[i] = (uint8_t)v;
    }

    return 0;
}

static void job_build(void)
{
    uint8_t cpu_key[16];
    const uint8_t *cpu_key_p = NULL;

    if (!S.have_dump) {
        st(0, "Dump first", true, false);
        return;
    }

    st(1, "Checking backup", false, false);
    if (!backup_ok()) {
        st(0, "Backup mismatch", true, false);
        return;
    }

    /* The CPU key is NOT required: the RGH3 chain is console-independent and the
     * KeyVault is copied verbatim from the dump, so no key is needed to build.
     * Pass it through only if a valid cpukey.txt happens to be present. */
    if (hex2bin(S.cpu_key_hex, cpu_key, 16) == 0) {
        cpu_key_p = cpu_key;
    }

    S.flashable = false;
    build_progress_t bp = { 0 };
    build_status_t bs = build_rgh_image(DUMP_FILE, cpu_key_p,
                                        IMAGE_FILE, &bp);

    if (bs == BUILD_OK) {
        S.flashable = true;
        st(100, "Image verified", true, true);
    } else {
        st(bp.percent, build_status_str(bs), true, false);
    }
}

/* Read the NAND back and compare every page (data + spare) with the image. */
static bool verify_flash(uint32_t pages)
{
    uint8_t *want = malloc(CHUNK_BYTES);
    uint8_t *got  = malloc(CHUNK_BYTES);
    FILE *f = fopen(IMAGE_FILE, "rb");
    bool ok = (want && got && f);
    uint32_t lba = 0;

    while (ok && lba < pages) {
        uint32_t n = (uint32_t)fread(want, NAND_PAGE_RAW,
                                     CHUNK_PAGES, f);
        if (n == 0) {
            ok = false;
            break;
        }

        for (uint32_t i = 0; i < n; i++) {
            uint8_t *page = got + (size_t)i * NAND_PAGE_RAW;
            if (xbox_nand_read_block(lba + i, page,
                                     page + NAND_PAGE_DATA) != 0) {
                ok = false;
                break;
            }
        }

        if (!ok) break;

        if (memcmp(want, got, (size_t)n * NAND_PAGE_RAW) != 0) {
            ESP_LOGE(TAG, "verify mismatch in pages %lu..%lu",
                     (unsigned long)lba,
                     (unsigned long)(lba + n - 1));
            ok = false;
            break;
        }

        lba += n;

        if ((lba & 0x3FF) == 0) {
            st(50 + (int)((uint64_t)lba * 50 / pages),
               "Verifying write", false, false);
        }
    }

    if (ok && lba != pages) ok = false;

    if (f) fclose(f);
    free(want);
    free(got);
    return ok;
}

/* ---------------- Write XeLL (RGH3) ----------------
 * Same steps as J-Runner's "Write XeLL" with RGH3 ticked:
 *   1. start from J-Runner's prebuilt TRINITY_RGH3.ecc (RGH3 CB_A/CB_X + SMC)
 *   2. drop in this console's own raw keyvault pages from the dump
 *   3. optionally swap in a newer xell-gggggg.bin and re-ECC those pages
 *   4. write the 80 blocks at the start of the NAND, then read back + compare
 * Nothing past the ECC image is touched, and no CPU key is needed. */
#define XELL_ECC_RAW   1351680u        /* 2560 pages x 0x210          */
#define XELL_ECC_PLAIN 1310720u        /* same image without spare     */
#define XELL_PAGES     (XELL_ECC_RAW / NAND_PAGE_RAW)
#define XELL_BIN_SIZE  0x40000u
#define KV_RAW_OFF     0x4200u
#define KV_RAW_LEN     0x4200u
#define XELL_OUT_FILE  SD_MOUNT_POINT "/XELL.ECC"

static const uint8_t XELL_SIG[16] = {
    0x48,0x00,0x00,0x20,0x48,0x00,0x00,0xEC,0x48,0x00,0x00,0x00,0x48,0x00,0x00,0x00 };

static size_t read_whole(const char *path, uint8_t *dst, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(dst, 1, cap, f);
    if (fgetc(f) != EOF) n = cap + 1;     /* larger than expected */
    fclose(f);
    return n;
}

static void job_xell(void)
{
    static const char *ecc_paths[] = {
        SD_MOUNT_POINT "/xell/TRINITY_RGH3.ecc",
        SD_MOUNT_POINT "/TRINITY_RGH3.ecc",
        SD_MOUNT_POINT "/common/xell-images/glitch2/TRINITY_RGH3.ecc",
    };
    static const char *xell_paths[] = {
        XEBUILD_DATA_ROOT "/xell-gggggg.bin",
        SD_MOUNT_POINT "/xell/xell-gggggg.bin",
    };

    if (!S.linked || !xbox_smc_stopped) { st(0, "Detect first", true, false); return; }
    if (!S.have_dump) { st(0, "Dump first", true, false); return; }

    st(1, "Checking backup", false, false);
    if (!backup_ok()) { st(0, "Backup mismatch", true, false); return; }

    uint8_t *img  = heap_caps_malloc(XELL_ECC_RAW + 1, MALLOC_CAP_SPIRAM);
    uint8_t *xell = heap_caps_malloc(XELL_BIN_SIZE + 1, MALLOC_CAP_SPIRAM);
    if (!img || !xell) { free(img); free(xell); st(0, "Out of memory", true, false); return; }

    /* 1. template */
    st(3, "Loading RGH3 ECC", false, false);
    size_t n = 0;
    for (size_t i = 0; i < sizeof(ecc_paths) / sizeof(ecc_paths[0]) && !n; i++)
        n = read_whole(ecc_paths[i], img, XELL_ECC_RAW);
    if (n == XELL_ECC_PLAIN) {
        /* no-spare variant: spread to raw pages, spare is rebuilt below */
        for (int p = (int)XELL_PAGES - 1; p >= 0; p--) {
            memmove(img + (size_t)p * NAND_PAGE_RAW, img + (size_t)p * NAND_PAGE_DATA, NAND_PAGE_DATA);
            nand_ecc_make_spare(img + (size_t)p * NAND_PAGE_RAW, (uint32_t)p,
                                img + (size_t)p * NAND_PAGE_RAW + NAND_PAGE_DATA);
        }
    } else if (n != XELL_ECC_RAW) {
        free(img); free(xell);
        st(0, n ? "Bad RGH3 ECC size" : "Need TRINITY_RGH3", true, false);
        return;
    }
    if (img[0] != 0xFF || img[1] != 0x4F) {
        free(img); free(xell); st(0, "Bad RGH3 ECC", true, false); return;
    }

    /* 2. this console's keyvault (raw pages incl. spare, as J-Runner does) */
    st(6, "Injecting keyvault", false, false);
    FILE *d = fopen(DUMP_FILE, "rb");
    uint8_t kvspare[NAND_PAGE_SPARE];
    bool kv_ok = d && fseek(d, KV_RAW_OFF, SEEK_SET) == 0 &&
                 fread(img + KV_RAW_OFF, 1, KV_RAW_LEN, d) == KV_RAW_LEN;
    if (d) fclose(d);
    memcpy(kvspare, img + KV_RAW_OFF + NAND_PAGE_DATA, NAND_PAGE_SPARE);
    if (!kv_ok) { free(img); free(xell); st(0, "Dump read error", true, false); return; }
    /* spare layout of the dump must match the template's (Trinity: 00 .. FF) */
    {
        const uint8_t *tsp = img + 0x8400 + NAND_PAGE_DATA;   /* page after KV */
        if (kvspare[5] != tsp[5] || (kvspare[0] == 0xFF) != (tsp[0] == 0xFF)) {
            free(img); free(xell); st(0, "NAND layout mismatch", true, false); return;
        }
    }

    /* 3. optional newer XeLL */
    size_t xn = 0;
    for (size_t i = 0; i < sizeof(xell_paths) / sizeof(xell_paths[0]) && !xn; i++)
        xn = read_whole(xell_paths[i], xell, XELL_BIN_SIZE);
    if (xn == XELL_BIN_SIZE && memcmp(xell, XELL_SIG, 16) == 0) {
        static const uint32_t offs[] = { 0x70000, 0xC0000, 0x100000, 0xE0000 };
        int injected = 0;
        for (size_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
            uint32_t pg0 = offs[i] / NAND_PAGE_DATA;
            if (pg0 + XELL_BIN_SIZE / NAND_PAGE_DATA > XELL_PAGES) continue;
            uint8_t *base = img + (size_t)pg0 * NAND_PAGE_RAW;
            if (memcmp(base, XELL_SIG, 16) != 0) continue;
            for (uint32_t k = 0; k < XELL_BIN_SIZE / NAND_PAGE_DATA; k++) {
                uint8_t *pg = base + (size_t)k * NAND_PAGE_RAW;
                memcpy(pg, xell + (size_t)k * NAND_PAGE_DATA, NAND_PAGE_DATA);
                nand_ecc_fix_page(pg);
            }
            injected++;
        }
        ESP_LOGI(TAG, "XeLL injected into %d slot(s)", injected);
    } else {
        ESP_LOGI(TAG, "no xell-gggggg.bin, keeping the ECC's own XeLL");
    }
    free(xell);

    /* final self-check: every page's ECC must be consistent */
    for (uint32_t p = 0; p < XELL_PAGES; p++) {
        uint8_t *pg = img + (size_t)p * NAND_PAGE_RAW;
        uint32_t e = nand_ecc_calc(pg);
        uint32_t have = (uint32_t)pg[0x20C] | ((uint32_t)pg[0x20D] << 8) |
                        ((uint32_t)pg[0x20E] << 16) | ((uint32_t)pg[0x20F] << 24);
        if ((e & ~0x3Fu) != (have & ~0x3Fu)) {
            ESP_LOGE(TAG, "ECC mismatch on page %lu", (unsigned long)p);
            free(img); st(0, "ECC check failed", true, false); return;
        }
    }

    FILE *o = fopen(XELL_OUT_FILE, "wb");
    if (o) { fwrite(img, 1, XELL_ECC_RAW, o); fclose(o); }

    /* 4. write + verify */
    for (uint32_t p = 0; p < XELL_PAGES; p++) {
        uint8_t *pg = img + (size_t)p * NAND_PAGE_RAW;
        if (xbox_nand_write_block(p, pg, pg + NAND_PAGE_DATA) != 0) {
            free(img); st(0, "Write error", true, false); return;
        }
        if ((p & 0x7F) == 0) st(10 + (int)(p * 60 / XELL_PAGES), "Writing XeLL", false, false);
    }
    rgb_verifying();
    uint8_t got[NAND_PAGE_RAW];
    for (uint32_t p = 0; p < XELL_PAGES; p++) {
        if (xbox_nand_read_block(p, got, got + NAND_PAGE_DATA) != 0 ||
            memcmp(got, img + (size_t)p * NAND_PAGE_RAW, NAND_PAGE_RAW) != 0) {
            ESP_LOGE(TAG, "XeLL verify mismatch at page %lu", (unsigned long)p);
            free(img); st(0, "Verify failed", true, false); return;
        }
        if ((p & 0x7F) == 0) st(70 + (int)(p * 30 / XELL_PAGES), "Verifying XeLL", false, false);
    }
    free(img);
    st(100, "XeLL written - boot it", true, true);
}

static void job_flash(void)
{
    uint32_t pages = trinity_pages();
    struct stat sb;

    if (!S.flashable) {
        st(0, "Not verified", true, false);
        return;
    }

    if (!S.linked || !xbox_smc_stopped) {
        st(0, "Detect first", true, false);
        return;
    }

    st(1, "Checking backup", false, false);
    if (!backup_ok()) {
        st(0, "Backup mismatch", true, false);
        return;
    }

    if (stat(IMAGE_FILE, &sb) != 0 ||
        (uint32_t)sb.st_size != NAND_16MB_RAW_BYTES) {
        st(0, "Image missing", true, false);
        return;
    }

    uint8_t *buf = malloc(CHUNK_BYTES);
    FILE *f = fopen(IMAGE_FILE, "rb");

    if (!buf || !f) {
        if (f) fclose(f);
        free(buf);
        st(0, "Image missing", true, false);
        return;
    }

    uint32_t lba = 0;
    bool ok = true;

    while (ok && lba < pages) {
        uint32_t n = (uint32_t)fread(buf, NAND_PAGE_RAW,
                                     CHUNK_PAGES, f);
        if (n == 0) {
            ok = false;
            break;
        }

        for (uint32_t i = 0; i < n && lba < pages; i++, lba++) {
            uint8_t *page = buf + (size_t)i * NAND_PAGE_RAW;

            if (xbox_nand_write_block(lba, page,
                                      page + NAND_PAGE_DATA) != 0) {
                ok = false;
                break;
            }

            if ((lba & 0x1FF) == 0) {
                st((int)((uint64_t)lba * 50 / pages),
                   "Writing NAND", false, false);
            }
        }
    }

    fclose(f);
    free(buf);

    if (!ok || lba != pages) {
        st(0, "Write error", true, false);
        return;
    }

    st(50, "Verifying write", false, false);
    rgb_verifying();

    if (verify_flash(pages)) {
        /* Release the SMC so the console cold-boots into the freshly-flashed
         * RGH3 image (DBG_EN was held high since detect, halting the console).
         * After this, re-detect is needed before any further NAND op. */
        st(99, "Restarting console", false, false);
        xbox_start_smc();
        xSemaphoreTake(S.lock, portMAX_DELAY);
        S.linked = false;
        xSemaphoreGive(S.lock);
        st(100, "Flash verified", true, true);
    } else {
        st(0, "Verify failed", true, false);
    }
}

static void worker_task(void *arg)
{
    /*
     * Validate an existing dump in the background at low priority.
     * This prevents the file comparison from delaying LCD/UI startup.
     */
    vTaskPrioritySet(NULL, 1);
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Give the SD card up to 5 seconds to mount. */
    for (int i = 0; i < 20 && !sd_is_mounted(); i++) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    bool valid_dump = false;
    struct stat dump_sb, backup_sb;

    if (sd_is_mounted() &&
        stat(DUMP_FILE, &dump_sb) == 0 &&
        stat(BACKUP_FILE, &backup_sb) == 0 &&
        (uint32_t)dump_sb.st_size == NAND_16MB_RAW_BYTES &&
        (uint32_t)backup_sb.st_size == NAND_16MB_RAW_BYTES) {
        valid_dump = backup_ok();
    }

    xSemaphoreTake(S.lock, portMAX_DELAY);
    S.have_dump = valid_dump;
    xSemaphoreGive(S.lock);

    /* Restore normal worker priority after validation. */
    vTaskPrioritySet(NULL, 5);

    for (;;) {
        job_t job = JOB_NONE;

        xSemaphoreTake(S.lock, portMAX_DELAY);
        if (S.busy && S.job != JOB_NONE) {
            job = S.job;
            S.job = JOB_NONE;
        }
        xSemaphoreGive(S.lock);

        if (job == JOB_NONE) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* Set the operation color before starting the job. */
        switch (job) {
            case JOB_DUMP:
                rgb_dumping();
                job_dump();
                break;

            case JOB_BUILD:
                rgb_building();
                job_build();
                break;

            case JOB_FLASH:
                rgb_flashing();
                job_flash();
                break;

            case JOB_XELL:
                rgb_flashing();
                job_xell();
                break;

            default:
                break;
        }

        xSemaphoreTake(S.lock, portMAX_DELAY);
        bool job_done = S.done;
        bool job_ok = S.ok;
        S.busy = false;
        xSemaphoreGive(S.lock);

        /* Keep failures red, verified flashes green, other success white. */
        if (job_done) {
            if (!job_ok) {
                rgb_error();
            } else if (job == JOB_FLASH) {
                rgb_complete();
            } else {
                rgb_ready();
            }
        }
    }
}

void jobs_init(void)
{
    S.lock = xSemaphoreCreateMutex();
    strcpy(S.stage, "idle");

    /* Validation now happens inside worker_task, after startup. */
    xTaskCreate(worker_task, "jobs", 8192, NULL, 5, NULL);
}

bool jobs_start(job_t job)
{
    bool ok = false;

    xSemaphoreTake(S.lock, portMAX_DELAY);

    if (!S.busy && (job == JOB_NONE || S.linked)) {
        S.busy = true;
        S.job = job;
        S.done = false;
        S.percent = 0;
        strcpy(S.stage, "starting");
        ok = true;
    }

    xSemaphoreGive(S.lock);
    return ok;
}

/* J-Runner over USB borrows the console the same way a job does: while it
 * holds the claim, the touch/web UI see "busy" and can't touch the SMC/NAND. */
static bool usb_claimed;

bool jobs_usb_claim(void)
{
    bool ok = false;
    xSemaphoreTake(S.lock, portMAX_DELAY);
    if (usb_claimed) {
        ok = true;
    } else if (!S.busy) {
        S.busy = true;
        S.job = JOB_NONE;
        S.done = false;
        S.percent = 0;
        strcpy(S.stage, "J-Runner (USB)");
        usb_claimed = ok = true;
    }
    xSemaphoreGive(S.lock);
    return ok;
}

void jobs_usb_release(void)
{
    xSemaphoreTake(S.lock, portMAX_DELAY);
    if (usb_claimed) {
        usb_claimed = false;
        S.busy = false;
        strcpy(S.stage, "idle");
    }
    xSemaphoreGive(S.lock);
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
    out->have_dump = S.have_dump;
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
    FILE *f = fopen(SD_MOUNT_POINT "/cpukey.txt", "rb");
    if (!f) return false;

    char buf[64] = { 0 };
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);

    if (n < 32) return false;

    /* pull the first 32 hex chars */
    char hex[33];
    int k = 0;

    for (size_t i = 0; i < n && k < 32; i++) {
        char c = buf[i];

        if ((c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F')) {
            hex[k++] = c;
        }
    }

    if (k < 32) return false;

    hex[32] = 0;
    return jobs_set_cpukey_hex(hex);
}

void jobs_detect(void)
{
    xbox_stop_smc();                      /* only toggles pins: always "succeeds" */
    uint32_t fc = xbox_get_flash_config();

    /* 0 / all-ones = nothing answered; 0xC0462002 = eMMC console (Corona 4GB),
     * which this firmware cannot dump or flash. */
    bool plausible = (fc != 0 &&
                      fc != 0xFFFFFFFFu &&
                      fc != 0xC0462002u);

    if (!plausible) {
        ESP_LOGW(TAG, "no usable flash config (0x%08lx)",
                 (unsigned long)fc);
    }

    xSemaphoreTake(S.lock, portMAX_DELAY);
    S.flash_config = plausible ? fc : 0;
    S.linked = plausible && xbox_smc_stopped;
    xSemaphoreGive(S.lock);
}