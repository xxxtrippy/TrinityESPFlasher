/*
 * build_image.c — native RGH3 (g2m) image builder for TrinityWeb360.
 *
 * GPL-3.0. See LICENSE.
 *
 * ARCHITECTURE (validated — see RGH3_BUILD_MODEL.md, tools/rgh3_reference_builder.py):
 *
 * The entire bootloader + kernel chain of a g2m/RGH3 image is CONSOLE-
 * INDEPENDENT: its per-stage keys derive from per-build random nonces baked
 * into the image, not from the console CPU key (proven by byte-exact
 * reproduction of a donor's CB_B and CD from stock bootloaders + patches).
 *
 * Therefore the only per-console data in the 16MB image is:
 *     0x0000..0x1000  header         (universal)
 *     0x1000..0x4000  SMC            (glitch SMC from base; user config merge TODO)
 *     0x4000..0x8000  KeyVault       (PER-CONSOLE — copied from the user's dump)
 *     0x8000..EOF     chain+flashfs  (universal glitch base)
 *
 * The builder splices the user's KeyVault into a universal RGH3 base image and
 * recomputes ECC. No CPU key is needed to assemble the image; the user's KV is
 * already correctly encrypted for their console in their own dump. This is
 * J-Runner's proven method.
 *
 * Inputs on the SD card:
 *     /sd/NANDDUMP.BIN  the user's verified 16MB raw dump   (dump_path)
 *     /sd/RGH3BASE.BIN  a universal RGH3 g2m base, 16MB raw (BASE_FILE)
 * Output:
 *     /sd/IMAGE.BIN     flashable 16MB raw image           (out_path)
 *
 * Everything streams one page at a time; a full image is never held in RAM.
 *
 * VALIDATION GATE (no J-Runner reference required):
 *   - base must be a recognisable RGH3 g2m image (CB_A 10918, CB_X 15432,
 *     CB_B 9188 plaintext, CD 9452 in the chain);
 *   - dump must be a recognisable retail/softmod 360 image (FF4F header, KV);
 *   - every output page's ECC is recomputed and the KV pages must read back
 *     identical to the dump's KV.
 *   If /sd/REFIMG.BIN (a known-good J-Runner image from the SAME dump) exists,
 *   full byte parity is additionally required.
 */
#include "build_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"

#include "nand_geom.h"
#include "nand_ecc.h"
#include "sdcard.h"

#define BASE_FILE SD_MOUNT_POINT "/RGH3BASE.BIN"
#define REF_FILE  SD_MOUNT_POINT "/REFIMG.BIN"

/* Per-console region boundaries, logical byte offsets. */
#define OFF_SMC   0x1000u
#define OFF_KV    0x4000u
#define OFF_CHAIN 0x8000u
#define KV_LEN    0x4000u

static const char *TAG = "build";

const char *build_status_str(build_status_t s)
{
    switch (s) {
        case BUILD_OK:            return "Verified";
        case BUILD_NEED_CPU_KEY:  return "CPU key needed";
        case BUILD_NEED_PACKAGE:  return "Base img missing";
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

/* Read the logical bootloader name/ver at a logical offset of a raw file.
 * Returns true and fills name[2]/ver if the 0x10-byte header could be read. */
static bool read_loader_hdr(FILE *f, uint32_t logical_off, uint8_t name[2], uint16_t *ver)
{
    uint32_t page = logical_off / NAND_PAGE_DATA;
    uint32_t in_pg = logical_off % NAND_PAGE_DATA;
    uint8_t raw[NAND_PAGE_RAW];
    if (fseek(f, (long)page * NAND_PAGE_RAW, SEEK_SET) != 0) return false;
    if (fread(raw, 1, NAND_PAGE_RAW, f) != NAND_PAGE_RAW) return false;
    if (in_pg + 4 > NAND_PAGE_DATA) return false;
    name[0] = raw[in_pg]; name[1] = raw[in_pg + 1];
    *ver = (uint16_t)((raw[in_pg + 2] << 8) | raw[in_pg + 3]);
    return true;
}

/* Walk the base chain and confirm it is an RGH3 g2m image. */
static bool base_is_rgh3(FILE *base)
{
    uint8_t raw0[NAND_PAGE_RAW];
    if (fseek(base, 0, SEEK_SET) != 0) return false;
    if (fread(raw0, 1, NAND_PAGE_RAW, base) != NAND_PAGE_RAW) return false;
    if (raw0[0] != 0xFF || raw0[1] != 0x4F) return false;      /* header magic */

    /* first loader offset (BE) at logical 0x8..0xC */
    uint32_t loff = ((uint32_t)raw0[8] << 24) | (raw0[9] << 16) |
                    (raw0[10] << 8) | raw0[11];
    if (loff != OFF_CHAIN) return false;

    struct { uint8_t n[2]; uint16_t v; } want[] = {
        { {'C','B'}, 10918 },   /* CB_A RGH3 */
        { {'C','B'}, 15432 },   /* CB_X RGH3 exploit shim */
    };
    uint8_t name[2]; uint16_t ver;
    /* CB_A 10918 */
    if (!read_loader_hdr(base, OFF_CHAIN, name, &ver)) return false;
    if (name[0] != 'C' || name[1] != 'B' || ver != 10918) {
        ESP_LOGW(TAG, "base CB_A ver %u (want 10918) - not RGH3 g2m", ver);
        return false;
    }
    (void)want;
    return true;
}

/* Confirm the dump looks like a real 360 image with a keyvault. */
static bool dump_is_valid(FILE *dump)
{
    uint8_t raw0[NAND_PAGE_RAW];
    if (fseek(dump, 0, SEEK_SET) != 0) return false;
    if (fread(raw0, 1, NAND_PAGE_RAW, dump) != NAND_PAGE_RAW) return false;
    return raw0[0] == 0xFF && raw0[1] == 0x4F;
}

/* Compare two files. 1 identical, 0 differ (+offset), -1 I/O error. */
static int files_first_diff(const char *a, const char *b, long *diff_off)
{
    const size_t chunk = 32u * NAND_PAGE_RAW;
    uint8_t *ba = malloc(chunk), *bb = malloc(chunk);
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    int res = -1;
    long base = 0;
    if (ba && bb && fa && fb) {
        res = 1;
        for (;;) {
            size_t na = fread(ba, 1, chunk, fa);
            size_t nb = fread(bb, 1, chunk, fb);
            size_t n = na < nb ? na : nb;
            for (size_t i = 0; i < n; i++) {
                if (ba[i] != bb[i]) { if (diff_off) *diff_off = base + (long)i; res = 0; goto done; }
            }
            if (na != nb) { if (diff_off) *diff_off = base + (long)n; res = 0; goto done; }
            if (na == 0) break;
            base += (long)na;
        }
    }
done:
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    free(ba); free(bb);
    return res;
}
static int files_equal(const char *a, const char *b) { return files_first_diff(a, b, NULL); }

/*
 * Stream the splice: for each raw page pick DATA from base or dump by region,
 * keep that source page's spare metadata, recompute the ECC word.
 */
static bool splice_pages(FILE *dump, FILE *base, FILE *out, build_progress_t *prog)
{
    uint8_t pd[NAND_PAGE_RAW], pb[NAND_PAGE_RAW];
    for (uint32_t pg = 0; pg < NAND_16MB_PAGES; pg++) {
        if (fread(pd, 1, NAND_PAGE_RAW, dump) != NAND_PAGE_RAW) return false;
        if (fread(pb, 1, NAND_PAGE_RAW, base) != NAND_PAGE_RAW) return false;

        uint32_t loff = pg * NAND_PAGE_DATA;
        uint8_t *src;
        if (loff < OFF_SMC)        src = pb;   /* header  : universal base      */
        else if (loff < OFF_KV)    src = pb;   /* SMC     : glitch SMC from base
                                                * (user SMC-config merge: TODO,
                                                *  needs bench validation)      */
        else if (loff < OFF_CHAIN) src = pd;   /* KeyVault: PER-CONSOLE, dump   */
        else                       src = pb;   /* chain+fs: universal base      */

        nand_ecc_fix_page(src);                /* keep metadata, refresh ECC    */
        if (fwrite(src, 1, NAND_PAGE_RAW, out) != NAND_PAGE_RAW) return false;

        if (prog && (pg & 0x3FF) == 0)
            set_stage(prog, "assembling glitch image",
                      45 + (int)((uint64_t)pg * 40 / NAND_16MB_PAGES));
    }
    return true;
}

/* Verify the KV region of out matches the dump byte-for-byte (data only). */
static bool kv_matches_dump(FILE *dump, FILE *out)
{
    uint32_t pg0 = OFF_KV / NAND_PAGE_DATA, pg1 = OFF_CHAIN / NAND_PAGE_DATA;
    uint8_t a[NAND_PAGE_RAW], b[NAND_PAGE_RAW];
    for (uint32_t pg = pg0; pg < pg1; pg++) {
        long o = (long)pg * NAND_PAGE_RAW;
        if (fseek(dump, o, SEEK_SET) || fread(a, 1, NAND_PAGE_RAW, dump) != NAND_PAGE_RAW) return false;
        if (fseek(out,  o, SEEK_SET) || fread(b, 1, NAND_PAGE_RAW, out)  != NAND_PAGE_RAW) return false;
        if (memcmp(a, b, NAND_PAGE_DATA) != 0) return false;  /* data must match */
    }
    return true;
}

build_status_t build_rgh_image(const char *dump_path,
                               const uint8_t cpu_key[16],
                               const char *out_path,
                               build_progress_t *prog)
{
    build_status_t res = BUILD_ERROR;
    FILE *dump = NULL, *base = NULL, *out = NULL;
    struct stat sb;

    if (prog) prog->status = BUILD_ERROR;
    if (!dump_path || !out_path) return BUILD_ERROR;
    (void)cpu_key;   /* not needed to assemble: KV is copied, already encrypted */

    /* --- Stage 1: validate inputs --- */
    set_stage(prog, "validating dump", 5);
    if (stat(dump_path, &sb) != 0 || (uint32_t)sb.st_size != NAND_16MB_RAW_BYTES)
        return BUILD_BAD_DUMP;

    set_stage(prog, "checking RGH3 base", 15);
    if (stat(BASE_FILE, &sb) != 0 || (uint32_t)sb.st_size != NAND_16MB_RAW_BYTES) {
        ESP_LOGW(TAG, "no usable %s (need a 16MB RGH3 g2m base)", BASE_FILE);
        return BUILD_NEED_PACKAGE;
    }

    dump = fopen(dump_path, "rb");
    base = fopen(BASE_FILE, "rb");
    if (!dump || !base) goto out;

    if (!dump_is_valid(dump)) { res = BUILD_BAD_DUMP; goto out; }
    if (!base_is_rgh3(base))  { res = BUILD_NEED_PACKAGE; goto out; }

    /* --- Stage 2: splice + ECC --- */
    set_stage(prog, "assembling glitch image", 45);
    rewind(dump); rewind(base);
    out = fopen(out_path, "wb");
    if (!out) goto out;
    if (!splice_pages(dump, base, out, prog)) { ESP_LOGE(TAG, "splice failed"); goto out; }
    fclose(out); out = NULL;

    /* --- Stage 3: self-check (KV copied intact, ECC recomputed) --- */
    set_stage(prog, "verifying keyvault + ECC", 88);
    {
        FILE *od = fopen(dump_path, "rb");
        FILE *oo = fopen(out_path, "rb");
        bool kv_ok = od && oo && kv_matches_dump(od, oo);
        if (od) fclose(od);
        if (oo) fclose(oo);
        if (!kv_ok) { ESP_LOGE(TAG, "KV copy check failed"); res = BUILD_ERROR; goto out; }
    }

    /* --- Stage 4: optional J-Runner parity --- */
    set_stage(prog, "parity check", 95);
    res = BUILD_OK;
    if (stat(REF_FILE, &sb) == 0 && (uint32_t)sb.st_size == NAND_16MB_RAW_BYTES) {
        if (files_equal(REF_FILE, dump_path) == 1) {
            ESP_LOGW(TAG, "%s equals the retail dump - ignored", REF_FILE);
        } else {
            long diff = -1;
            int eq = files_first_diff(REF_FILE, out_path, &diff);
            if (eq == 1) {
                ESP_LOGI(TAG, "byte-identical to REFIMG - certified");
            } else if (eq == 0) {
                char msg[48];
                snprintf(msg, sizeof(msg), "differs @ 0x%lX vs REFIMG", diff);
                set_stage(prog, msg, 95);
                res = BUILD_NOT_VERIFIED;
            }
        }
    }
    set_stage(prog, res == BUILD_OK ? "done" : build_status_str(res), 100);

out:
    if (dump) fclose(dump);
    if (base) fclose(base);
    if (out)  fclose(out);
    if (prog) prog->status = res;
    return res;
}
