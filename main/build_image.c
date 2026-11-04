/*
 * build_image.c — native RGH image build pipeline for TrinityWeb360.
 *
 * GPL-3.0. See LICENSE.
 *
 * Streaming version: the dump and the output image are SD-card files that are
 * processed in small chunks. Nothing the size of a full NAND image is ever
 * allocated.
 *
 * ======================= READ THIS BEFORE TRUSTING OUTPUT =======================
 * The data-plumbing stages are implemented. The CRYPTOGRAPHIC stages that make
 * a bootable image (keyvault decrypt/verify, bootloader re-encryption, hash
 * chain) and the ECC regeneration are NOT implemented yet: the output is
 * currently a copy of the dump.
 *
 * The only thing that can certify an image is PARITY: BUILD_OK is returned only
 * if the built image is byte-identical to a known-good image you built in
 * J-Runner from the SAME dump and CPU key, placed on the card as
 * /sd/REFIMG.BIN (raw, with spare, same size as the dump). No reference, a
 * reference that equals the retail dump, or any mismatch = BUILD_NOT_VERIFIED,
 * and the flash path refuses the image.
 * ================================================================================
 */
#include "build_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "nand_geom.h"
#include "nand_ecc.h"
#include "xenon_keys.h"
#include "sdcard.h"

#define REF_FILE SD_MOUNT_POINT "/REFIMG.BIN"

/* First 0x8000 logical bytes hold the header + keyvault region. */
#define HDR_PAGES   (0x8000u / NAND_PAGE_DATA)
#define COPY_PAGES  32u

static const char *TAG = "build";

const char *build_status_str(build_status_t s)
{
    switch (s) {
        case BUILD_OK:            return "Verified";
        case BUILD_NEED_CPU_KEY:  return "CPU key needed";
        case BUILD_NEED_PACKAGE:  return "Data pkg missing";
        case BUILD_BAD_DUMP:      return "Bad dump";
        case BUILD_NOT_VERIFIED:  return "Not verified";
        default:                  return "Build error";
    }
}

static void set_stage(build_progress_t *p, const char *s, int pct)
{
    if (!p) return;
    strncpy((char *)p->stage, s, sizeof(p->stage) - 1);
    p->stage[sizeof(p->stage) - 1] = '\0';
    p->percent = pct;
    ESP_LOGI(TAG, "[%d%%] %s", pct, s);
}

static xebuild_ingredient_t g_ingredients[] = {
    { "cb.bin",           "CB bootloader",       true,  false, 0 },
    { "cbb.bin",          "CB_B bootloader",     true,  false, 0 },
    { "cd.bin",           "CD bootloader",       true,  false, 0 },
    { "ce.bin",           "CE (compressed kernel)", true, false, 0 },
    { "patches.bin",      "glitch patch set",    true,  false, 0 },
    { "smc.bin",          "glitch SMC",          true,  false, 0 },
    { "smc_config.bin",   "SMC config (per-box)", false, false, 0 },
    { "xell-reloaded.bin","xeLL (first-boot CPU key reader)", false, false, 0 },
};
#define N_INGREDIENTS (sizeof(g_ingredients)/sizeof(g_ingredients[0]))

/* Chunked file copy (start of the image = the user's own raw dump). */
static bool copy_file(const char *src, const char *dst,
                      build_progress_t *prog, int p0, int p1)
{
    const size_t chunk = (size_t)COPY_PAGES * NAND_PAGE_RAW;
    uint8_t *buf = malloc(chunk);
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    bool ok = (buf && in && out);

    size_t total = 0, n;
    while (ok && (n = fread(buf, 1, chunk, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
        total += n;
        set_stage(prog, "assembling glitch image",
                  p0 + (int)((uint64_t)total * (p1 - p0) / NAND_16MB_RAW_BYTES));
    }
    if (ok && total != NAND_16MB_RAW_BYTES) ok = false;

    if (in) fclose(in);
    if (out && fclose(out) != 0) ok = false;
    free(buf);
    return ok;
}

/* 1 = identical, 0 = differ, -1 = I/O error. */
static int files_equal(const char *a, const char *b)
{
    const size_t chunk = (size_t)COPY_PAGES * NAND_PAGE_RAW;
    uint8_t *ba = malloc(chunk), *bb = malloc(chunk);
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    int res = -1;

    if (ba && bb && fa && fb) {
        res = 1;
        for (;;) {
            size_t na = fread(ba, 1, chunk, fa);
            size_t nb = fread(bb, 1, chunk, fb);
            if (na != nb || memcmp(ba, bb, na) != 0) { res = 0; break; }
            if (na == 0) break;
        }
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    free(ba);
    free(bb);
    return res;
}

build_status_t build_rgh_image(const char *dump_path,
                               const uint8_t cpu_key[16],
                               const char *out_path,
                               build_progress_t *prog)
{
    build_status_t res = BUILD_ERROR;
    uint8_t *raw = NULL, *logical = NULL;
    xenon_secrets_t *secrets = NULL;
    FILE *f = NULL;

    if (prog) prog->status = BUILD_ERROR;
    if (!dump_path || !out_path) return BUILD_ERROR;

    struct stat sb;
    if (stat(dump_path, &sb) != 0 || (uint32_t)sb.st_size != NAND_16MB_RAW_BYTES) {
        return BUILD_BAD_DUMP;
    }

    /* --- Stage 1: read only the header region, strip spare --- */
    set_stage(prog, "reading dump header", 5);
    raw     = malloc((size_t)HDR_PAGES * NAND_PAGE_RAW);
    logical = malloc((size_t)HDR_PAGES * NAND_PAGE_DATA);
    f = fopen(dump_path, "rb");
    if (!raw || !logical || !f ||
        fread(raw, NAND_PAGE_RAW, HDR_PAGES, f) != HDR_PAGES) {
        ESP_LOGE(TAG, "cannot read dump header");
        goto out;
    }
    fclose(f); f = NULL;
    nand_raw_to_logical(raw, HDR_PAGES, logical);

    /* --- Stage 2: locate per-console secrets (heap: struct is >16 KB) --- */
    set_stage(prog, "extracting console secrets", 15);
    secrets = calloc(1, sizeof(*secrets));
    if (!secrets) goto out;
    if (xenon_extract_secrets(logical, HDR_PAGES * NAND_PAGE_DATA, secrets) < 0) {
        res = BUILD_BAD_DUMP;
        goto out;
    }
    if (!cpu_key) { res = BUILD_NEED_CPU_KEY; goto out; }
    xenon_set_cpu_key(secrets, cpu_key);

    /* --- Stage 3: validate the SD package --- */
    set_stage(prog, "checking xeBuild data package", 25);
    if (!sd_is_mounted() || sd_check_package(g_ingredients, N_INGREDIENTS) > 0) {
        res = BUILD_NEED_PACKAGE;
        goto out;
    }

    /* --- Stage 4: keyvault decrypt + verify (GATED, placeholder) --- */
    set_stage(prog, "keyvault decrypt + verify", 40);

    /* --- Stage 5: assemble image (placeholder: copy of the user's dump) --- */
    set_stage(prog, "assembling glitch image", 45);
    if (!copy_file(dump_path, out_path, prog, 45, 85)) {
        ESP_LOGE(TAG, "image write failed");
        res = BUILD_ERROR;
        goto out;
    }

    /* --- Stage 6: regenerate ECC (NOT implemented; must also stream) --- */
    set_stage(prog, "regenerating ECC", 90);

    /* --- Stage 7: parity check against a known-good J-Runner image --- */
    set_stage(prog, "parity check vs REFIMG.BIN", 95);
    res = BUILD_NOT_VERIFIED;
    if (stat(REF_FILE, &sb) == 0 && (uint32_t)sb.st_size == NAND_16MB_RAW_BYTES &&
        files_equal(REF_FILE, dump_path) == 0 &&   /* a glitch image != retail dump */
        files_equal(REF_FILE, out_path) == 1) {
        res = BUILD_OK;
    } else {
        ESP_LOGW(TAG, "no parity with %s: image NOT certified for flashing", REF_FILE);
    }
    set_stage(prog, "done", 100);

out:
    if (f) fclose(f);
    free(raw);
    free(logical);
    free(secrets);
    if (prog) prog->status = res;
    return res;
}
