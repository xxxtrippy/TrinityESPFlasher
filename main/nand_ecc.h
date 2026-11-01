#ifndef NAND_ECC_H
#define NAND_ECC_H

#include <stdint.h>

/*
 * Xbox 360 NAND ECC (spare-area) handling.
 *
 * A raw dump is a sequence of 0x210-byte pages: 0x200 data + 0x10 spare. The
 * spare holds a block/page address and a Hamming-style ECC the SB computes.
 * J-Runner's "raw <-> ECC" conversions strip or regenerate these so xeBuild
 * can work on a flat logical image, then re-attach valid spare before writing
 * back.
 *
 * Both directions are deterministic and fully implementable on-chip. This is
 * the one part of the "build" that is pure, testable arithmetic with no
 * per-console secrets involved.
 */

/* Compute the 4-byte ECC + metadata for one 0x200 data page at logical block
 * `page_no`, writing the full 0x10 spare bytes into `spare_out`. */
void nand_ecc_make_spare(const uint8_t data[0x200], uint32_t page_no,
                         uint8_t spare_out[0x10]);

/* Convert a raw dump (data+spare per page) into a flat logical image (data
 * only). `pages` pages are processed. Returns 0 on success. */
int nand_raw_to_logical(const uint8_t *raw, uint32_t pages, uint8_t *logical_out);

/* Convert a flat logical image back to raw (data+spare) by regenerating spare.
 * Returns 0 on success. */
int nand_logical_to_raw(const uint8_t *logical, uint32_t pages, uint8_t *raw_out);

#endif
