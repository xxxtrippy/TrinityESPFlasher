/*
 * nand_ecc.c — Xbox 360 NAND spare-area ECC and raw<->logical conversion.
 *
 * GPL-3.0. See LICENSE.
 *
 * The ECC is a 26-bit linear feedback over the first 0x1066 bits of the
 * 0x210-byte page (data, the 12 metadata bytes, and the first 6 bits of the
 * ECC word, which is treated as zero), polynomial 0x6954559. The result is
 * inverted and shifted left by 6, then stored little endian at page offset
 * 0x20C.
 *
 * Checked against J-Runner's Trinity images: every page of TRINITY.ecc,
 * TRINITY_RGH3.ecc and TRINITY_CR4.ecc reproduces its stored ECC word.
 */
#include "nand_ecc.h"

#include <string.h>

#include "nand_geom.h"

#define ECC_OFFSET 0x20Cu

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t nand_ecc_calc(const uint8_t page[0x210])
{
    uint32_t val = 0, v = 0;

    for (uint32_t i = 0; i < 0x1066; i++) {
        if (!(i & 31)) {
            uint32_t off = (i >> 5) * 4;
            /* the ECC word itself counts as zero */
            v = ~((off < ECC_OFFSET) ? rd32le(page + off) : 0u);
        }
        val ^= v & 1;
        v >>= 1;
        if (val & 1) val ^= 0x6954559u;
        val >>= 1;
    }
    val = ~val;
    return val << 6;
}

void nand_ecc_fix_page(uint8_t page[0x210])
{
    uint32_t e = nand_ecc_calc(page);
    page[ECC_OFFSET + 0] = (uint8_t)(e);
    page[ECC_OFFSET + 1] = (uint8_t)(e >> 8);
    page[ECC_OFFSET + 2] = (uint8_t)(e >> 16);
    page[ECC_OFFSET + 3] = (uint8_t)(e >> 24);
}

void nand_ecc_make_spare(const uint8_t data[0x200], uint32_t page_no,
                         uint8_t spare_out[0x10])
{
    uint8_t page[NAND_PAGE_RAW];
    uint32_t block = page_no / 32u;             /* 32 pages per 0x4000 block */

    memcpy(page, data, NAND_PAGE_DATA);
    memset(page + NAND_PAGE_DATA, 0, NAND_PAGE_SPARE);
    /* Bootloader-area metadata: block number at byte 1, good-block marker 0xFF
     * at byte 5. Verified against J-Runner's Trinity images for blocks 0..79;
     * byte 2 (high part of the block number) is an assumption beyond 0xFF. */
    page[NAND_PAGE_DATA + 1] = (uint8_t)(block);
    page[NAND_PAGE_DATA + 2] = (uint8_t)(block >> 8);
    page[NAND_PAGE_DATA + 5] = 0xFF;
    nand_ecc_fix_page(page);
    memcpy(spare_out, page + NAND_PAGE_DATA, NAND_PAGE_SPARE);
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
