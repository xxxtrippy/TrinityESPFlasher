#ifndef NAND_ECC_H
#define NAND_ECC_H

#include <stdint.h>

/*
 * Xbox 360 NAND ECC (spare-area) handling.
 *
 * A raw dump is a sequence of 0x210-byte pages: 0x200 data + 0x10 spare. The
 * spare is 12 bytes of metadata followed by a 4-byte ECC word (little endian)
 * at spare offset 12, i.e. page offset 0x20C.
 *
 * VERIFIED: nand_ecc_calc() reproduces the stored ECC word of every page of
 * J-Runner's bundled Trinity images (TRINITY.ecc, TRINITY_RGH3.ecc,
 * TRINITY_CR4.ecc: 2560 of 2560 pages each, zero mismatches).
 */

/* ECC word for one raw 0x210-byte page. The ECC field itself (0x20C..0x20F) is
 * ignored, so this works on a page whose ECC is stale or blank. */
uint32_t nand_ecc_calc(const uint8_t page[0x210]);

/* Recompute and store the ECC word in place. The 12 metadata bytes are kept
 * exactly as they are, so a page taken from a dump keeps its real metadata. */
void nand_ecc_fix_page(uint8_t page[0x210]);

/* Build a spare for a page that has no existing metadata: bootloader-area
 * metadata (block number = page_no/32 at byte 1, 0xFF at byte 5) plus the ECC.
 * Verified for pages 0..2559 (blocks 0..79) against J-Runner's Trinity images. */
void nand_ecc_make_spare(const uint8_t data[0x200], uint32_t page_no,
                         uint8_t spare_out[0x10]);

/* Convert a raw dump (data+spare per page) into a flat logical image (data
 * only). `pages` pages are processed. Returns 0 on success. */
int nand_raw_to_logical(const uint8_t *raw, uint32_t pages, uint8_t *logical_out);

/* Convert a flat logical image back to raw (data+spare) using make_spare(). Returns 0 on success. When rebuilding from a real dump,
 * prefer keeping the dump's own spare and calling nand_ecc_fix_page(). */
int nand_logical_to_raw(const uint8_t *logical, uint32_t pages, uint8_t *raw_out);

#endif
