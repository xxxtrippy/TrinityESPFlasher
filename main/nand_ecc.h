#ifndef NAND_ECC_H
#define NAND_ECC_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Xbox 360 NAND ECC (spare-area) handling.
 *
 * A raw dump is a sequence of 0x210-byte pages: 0x200 data + 0x10 spare. The
 * spare is 12 bytes of metadata followed by a 4-byte ECC word (little endian)
 * at spare offset 12, i.e. page offset 0x20C.
 *
 * VERIFIED: nand_ecc_calc() reproduces the stored ECC word of every page of
 * full 16 MB xeBuild Trinity images (32768 of 32768 pages, zero mismatches),
 * see tools/test_build_image.c.
 */

/* ECC word for one raw 0x210-byte page, with the low 6 bits clear. The stored
 * ECC bits are ignored, but the 6 FsBlockType bits in the byte at 0x20C are an
 * input, so they must already hold their final value. */
uint32_t nand_ecc_calc(const uint8_t page[0x210]);

/* Recompute and store the ECC word in place. The 12 metadata bytes and the 6
 * FsBlockType bits are kept exactly as they are. Never call this on an erased
 * (all 0xFF) page: it would stop being erased. */
void nand_ecc_fix_page(uint8_t page[0x210]);

/* True if the whole raw page is 0xFF (erased). */
bool nand_page_is_blank(const uint8_t page[0x210]);

/* True if the stored ECC matches the page (erased pages count as good). */
bool nand_ecc_page_ok(const uint8_t page[0x210]);

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
