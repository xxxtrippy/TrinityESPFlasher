/*
 * Host test for the firmware's image builder (main/build_image.c, flashfs.c,
 * nand_ecc.c) against REAL images. It compiles the same C files the ESP runs.
 *
 * You need, for one Trinity console:
 *   base.bin   an RGH3 image J-Runner built for it (updflash.bin)
 *   dump.bin   a raw NAND read of the same console (nanddump1.bin). It may be
 *              a read-back of a different build; only the console must match.
 *   its CPU key (32 hex digits)
 *
 * Build and run (from tools/):
 *   gcc -O2 -Ihost_shim -I../main -DBUILD_BASE_FILE='"base.bin"' \
 *       -DBUILD_REF_FILE='"ref.bin"' test_build_image.c ../main/build_image.c \
 *       ../main/flashfs.c ../main/nand_ecc.c ../main/xecrypt_bl.c -o t_build
 *   mkdir run && cd run
 *   ../t_build /path/updflash.bin /path/nanddump1.bin <cpukey hex>
 *
 * It writes scratch files in the current directory. Nothing here is uploaded
 * anywhere; keep your images and key private.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "build_image.h"
#include "flashfs.h"
#include "nand_ecc.h"
#include "nand_geom.h"
#include "xecrypt_bl.h"

static int fails;
#define CHECK(c, ...) do { if (c) { printf("  ok    "); } else { printf("  FAIL  "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static uint8_t *load(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { perror(p); exit(2); }
    fclose(f); *n = (size_t)sz;
    return b;
}

static void save(const char *p, const uint8_t *b, size_t n)
{
    FILE *f = fopen(p, "wb");
    if (!f || fwrite(b, 1, n, f) != n) { perror(p); exit(2); }
    fclose(f);
}

static int ecc_bad(const uint8_t *img)
{
    int bad = 0;
    for (uint32_t p = 0; p < NAND_16MB_PAGES; p++)
        if (!nand_ecc_page_ok(img + (size_t)p * NAND_PAGE_RAW)) bad++;
    return bad;
}

/* decrypt a KeyVault-format buffer in place (after the 16-byte header) */
static void cc_dec(const uint8_t key[16], uint8_t *buf, size_t len)
{
    uint8_t k[20];
    xc_hmac_sha1(key, 16, buf, 16, k);
    xc_rc4(k, 16, buf + 0x10, len - 0x10);
}

static void logical(const uint8_t *raw, uint32_t off, uint8_t *dst, uint32_t len)
{
    for (uint32_t d = 0; d < len; d += NAND_PAGE_DATA)
        memcpy(dst + d, raw + (size_t)((off + d) / NAND_PAGE_DATA) * NAND_PAGE_RAW, NAND_PAGE_DATA);
}

/* file block list of the base, for the "only allowed pages changed" check */
static bool page_allowed(const ffs_t *bfs, uint32_t loff)
{
    static const char *names[] = { "secdata.bin", "extended.bin", "crl.bin", "dae.bin", "fcrt.bin" };
    if (loff >= 0x4000 && loff < 0x8000) return true;                 /* KeyVault */
    if (loff >= 0xF70000 && loff < 0xF80000) return true;             /* config   */
    if (loff / FFS_BLOCK_BYTES == bfs->root_block) return true;       /* size fields */
    for (int i = 0; i < 5; i++) {
        ffs_file_t f;
        if (!ffs_find(bfs, names[i], &f)) continue;
        for (int k = 0; k < f.nchain; k++) if (loff / FFS_BLOCK_BYTES == f.chain[k]) return true;
    }
    return false;
}

static uint8_t fixed_rand[2][8];
static int rand_calls;
static void test_random(uint8_t *out, size_t n)
{
    memcpy(out, fixed_rand[rand_calls & 1], n < 8 ? n : 8);
    rand_calls++;
}

int main(int argc, char **argv)
{
    if (argc != 4 || strlen(argv[3]) != 32) {
        fprintf(stderr, "usage: %s updflash.bin nanddump.bin <32 hex cpu key>\n", argv[0]);
        return 2;
    }
    uint8_t key[16], wrong[16];
    for (int i = 0; i < 16; i++) { unsigned v; sscanf(argv[3] + i * 2, "%2x", &v); key[i] = (uint8_t)v; wrong[i] = (uint8_t)~v; }

    size_t nb, nd;
    uint8_t *base = load(argv[1], &nb), *dump = load(argv[2], &nd);
    if (nb != NAND_16MB_RAW_BYTES || nd != NAND_16MB_RAW_BYTES) { fprintf(stderr, "need 16 MB raw images (17301504 bytes)\n"); return 2; }
    save("base.bin", base, nb);
    remove("ref.bin");

    build_report_t rep;
    build_status_t st;
    size_t no;
    uint8_t *out;
    FILE *fb = fopen("base.bin", "rb");
    ffs_t *bfs = malloc(sizeof(*bfs));
    if (!fb || !ffs_open(fb, bfs)) { fprintf(stderr, "base has no filesystem\n"); return 2; }

    printf("1. ECC on real images\n");
    CHECK(ecc_bad(base) == 0, "base: all 32768 pages reproduce their stored ECC (%d bad)", ecc_bad(base));
    CHECK(ecc_bad(dump) == 0, "dump: all 32768 pages reproduce their stored ECC (%d bad)", ecc_bad(dump));

    printf("2. Rebuild the base from itself (dump = base)\n");
    st = build_rgh_image_ex(argv[1], key, "out2.bin", NULL, &rep);
    CHECK(st == BUILD_OK, "status %s", build_status_str(st));
    out = load("out2.bin", &no);
    CHECK(no == nb && memcmp(out, base, nb) == 0, "output is byte-identical to the J-Runner image");
    CHECK(rep.kv_verified && rep.same_console, "KeyVault authenticated, same console detected");
    free(out);

    printf("3. Re-target the base at the console's NAND read-back (with CPU key)\n");
    st = build_rgh_image_ex(argv[2], key, "out3.bin", NULL, &rep);
    CHECK(st == BUILD_OK, "status %s", build_status_str(st));
    out = load("out3.bin", &no);
    CHECK(ecc_bad(out) == 0, "every output page has a valid ECC");
    {
        int stray = 0, changed = 0;
        for (uint32_t p = 0; p < NAND_16MB_PAGES; p++) {
            if (memcmp(out + (size_t)p * NAND_PAGE_RAW, base + (size_t)p * NAND_PAGE_RAW, NAND_PAGE_RAW) == 0) continue;
            changed++;
            if (!page_allowed(bfs, p * NAND_PAGE_DATA)) { if (stray < 5) printf("        stray change at logical 0x%X\n", p * NAND_PAGE_DATA); stray++; }
        }
        CHECK(stray == 0, "%d pages differ from the base, all inside per-console regions", changed);
        uint8_t a[0x4000], b[0x4000];
        logical(out, 0x4000, a, 0x4000); logical(dump, 0x4000, b, 0x4000);
        CHECK(memcmp(a, b, 0x4000) == 0, "KeyVault equals the dump's");
        logical(base, 0x4000, b, 0x4000);
        cc_dec(key, a, 0x4000); cc_dec(key, b, 0x4000);
        CHECK(memcmp(a + 0x18, b + 0x18, 0x4000 - 0x18) == 0, "decrypted KeyVault equals the J-Runner image's KeyVault");
        logical(out, 0xF7C000, a, 0x4000); logical(dump, 0xF7C000, b, 0x4000);
        CHECK(memcmp(a, b, 0x4000) == 0, "SMC config block equals the dump's");
        printf("        files: secdata %d extended %d crl %d dae %d fcrt %d (0 base, 1 dump, 2 fresh); mobile blocks erased %u\n",
               rep.file_src[0], rep.file_src[1], rep.file_src[2], rep.file_src[3], rep.file_src[4], (unsigned)rep.mobile_erased);
        CHECK(rep.same_console && rep.mobile_erased == 0, "same console: mobile blocks kept");
    }
    free(out);

    printf("4. Same, without a CPU key\n");
    st = build_rgh_image_ex(argv[2], NULL, "out4.bin", NULL, &rep);
    CHECK(st == BUILD_OK, "status %s", build_status_str(st));
    if (st == BUILD_OK) {
        out = load("out4.bin", &no);
        CHECK(ecc_bad(out) == 0, "every output page has a valid ECC");
        bool kv_same_bytes = memcmp(dump + (size_t)0x20 * NAND_PAGE_RAW, base + (size_t)0x20 * NAND_PAGE_RAW, NAND_PAGE_DATA) == 0;
        CHECK(rep.same_console == kv_same_bytes, "console match falls back to a KeyVault byte compare (%s)", rep.same_console ? "same" : "treated as different");
        if (!rep.same_console) CHECK(rep.mobile_erased > 0, "treated as another console: %u mobile blocks erased", (unsigned)rep.mobile_erased);
        free(out);
    }

    printf("5. Wrong CPU key is refused\n");
    st = build_rgh_image_ex(argv[2], wrong, "out5.bin", NULL, &rep);
    CHECK(st == BUILD_BAD_KEY, "status %s", build_status_str(st));
    { FILE *f = fopen("out5.bin", "rb"); CHECK(f == NULL, "no image left behind"); if (f) fclose(f); }

    printf("6. Dump without a filesystem: fresh secdata/extended from the CPU key\n");
    {
        /* wipe every filesystem root in a copy of the dump */
        uint8_t *d6 = malloc(nd); memcpy(d6, dump, nd);
        int wiped = 0;
        for (uint32_t b = 0; b < FFS_BLOCKS; b++) {
            uint8_t *pg = d6 + (size_t)b * FFS_PAGES_PER_BLK * NAND_PAGE_RAW;
            if (!nand_page_is_blank(pg) && ffs_spare_type(pg) == FFS_TYPE_ROOT) { memset(pg, 0xFF, (size_t)FFS_PAGES_PER_BLK * NAND_PAGE_RAW); wiped++; }
        }
        save("dump6.bin", d6, nd); free(d6);
        printf("        (%d root blocks wiped)\n", wiped);

        /* make the fresh extended.bin use the same 8 random bytes xeBuild used
         * in the base, so the result can be compared byte for byte */
        ffs_file_t f; uint8_t ext[0x4000];
        bool have = ffs_find(bfs, "extended.bin", &f) && f.size == 0x4000 && ffs_read_file(fb, &f, ext);
        uint8_t ext_enc[0x4000]; memcpy(ext_enc, ext, sizeof(ext));
        cc_dec(key, ext, 0x4000);
        bool ext_blank = have;
        for (int i = 0x18; i < 0x4000 && ext_blank; i++) if (ext[i]) ext_blank = false;
        memset(fixed_rand[0], 0x5A, 8); memcpy(fixed_rand[1], ext + 0x10, 8);
        rand_calls = 0; build_set_random(test_random);
        st = build_rgh_image_ex("dump6.bin", key, "out6.bin", NULL, &rep);
        build_set_random(NULL);
        CHECK(st == BUILD_OK, "status %s", build_status_str(st));
        CHECK(rep.file_src[0] == BUILD_SRC_FRESH && rep.file_src[1] == BUILD_SRC_FRESH, "secdata.bin and extended.bin regenerated");
        if (st == BUILD_OK) {
            out = load("out6.bin", &no);
            CHECK(ecc_bad(out) == 0, "every output page has a valid ECC");
            if (ext_blank) {
                uint8_t got[0x4000]; logical(out, f.chain[0] * FFS_BLOCK_BYTES, got, 0x4000);
                CHECK(memcmp(got, ext_enc, 0x4000) == 0, "fresh extended.bin is byte-identical to the one xeBuild wrote (same random bytes)");
            } else {
                printf("        (base extended.bin is not a blank one; exact compare skipped)\n");
            }
            FILE *fo = fopen("out6.bin", "rb"); ffs_t *ofs = malloc(sizeof(*ofs)); ffs_file_t sf; uint8_t sec[0x4000];
            bool sok = fo && ffs_open(fo, ofs) && ffs_find(ofs, "secdata.bin", &sf) && ffs_read_file(fo, &sf, sec) && build_cc_verify(key, sec, sf.size, false);
            CHECK(sok, "fresh secdata.bin authenticates with the CPU key");
            if (fo) fclose(fo);
            free(ofs); free(out);
        }

        st = build_rgh_image_ex("dump6.bin", NULL, "out6b.bin", NULL, &rep);
        if (rep.same_console) printf("        (dump KeyVault bytes equal the base's; 'needs key' case not reachable with these files)\n");
        else CHECK(st == BUILD_NEED_CPU_KEY, "other console + no filesystem + no key -> %s", build_status_str(st));
    }

    printf("7. A remapped KeyVault block is refused\n");
    {
        uint8_t *d7 = malloc(nd); memcpy(d7, dump, nd);
        uint8_t *pg = d7 + (size_t)(0x4000 / NAND_PAGE_DATA) * NAND_PAGE_RAW;
        pg[0x201] = 0x77; nand_ecc_fix_page(pg);
        save("dump7.bin", d7, nd); free(d7);
        st = build_rgh_image_ex("dump7.bin", key, "out7.bin", NULL, &rep);
        CHECK(st == BUILD_BAD_BLOCKS, "status %s", build_status_str(st));
    }

    printf("8. REFIMG parity gate\n");
    save("ref.bin", base, nb);
    st = build_rgh_image_ex(argv[1], key, "out8.bin", NULL, &rep);
    CHECK(st == BUILD_OK, "identical reference -> %s", build_status_str(st));
    st = build_rgh_image_ex(argv[2], NULL, "out8b.bin", NULL, &rep);
    CHECK(st == BUILD_NOT_VERIFIED || memcmp(dump, base, nb) == 0, "different reference -> %s", build_status_str(st));
    remove("ref.bin");

    fclose(fb);
    printf("\n%s (%d failed)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails ? 1 : 0;
}
