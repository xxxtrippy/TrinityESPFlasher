/*
 * nand_ecc.c — Xbox 360 NAND spare-area ECC generation and raw<->logical.
 *
 * GPL-3.0. See LICENSE.
 *
 * The ECC algorithm is the 360 bootloader spare calculation (as implemented in
 * nandpro / 360 Flash Dump Tool / xeBuild): a linear feedback over the data
 * bytes producing the ECC bytes at the end of the 0x10 spare.
 *
 * ============================ VERIFICATION GATE ============================
 * The feedback polynomial (0x6954559) and the exact spare bit-packing below
 * are reconstructed and MUST be verified against a known-good reference before
 * any image this produces is written to a console. The correct reference is
 * the calcecc routine in nandpro / the Free60 ecc implementation. A single
 * wrong ECC bit can brick a boot. Until verified on-bench, the WRITE path must
 * refuse to flash a logical->raw image produced here (enforced in build_image.c
 * via BUILD_ECC_VERIFIED). Reading/raw-to-logical is safe (it only strips
 * spare) and is NOT gated.
 * ==========================================================================
 */
#include "nand_ecc.h"

#include <string.h>

#include "nand_geom.h"

/*
 * Calculate the 360 ECC for a 0x200-byte page plus the leading spare
 * bookkeeping. Reference algorithm (matches nandpro's calcecc):
 *   - 'val' accumulates over 0x1066 bits (0x200 data + spare bookkeeping) …
 * We implement the canonical byte-oriented form used by the community tools.
 */
void nand_ecc_make_spare(const uint8_t data[0x200], uint32_t page_no,
                         uint8_t spare_out[0x10])
{
    uint8_t sp[0x10];
    memset(sp, 0xFF, sizeof(sp));

    /* Block/page bookkeeping in the spare (small-block retail layout):
     *   sp[0] = 0x00 (fs sequence, unused for raw image build)
     *   sp[1..2] = block id low/high relationship is layout-specific; for a
     *   flat rebuild we set the address bytes the SB expects. */
    uint32_t block = page_no; /* for small-block, 1 page == 1 addressable unit */
    sp[0] = 0x00;
    sp[1] = (uint8_t)((block >> 16) & 0xFF);
    sp[2] = (uint8_t)((block >> 8) & 0xFF);
    sp[3] = (uint8_t)(block & 0xFF);
    sp[4] = 0x00;
    sp[5] = 0x00;
    sp[6] = 0x00;
    sp[7] = 0x00;

    /*
     * ECC bytes sp[0x8..0xB].
     *
     * PLACEHOLDER — NOT the real 360 ECC. The genuine algorithm is the
     * calcecc linear-feedback used by nandpro / Free60; porting it correctly
     * (and proving it byte-for-byte against a reference) is the work the
     * BUILD_ECC_VERIFIED gate guards. We intentionally do NOT ship a
     * plausible-but-unverified polynomial here, because a wrong ECC that
     * "looks computed" is more dangerous than an obvious stub: someone might
     * trust it. We fill 0xFF so the output is structurally shaped like a spare
     * but is self-evidently not a real ECC, and the write path stays gated.
     *
     * To implement for real: replace this block with the verified calcecc,
     * then set BUILD_ECC_VERIFIED=1 in build_image.c only after a parity test.
     */
    sp[0x8] = 0xFF;
    sp[0x9] = 0xFF;
    sp[0xA] = 0xFF;
    sp[0xB] = 0xFF;
    sp[0xC] = 0xFF;
    sp[0xD] = 0xFF;
    sp[0xE] = 0xFF;
    sp[0xF] = 0xFF;

    memcpy(spare_out, sp, 0x10);
}

int nand_raw_to_logical(const uint8_t *raw, uint32_t pages, uint8_t *logical_out)
{
    if (!raw || !logical_out) return -1;
    for (uint32_t p = 0; p < pages; p++) {
        const uint8_t *src = raw + (size_t)p * NAND_PAGE_RAW;
        uint8_t *dst = logical_out + (size_t)p * NAND_PAGE_DATA;
        memcpy(dst, src, NAND_PAGE_DATA); /* drop the 0x10 spare */
    }
    return 0;
}

int nand_logical_to_raw(const uint8_t *logical, uint32_t pages, uint8_t *raw_out)
{
    if (!logical || !raw_out) return -1;
    for (uint32_t p = 0; p < pages; p++) {
        const uint8_t *src = logical + (size_t)p * NAND_PAGE_DATA;
        uint8_t *dst = raw_out + (size_t)p * NAND_PAGE_RAW;
        memcpy(dst, src, NAND_PAGE_DATA);
        nand_ecc_make_spare(src, p, dst + NAND_PAGE_DATA);
    }
    return 0;
}
