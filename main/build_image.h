#ifndef BUILD_IMAGE_H
#define BUILD_IMAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "xenon_keys.h"

/*
 * RGH/glitch image build pipeline.
 *
 * Input:  a raw NAND dump (from the console, via xbox_nand_read_block)
 *         + the user's CPU key
 *         + the xeBuild data package on the SD card
 * Output: a raw image ready to flash back.
 *
 * This is the native equivalent of what J-Runner drives through xeBuild. The
 * pieces that are pure arithmetic (ECC, image assembly/layout, copying
 * ingredients into their slots) are implemented here. The pieces that depend
 * on correct 360 crypto (keyvault decrypt, bootloader re-encryption, hash
 * chain) are staged and GATED behind verification flags so the firmware can
 * never silently flash an unverified image.
 */

typedef enum {
    BUILD_OK = 0,
    BUILD_NEED_CPU_KEY,
    BUILD_NEED_PACKAGE,       /* SD package missing required ingredients       */
    BUILD_BAD_DUMP,
    BUILD_NOT_VERIFIED,       /* a required stage is not yet bench-verified    */
    BUILD_ERROR,
} build_status_t;

typedef struct {
    /* progress for the web UI, updated as the build runs */
    volatile int   percent;
    volatile char  stage[48];
    volatile build_status_t status;
} build_progress_t;

/*
 * Run the full build. `raw_dump`/`raw_len` is the verified console dump.
 * `cpu_key` is 16 bytes (must be known). `out_image` must be raw_len bytes.
 * Progress is reported through `prog` (may be NULL).
 *
 * Returns BUILD_OK only if every stage completed AND every gated stage is
 * verified. Otherwise returns the first blocking status.
 */
build_status_t build_rgh_image(const uint8_t *raw_dump, uint32_t raw_len,
                               const uint8_t cpu_key[16],
                               uint8_t *out_image,
                               build_progress_t *prog);

/* Human string for a status, for the web UI. */
const char *build_status_str(build_status_t s);

#endif
