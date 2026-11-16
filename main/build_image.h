#ifndef BUILD_IMAGE_H
#define BUILD_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


/*
 * RGH3 image builder (streaming). See build_image.c for the model.
 *
 * Input:  a raw NAND dump FILE of the target console (0x210 bytes per page)
 *         + /sd/RGH3BASE.BIN, a finished RGH3 image to re-target
 *         + optionally the target's CPU key (authenticates the KeyVault and
 *           lets missing per-console files be regenerated)
 * Output: a raw image FILE, ready to flash.
 *
 * Everything streams page by page: a raw Trinity image is ~16.5 MiB and the
 * N16R8 has 8 MB of PSRAM, so no image is ever held in RAM.
 */

typedef enum {
    BUILD_OK = 0,
    BUILD_NEED_CPU_KEY,       /* a per-console file must be regenerated        */
    BUILD_NEED_PACKAGE,       /* RGH3BASE.BIN missing or not an RGH3 image     */
    BUILD_BAD_DUMP,
    BUILD_BAD_KEY,            /* CPU key does not match the dump's KeyVault    */
    BUILD_BAD_BLOCKS,         /* dump has bad/remapped blocks we would need    */
    BUILD_NOT_VERIFIED,       /* REFIMG.BIN present and the output differs     */
    BUILD_ERROR,
} build_status_t;

typedef struct {
    volatile int   percent;
    volatile char  stage[48];
    volatile build_status_t status;
} build_progress_t;

/* Where each per-console file in the output came from. Order:
 * secdata.bin, extended.bin, crl.bin, dae.bin, fcrt.bin. */
typedef enum { BUILD_SRC_BASE = 0, BUILD_SRC_DUMP, BUILD_SRC_FRESH } build_src_t;

typedef struct {
    bool        kv_verified;      /* KeyVault HMAC checked with the CPU key      */
    bool        same_console;     /* base was built for the dump's console       */
    bool        config_from_dump; /* config blocks (SMC config) taken from dump  */
    bool        dump_fs;          /* dump had a readable filesystem              */
    build_src_t file_src[5];
    uint32_t    pages_patched;    /* base pages rewritten by a file overlay      */
    uint32_t    mobile_erased;    /* mobile-data blocks erased                   */
    uint32_t    dump_ecc_fixed;   /* copied dump pages whose ECC was inconsistent */
    bool        ref_matched;      /* REFIMG.BIN was present and byte-identical     */
} build_report_t;

/* Returns BUILD_OK only if the image was assembled AND passed its self-check
 * (every page's ECC, KeyVault identical to the dump, filesystem opens, CPU-key
 * files verify). cpu_key may be NULL. */
build_status_t build_rgh_image(const char *dump_path,
                               const uint8_t cpu_key[16],
                               const char *out_path,
                               build_progress_t *prog);

/* Same, and fills a report of what was done (rep may be NULL). */
build_status_t build_rgh_image_ex(const char *dump_path,
                                  const uint8_t cpu_key[16],
                                  const char *out_path,
                                  build_progress_t *prog,
                                  build_report_t *rep);

const char *build_status_str(build_status_t s);

/* KeyVault-format helpers (KeyVault, extended.bin: suffix = true;
 * secdata.bin: suffix = false). */
bool build_cc_verify(const uint8_t cpu_key[16], const uint8_t *buf, size_t len, bool suffix);
bool build_cc_make_blank(const uint8_t cpu_key[16], uint8_t *out, size_t len, bool suffix);

/* Test hook: replace the random source used for fresh files (NULL = default). */
typedef void (*build_random_fn)(uint8_t *out, size_t n);
void build_set_random(build_random_fn fn);

#endif
