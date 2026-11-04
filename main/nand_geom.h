#ifndef NAND_GEOM_H
#define NAND_GEOM_H

#include <stdint.h>

/*
 * Xbox 360 NAND geometry.
 *
 * A "small block" controller (Trinity/Jasper 16MB, config major 0) addresses
 * the NAND in 0x200 (512-byte) logical pages, each carrying 0x10 (16) bytes of
 * spare/ECC data. A raw dump is therefore 0x210 bytes per page.
 *
 *   Trinity 16MB:  16MB of DATA = 0x1000000 data bytes = 0x8000 pages of
 *   0x200 => with spare: 0x8000 * 0x210 = 0x1080000 = 17,301,504 bytes
 *   (about 16.5 MiB) raw dump.
 *
 * We keep both the data size and the raw-with-spare size explicit so the web
 * UI can label downloads correctly and the builder can sanity-check a dump.
 */

#define NAND_PAGE_DATA   0x200u          /* 512 bytes of data per page       */
#define NAND_PAGE_SPARE  0x10u           /* 16 bytes of spare/ECC per page   */
#define NAND_PAGE_RAW    (NAND_PAGE_DATA + NAND_PAGE_SPARE) /* 0x210          */

/* Trinity / small-block 16MB part */
#define NAND_16MB_DATA_BYTES 0x1000000u                     /* 16 MB data     */
#define NAND_16MB_PAGES      (NAND_16MB_DATA_BYTES / NAND_PAGE_DATA) /* 0x8000 */
#define NAND_16MB_RAW_BYTES  (NAND_16MB_PAGES * NAND_PAGE_RAW)       /* ~17.3M */

/* Block-size (erase unit) is decoded from flash_config at runtime in xbox.c.
 * Small-block default is 0x4000 (32 pages); large-block Jasper variants use
 * 0x20000 / 0x40000. The builder reads the real value, never assumes. */
#define NAND_SMALLBLOCK_ERASE 0x4000u

/* Decode helpers for a flash_config word (see xbox_nand_write_block). */
static inline int nand_cfg_major(uint32_t fc) { return (fc >> 17) & 3; }
static inline int nand_cfg_minor(uint32_t fc) { return (fc >> 4) & 3; }

static inline uint32_t nand_block_bytes(uint32_t fc)
{
    int major = nand_cfg_major(fc);
    int minor = nand_cfg_minor(fc);
    uint32_t bs = NAND_SMALLBLOCK_ERASE;
    if (major >= 1) {
        if (minor == 2) bs = 0x20000u;
        else if (minor == 3) bs = 0x40000u;
    }
    return bs;
}

#endif
