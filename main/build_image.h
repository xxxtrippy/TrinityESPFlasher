#ifndef BUILD_IMAGE_H
#define BUILD_IMAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "xenon_keys.h"

/*
 * RGH/glitch image build pipeline (streaming).
 *
 * Input:  a raw NAND dump FILE on the SD card (data+spare, 0x210 per page)
 *         + the user's CPU key
 *         + the xeBuild data package on the SD card
 * Output: a raw image FILE on the SD card, ready to flash back.
 *
 * Everything streams through small chunks: a raw Trinity dump is ~16.5 MiB and
 * the N16R8 only has 8 MB of PSRAM, so the dump is never held in RAM.
 *
 * The crypto/ECC stages are GATED behind verification flags (see
 * build_image.c) so the firmware can never silently flash an unverified image.
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
    volatile int   percent;
    volatile char  stage[48];
    volatile build_status_t status;
} build_progress_t;

/* Returns BUILD_OK only if every stage completed AND every gated stage is
 * verified. Otherwise returns the first blocking status. */
build_status_t build_rgh_image(const char *dump_path,
                               const uint8_t cpu_key[16],
                               const char *out_path,
                               build_progress_t *prog);

const char *build_status_str(build_status_t s);

#endif
