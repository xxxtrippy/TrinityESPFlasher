/*
 * build_image.c — native RGH3 (glitch2m) image builder for TrinityWeb360.
 *
 * GPL-3.0. See LICENSE.
 *
 * MODEL (checked against real xeBuild 1.21 "glitch2 mfg" + RGH3 images, see
 * RGH3_BUILD_MODEL.md and tools/test_build_image.c):
 *
 * Everything in a glitch2m/RGH3 image is the same for every console of the
 * same model and patch set, EXCEPT:
 *
 *   KeyVault            logical 0x4000..0x8000      encrypted with the CPU key
 *   config blocks       logical 0xF70000..0xF80000  SMC config (fan/thermal
 *                                                   calibration) + settings
 *   secdata.bin         flash filesystem            encrypted with the CPU key
 *   extended.bin        flash filesystem            encrypted with the CPU key
 *   crl.bin, dae.bin,   flash filesystem            per-console body
 *   fcrt.bin
 *   mobile blocks       FsBlockType 0x31..0x3F      dashboard caches
 *
 * So the builder takes a finished image built for ONE console (the "base",
 * /sd/RGH3BASE.BIN — e.g. the updflash.bin J-Runner made for your own Trinity
 * with your patch set) and re-targets it at the console whose NAND dump is
 * given:
 *
 *   - KeyVault and config blocks: raw pages copied from the dump.
 *   - The five per-console files: copied from the dump's own filesystem into
 *     the base's file slots (the base directory keeps its layout; only a size
 *     field changes if the dump's copy is shorter). They are already encrypted
 *     for that console.
 *   - secdata.bin / extended.bin missing from the dump: a fresh blank one is
 *     encrypted with the CPU key (this is what xeBuild does too).
 *   - crl.bin / dae.bin / fcrt.bin missing from the dump: the base's copy is
 *     left in place. It cannot be decrypted by the other console, which treats
 *     it as absent. NOT yet confirmed on a second console.
 *   - Mobile blocks: kept if the base was built for this same console,
 *     otherwise erased (the dashboard recreates them).
 *
 * Every page that is not touched is copied from the base byte for byte, spare
 * included. Touched pages keep the base's spare metadata and get a new ECC.
 *
 * The CPU key is optional. With it, the dump's KeyVault is authenticated
 * (HMAC) before anything is written, which catches a wrong key or a bad dump.
 *
 * Limits, checked and refused rather than guessed:
 *   - 16 MB small-block images only (Trinity).
 *   - No bad/remapped blocks in the regions that are read from the dump.
 *
 * Inputs / outputs on the SD card:
 *     dump_path          verified raw dump of the target console
 *     /sd/RGH3BASE.BIN   finished RGH3 image to re-target
 *     out_path           flashable raw image
 *     /sd/REFIMG.BIN     optional: if present the output must equal it
 */
#include "build_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"

#include "flashfs.h"
#include "nand_ecc.h"
#include "nand_geom.h"
#include "sdcard.h"
#include "xecrypt_bl.h"

#ifndef BUILD_BASE_FILE
#define BUILD_BASE_FILE SD_MOUNT_POINT "/RGH3BASE.BIN"
#endif
#ifndef BUILD_REF_FILE
#define BUILD_REF_FILE  SD_MOUNT_POINT "/REFIMG.BIN"
#endif

/* Per-console regions, logical byte offsets. */
#define OFF_KV      0x4000u
#define KV_LEN      0x4000u
#define OFF_CHAIN   0x8000u
#define OFF_CONFIG  0xF70000u
#define CONFIG_LEN  0x10000u
#define OFF_SMCCFG  0xF7C000u          /* the SMC config block inside the config area */

static const char *TAG = "build";

const char *build_status_str(build_status_t s)
{
    switch (s) {
        case BUILD_OK:            return "Verified";
        case BUILD_NEED_CPU_KEY:  return "CPU key needed";
        case BUILD_NEED_PACKAGE:  return "Base img missing";
        case BUILD_BAD_DUMP:      return "Bad dump";
        case BUILD_BAD_KEY:       return "Key/KV mismatch";
        case BUILD_BAD_BLOCKS:    return "Bad blocks: use PC";
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

/* ------------------------------------------------------------------ random */

static build_random_fn s_random;

void build_set_random(build_random_fn fn) { s_random = fn; }

#ifdef ESP_PLATFORM
#include "esp_random.h"
static void default_random(uint8_t *out, size_t n) { esp_fill_random(out, n); }
#else
static void default_random(uint8_t *out, size_t n) { for (size_t i = 0; i < n; i++) out[i] = (uint8_t)rand(); }
#endif

static void get_random(uint8_t *out, size_t n) { (s_random ? s_random : default_random)(out, n); }

/* ------------------------------------------------- CPU-key "confounder" files
 *
 * KeyVault, extended.bin and secdata.bin share one format:
 *     [0x00..0x10)  HMAC-SHA1(cpukey, plaintext[0x10..) + suffix)[:16]
 *     [0x10..end)   RC4(key = HMAC-SHA1(cpukey, bytes[0x00..0x10))[:16])
 *                   plaintext starts with 8 random bytes
 * suffix is the two bytes 07 12 for the KeyVault and extended.bin, and empty
 * for secdata.bin. Checked against real files and their CPU key.
 */
static const uint8_t SUFFIX_0712[2] = { 0x07, 0x12 };

static void cc_header(const uint8_t cpu_key[16], const uint8_t *plain_tail, size_t tail_len,
                      bool suffix, uint8_t *scratch /* tail_len + 2 */, uint8_t hdr[16])
{
    uint8_t h[20];
    memcpy(scratch, plain_tail, tail_len);
    if (suffix) memcpy(scratch + tail_len, SUFFIX_0712, 2);
    xc_hmac_sha1(cpu_key, 16, scratch, tail_len + (suffix ? 2 : 0), h);
    memcpy(hdr, h, 16);
}

static void cc_crypt(const uint8_t cpu_key[16], const uint8_t hdr[16], uint8_t *body, size_t n)
{
    uint8_t k[20];
    xc_hmac_sha1(cpu_key, 16, hdr, 16, k);
    xc_rc4(k, 16, body, n);
}

/* Decrypt a copy and check the HMAC. buf is left untouched. */
bool build_cc_verify(const uint8_t cpu_key[16], const uint8_t *buf, size_t len, bool suffix)
{
    if (len < 0x20) return false;
    uint8_t *tmp = malloc(len + 2);
    uint8_t *scr = malloc(len + 2);
    bool ok = false;
    if (tmp && scr) {
        uint8_t hdr[16];
        memcpy(tmp, buf + 0x10, len - 0x10);
        cc_crypt(cpu_key, buf, tmp, len - 0x10);
        cc_header(cpu_key, tmp, len - 0x10, suffix, scr, hdr);
        ok = memcmp(hdr, buf, 16) == 0;
    }
    free(tmp); free(scr);
    return ok;
}

/* Make a fresh blank file of `len` bytes: 8 random bytes then zeros, encrypted. */
bool build_cc_make_blank(const uint8_t cpu_key[16], uint8_t *out, size_t len, bool suffix)
{
    if (len < 0x20) return false;
    uint8_t *scr = malloc(len + 2);
    if (!scr) return false;
    memset(out, 0, len);
    get_random(out + 0x10, 8);
    cc_header(cpu_key, out + 0x10, len - 0x10, suffix, scr, out);
    cc_crypt(cpu_key, out, out + 0x10, len - 0x10);
    free(scr);
    return true;
}

/* ---------------------------------------------------------------- overlays
 * Small pieces of data that replace base data at a logical offset. */
#define MAX_OVERLAYS 12

typedef struct {
    uint32_t off, len;          /* logical byte range            */
    uint8_t *data;              /* malloc'd, owned by the builder */
} overlay_t;

typedef struct {
    overlay_t ov[MAX_OVERLAYS];
    int n;
} overlays_t;

static bool ov_add(overlays_t *o, uint32_t off, uint32_t len, uint8_t *data)
{
    if (o->n >= MAX_OVERLAYS) { free(data); return false; }
    o->ov[o->n].off = off; o->ov[o->n].len = len; o->ov[o->n].data = data;
    o->n++;
    return true;
}

static void ov_free(overlays_t *o)
{
    for (int i = 0; i < o->n; i++) free(o->ov[i].data);
    o->n = 0;
}

/* Apply every overlay that intersects this page. Returns true if it changed. */
static bool ov_apply(const overlays_t *o, uint32_t page_off, uint8_t *page)
{
    bool changed = false;
    for (int i = 0; i < o->n; i++) {
        const overlay_t *v = &o->ov[i];
        uint32_t a = v->off > page_off ? v->off : page_off;
        uint32_t b = (v->off + v->len) < (page_off + NAND_PAGE_DATA) ? (v->off + v->len) : (page_off + NAND_PAGE_DATA);
        if (a >= b) continue;
        if (memcmp(page + (a - page_off), v->data + (a - v->off), b - a) != 0) {
            memcpy(page + (a - page_off), v->data + (a - v->off), b - a);
            changed = true;
        }
    }
    return changed;
}

/* ----------------------------------------------------------------- checks */

static bool header_ok(FILE *f)
{
    uint8_t pg[NAND_PAGE_RAW];
    if (!ffs_read_page(f, 0, pg)) return false;
    if (pg[0] != 0xFF || pg[1] != 0x4F) return false;
    /* KeyVault offset in the header must be where we expect it */
    uint32_t kvoff = ((uint32_t)pg[0x6C] << 24) | ((uint32_t)pg[0x6D] << 16) | ((uint32_t)pg[0x6E] << 8) | pg[0x6F];
    return kvoff == OFF_KV;
}

/* The base must be an RGH3 image: chain at 0x8000 starting with CB_A 10918. */
static bool base_is_rgh3(FILE *base)
{
    uint8_t pg[NAND_PAGE_RAW];
    if (!ffs_read_page(base, 0, pg)) return false;
    uint32_t loff = ((uint32_t)pg[8] << 24) | ((uint32_t)pg[9] << 16) | ((uint32_t)pg[10] << 8) | pg[11];
    if (loff != OFF_CHAIN) return false;
    if (!ffs_read_page(base, OFF_CHAIN / NAND_PAGE_DATA, pg)) return false;
    uint16_t ver = (uint16_t)((pg[2] << 8) | pg[3]);
    if (pg[0] != 'C' || pg[1] != 'B' || ver != 10918) {
        ESP_LOGW(TAG, "base first loader %c%c %u (want CB 10918) - not an RGH3 image", pg[0], pg[1], ver);
        return false;
    }
    return true;
}

/* Blocks [b0, b1) of a raw image must be un-remapped and not marked bad.
 * Erased blocks are fine. */
static bool blocks_plain(FILE *f, uint32_t b0, uint32_t b1)
{
    uint8_t pg[NAND_PAGE_RAW];
    for (uint32_t b = b0; b < b1; b++) {
        if (!ffs_read_page(f, b * FFS_PAGES_PER_BLK, pg)) return false;
        if (nand_page_is_blank(pg)) continue;
        if (pg[0x205] != 0xFF || ffs_spare_block_id(pg) != b) {
            ESP_LOGW(TAG, "block 0x%03lX is bad or remapped (spare id 0x%03lX)",
                     (unsigned long)b, (unsigned long)ffs_spare_block_id(pg));
            return false;
        }
    }
    return true;
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

/* ------------------------------------------------------ per-console files */

typedef struct {
    const char *name;
    bool cpukey_format;     /* KeyVault-style file we can regenerate           */
    bool suffix;            /* HMAC suffix 07 12                               */
} pc_file_t;

static const pc_file_t PC_FILES[] = {
    { "secdata.bin",  true,  false },
    { "extended.bin", true,  true  },
    { "crl.bin",      false, false },
    { "dae.bin",      false, false },
    { "fcrt.bin",     false, false },
};
#define N_PC_FILES (sizeof(PC_FILES) / sizeof(PC_FILES[0]))

/*
 * Queue the overlays for one per-console file. Source, in order:
 *   1. the dump's own copy, if it fits the base's slot;
 *   2. a fresh blank encrypted with the CPU key (secdata/extended only);
 *   3. nothing — the base's copy stays.
 */
static bool add_file(overlays_t *ovl, FILE *dump, const ffs_t *dfs, bool have_dfs,
                     const ffs_t *bfs, const pc_file_t *pf, const uint8_t *cpu_key,
                     build_report_t *rep, int idx)
{
    ffs_file_t bf, df;
    rep->file_src[idx] = BUILD_SRC_BASE;
    if (!ffs_find(bfs, pf->name, &bf)) return true;          /* base has no such file */

    uint32_t cap = (uint32_t)bf.nchain * FFS_BLOCK_BYTES;
    uint8_t *buf = NULL;
    uint32_t size = 0;

    if (have_dfs && ffs_find(dfs, pf->name, &df) && df.size > 0 && df.size <= cap &&
        (uint32_t)df.nchain <= (uint32_t)bf.nchain) {
        buf = malloc((size_t)df.nchain * FFS_BLOCK_BYTES);
        if (!buf) return false;
        if (ffs_read_file(dump, &df, buf)) {
            size = df.size;
            /* a KeyVault-style file must decrypt with the key we were given */
            if (pf->cpukey_format && cpu_key && !build_cc_verify(cpu_key, buf, size, pf->suffix)) {
                ESP_LOGW(TAG, "%s in the dump does not verify with this CPU key - regenerating", pf->name);
                free(buf); buf = NULL;
            } else {
                rep->file_src[idx] = BUILD_SRC_DUMP;
            }
        } else {
            free(buf); buf = NULL;                            /* remapped block etc. */
        }
    }

    if (!buf && pf->cpukey_format && cpu_key) {
        size = bf.size;
        buf = malloc(size);
        if (!buf) return false;
        if (!build_cc_make_blank(cpu_key, buf, size, pf->suffix)) { free(buf); return false; }
        rep->file_src[idx] = BUILD_SRC_FRESH;
    }

    if (!buf) {
        ESP_LOGW(TAG, "%s: not in the dump, keeping the base's copy", pf->name);
        return true;
    }

    /* data, one overlay per block of the base's chain (zero padded) */
    for (int k = 0; k < bf.nchain; k++) {
        uint32_t o = (uint32_t)k * FFS_BLOCK_BYTES;
        if (o >= size) break;
        uint32_t n = size - o < FFS_BLOCK_BYTES ? size - o : FFS_BLOCK_BYTES;
        uint32_t padded = (n + NAND_PAGE_DATA - 1) & ~(NAND_PAGE_DATA - 1);
        uint8_t *blk = calloc(1, padded);
        if (!blk) { free(buf); return false; }
        memcpy(blk, buf + o, n);
        if (!ov_add(ovl, bf.chain[k] * FFS_BLOCK_BYTES, padded, blk)) { free(buf); return false; }
    }
    free(buf);

    /* size field in the base directory */
    if (size != bf.size) {
        uint8_t *sz = malloc(4);
        if (!sz) return false;
        sz[0] = (uint8_t)(size >> 24); sz[1] = (uint8_t)(size >> 16); sz[2] = (uint8_t)(size >> 8); sz[3] = (uint8_t)size;
        if (!ov_add(ovl, ffs_entry_size_offset(bfs, bf.entry_index), 4, sz)) return false;
    }
    return true;
}

/* --------------------------------------------------------------- assemble */

typedef struct {
    bool same_console;      /* base was built for the dump's console */
    bool copy_config;       /* take config blocks from the dump      */
} plan_t;

static bool assemble(FILE *dump, FILE *base, FILE *out, const overlays_t *ovl,
                     const plan_t *plan, build_progress_t *prog, build_report_t *rep)
{
    uint8_t pb[NAND_PAGE_RAW], pd[NAND_PAGE_RAW];
    bool erase_block = false;

    if (fseek(base, 0, SEEK_SET) != 0) return false;
    for (uint32_t pg = 0; pg < NAND_16MB_PAGES; pg++) {
        if (fread(pb, 1, NAND_PAGE_RAW, base) != NAND_PAGE_RAW) return false;
        uint32_t loff = pg * NAND_PAGE_DATA;
        const uint8_t *src = pb;

        /* mobile-data blocks built for another console: erase the block */
        if ((pg % FFS_PAGES_PER_BLK) == 0) {
            uint32_t t = nand_page_is_blank(pb) ? 0 : ffs_spare_type(pb);
            erase_block = !plan->same_console && t > FFS_TYPE_ROOT;
            if (erase_block) rep->mobile_erased++;
        }

        bool from_dump = (loff >= OFF_KV && loff < OFF_KV + KV_LEN) ||
                         (plan->copy_config && loff >= OFF_CONFIG && loff < OFF_CONFIG + CONFIG_LEN);
        if (from_dump) {
            if (!ffs_read_page(dump, pg, pd)) return false;
            if (!nand_page_is_blank(pd)) {
                if (!nand_ecc_page_ok(pd)) rep->dump_ecc_fixed++;
                nand_ecc_fix_page(pd);                 /* dump pages get a clean ECC */
            }
            src = pd;
        } else if (erase_block) {
            memset(pb, 0xFF, NAND_PAGE_RAW);
        } else if (ov_apply(ovl, loff, pb)) {
            /* an erased page that now holds data needs real metadata */
            bool spare_blank = true;
            for (uint32_t i = NAND_PAGE_DATA; i < NAND_PAGE_RAW; i++) if (pb[i] != 0xFF) { spare_blank = false; break; }
            if (spare_blank) nand_ecc_make_spare(pb, pg, pb + NAND_PAGE_DATA);
            else             nand_ecc_fix_page(pb);
            rep->pages_patched++;
        }

        if (fwrite(src, 1, NAND_PAGE_RAW, out) != NAND_PAGE_RAW) return false;

        if (prog && (pg & 0x3FF) == 0)
            set_stage(prog, "assembling image", 30 + (int)((uint64_t)pg * 50 / NAND_16MB_PAGES));
    }
    return true;
}

/* ------------------------------------------------------------- self-check */

static bool self_check(const char *dump_path, const char *out_path, const uint8_t *cpu_key,
                       const plan_t *plan, build_report_t *rep)
{
    FILE *od = fopen(dump_path, "rb"), *oo = fopen(out_path, "rb");
    uint8_t a[NAND_PAGE_RAW], b[NAND_PAGE_RAW];
    bool ok = od && oo;
    (void)plan;

    /* 1. every written page has a consistent ECC */
    for (uint32_t pg = 0; ok && pg < NAND_16MB_PAGES; pg++) {
        if (fread(b, 1, NAND_PAGE_RAW, oo) != NAND_PAGE_RAW) { ok = false; break; }
        if (!nand_ecc_page_ok(b)) {
            ESP_LOGE(TAG, "self-check: bad ECC on page 0x%lX", (unsigned long)pg);
            ok = false;
        }
    }

    /* 2. the KeyVault is the dump's, bit for bit */
    for (uint32_t pg = OFF_KV / NAND_PAGE_DATA; ok && pg < (OFF_KV + KV_LEN) / NAND_PAGE_DATA; pg++) {
        if (!ffs_read_page(od, pg, a) || !ffs_read_page(oo, pg, b) || memcmp(a, b, NAND_PAGE_DATA) != 0) {
            ESP_LOGE(TAG, "self-check: KeyVault page 0x%lX differs from the dump", (unsigned long)pg);
            ok = false;
        }
    }

    /* 3. the filesystem still opens and the CPU-key files verify */
    if (ok) {
        ffs_t *fs = malloc(sizeof(*fs));
        ok = fs && ffs_open(oo, fs);
        if (!ok) ESP_LOGE(TAG, "self-check: output filesystem does not open");
        for (size_t i = 0; ok && cpu_key && i < N_PC_FILES; i++) {
            ffs_file_t f;
            if (!PC_FILES[i].cpukey_format || rep->file_src[i] == BUILD_SRC_BASE) continue;
            if (!ffs_find(fs, PC_FILES[i].name, &f)) { ok = false; break; }
            uint8_t *buf = malloc((size_t)f.nchain * FFS_BLOCK_BYTES);
            ok = buf && ffs_read_file(oo, &f, buf) && build_cc_verify(cpu_key, buf, f.size, PC_FILES[i].suffix);
            if (!ok) ESP_LOGE(TAG, "self-check: %s does not verify", PC_FILES[i].name);
            free(buf);
        }
        free(fs);
    }

    if (od) fclose(od);
    if (oo) fclose(oo);
    return ok;
}

/* ------------------------------------------------------------------- main */

build_status_t build_rgh_image_ex(const char *dump_path, const uint8_t cpu_key[16],
                                  const char *out_path, build_progress_t *prog,
                                  build_report_t *rep_out)
{
    build_status_t res = BUILD_ERROR;
    FILE *dump = NULL, *base = NULL, *out = NULL;
    ffs_t *dfs = NULL, *bfs = NULL;
    uint8_t *kv = NULL, *bkv = NULL;
    overlays_t ovl = { .n = 0 };
    build_report_t rep;
    plan_t plan = { false, false };
    struct stat sb;

    memset(&rep, 0, sizeof(rep));
    if (prog) prog->status = BUILD_ERROR;
    if (!dump_path || !out_path) return BUILD_ERROR;

    /* --- 1. inputs --- */
    set_stage(prog, "checking dump", 3);
    if (stat(dump_path, &sb) != 0 || (uint32_t)sb.st_size != NAND_16MB_RAW_BYTES) { res = BUILD_BAD_DUMP; goto out; }

    set_stage(prog, "checking RGH3 base", 6);
    if (stat(BUILD_BASE_FILE, &sb) != 0 || (uint32_t)sb.st_size != NAND_16MB_RAW_BYTES) {
        ESP_LOGW(TAG, "no usable %s (need a 16MB raw RGH3 image)", BUILD_BASE_FILE);
        res = BUILD_NEED_PACKAGE; goto out;
    }

    dump = fopen(dump_path, "rb");
    base = fopen(BUILD_BASE_FILE, "rb");
    if (!dump || !base) goto out;

    if (!header_ok(dump)) { res = BUILD_BAD_DUMP; goto out; }
    if (!header_ok(base) || !base_is_rgh3(base)) { res = BUILD_NEED_PACKAGE; goto out; }

    /* regions we copy raw from the dump must not be remapped */
    if (!blocks_plain(dump, OFF_KV / FFS_BLOCK_BYTES, (OFF_KV + KV_LEN) / FFS_BLOCK_BYTES) ||
        !blocks_plain(dump, OFF_CONFIG / FFS_BLOCK_BYTES, (OFF_CONFIG + CONFIG_LEN) / FFS_BLOCK_BYTES)) {
        res = BUILD_BAD_BLOCKS; goto out;
    }

    /* --- 2. KeyVault --- */
    set_stage(prog, "checking keyvault", 10);
    kv = malloc(KV_LEN); bkv = malloc(KV_LEN);
    if (!kv || !bkv) goto out;
    if (!ffs_read_logical(dump, OFF_KV, kv, KV_LEN) || !ffs_read_logical(base, OFF_KV, bkv, KV_LEN)) goto out;
    {
        bool blank = true;
        for (uint32_t i = 0; i < 0x200; i++) if (kv[i] != 0xFF && kv[i] != 0x00) { blank = false; break; }
        if (blank) { ESP_LOGE(TAG, "dump has no KeyVault"); res = BUILD_BAD_DUMP; goto out; }
    }
    if (cpu_key) {
        if (!build_cc_verify(cpu_key, kv, KV_LEN, true)) {
            ESP_LOGE(TAG, "the dump's KeyVault does not verify with this CPU key");
            res = BUILD_BAD_KEY; goto out;
        }
        rep.kv_verified = true;
        plan.same_console = build_cc_verify(cpu_key, bkv, KV_LEN, true);
    } else {
        ESP_LOGW(TAG, "no CPU key: KeyVault copied without authentication");
        plan.same_console = memcmp(kv, bkv, KV_LEN) == 0;
    }
    rep.same_console = plan.same_console;

    /* --- 3. config blocks: only if the dump actually has an SMC config --- */
    {
        uint8_t pg[NAND_PAGE_RAW];
        plan.copy_config = ffs_read_page(dump, OFF_SMCCFG / NAND_PAGE_DATA, pg) && !nand_page_is_blank(pg);
        rep.config_from_dump = plan.copy_config;
        if (!plan.copy_config) ESP_LOGW(TAG, "dump has no SMC config block - keeping the base's");
    }

    /* --- 4. per-console files --- */
    set_stage(prog, "reading filesystems", 16);
    dfs = malloc(sizeof(*dfs)); bfs = malloc(sizeof(*bfs));
    if (!dfs || !bfs) goto out;
    if (!ffs_open(base, bfs)) { ESP_LOGE(TAG, "base has no filesystem"); res = BUILD_NEED_PACKAGE; goto out; }
    bool have_dfs = ffs_open(dump, dfs);
    rep.dump_fs = have_dfs;
    if (!have_dfs) ESP_LOGW(TAG, "dump has no readable filesystem");

    for (size_t i = 0; i < N_PC_FILES; i++) {
        if (!add_file(&ovl, dump, dfs, have_dfs, bfs, &PC_FILES[i], cpu_key, &rep, (int)i)) goto out;
    }
    /* A different console with files we could neither copy nor regenerate
     * needs the CPU key to finish. */
    if (!plan.same_console && !cpu_key) {
        for (size_t i = 0; i < N_PC_FILES; i++) {
            if (PC_FILES[i].cpukey_format && rep.file_src[i] == BUILD_SRC_BASE) {
                ESP_LOGE(TAG, "%s must be regenerated: CPU key required", PC_FILES[i].name);
                res = BUILD_NEED_CPU_KEY; goto out;
            }
        }
    }

    /* --- 5. assemble --- */
    set_stage(prog, "assembling image", 30);
    out = fopen(out_path, "wb");
    if (!out) goto out;
    if (!assemble(dump, base, out, &ovl, &plan, prog, &rep)) { ESP_LOGE(TAG, "assemble failed"); goto out; }
    if (fclose(out) != 0) { out = NULL; goto out; }
    out = NULL;

    /* --- 6. self-check --- */
    set_stage(prog, "self-check", 85);
    if (!self_check(dump_path, out_path, cpu_key, &plan, &rep)) { res = BUILD_ERROR; goto out; }

    /* --- 7. optional reference parity --- */
    res = BUILD_OK;
    if (stat(BUILD_REF_FILE, &sb) == 0 && (uint32_t)sb.st_size == NAND_16MB_RAW_BYTES) {
        long diff = -1;
        set_stage(prog, "parity check", 95);
        int eq = files_first_diff(BUILD_REF_FILE, out_path, &diff);
        if (eq == 1) {
            ESP_LOGI(TAG, "byte-identical to REFIMG");
            rep.ref_matched = true;
        } else {
            char msg[48];
            snprintf(msg, sizeof(msg), "differs @ 0x%lX vs REFIMG", diff);
            set_stage(prog, msg, 95);
            res = BUILD_NOT_VERIFIED;
        }
    }
    set_stage(prog, res == BUILD_OK ? "done" : build_status_str(res), 100);

out:
    if (dump) fclose(dump);
    if (base) fclose(base);
    if (out)  fclose(out);
    if (res != BUILD_OK && res != BUILD_NOT_VERIFIED && out_path) remove(out_path);
    ov_free(&ovl);
    free(dfs); free(bfs); free(kv); free(bkv);
    if (rep_out) *rep_out = rep;
    if (prog) prog->status = res;
    return res;
}

build_status_t build_rgh_image(const char *dump_path, const uint8_t cpu_key[16],
                               const char *out_path, build_progress_t *prog)
{
    return build_rgh_image_ex(dump_path, cpu_key, out_path, prog, NULL);
}
