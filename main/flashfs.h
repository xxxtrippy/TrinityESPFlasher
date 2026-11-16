#ifndef FLASHFS_H
#define FLASHFS_H
/*
 * flashfs — read-only view of the Xbox 360 NAND filesystem in a raw
 * small-block (16 MB Trinity) image: 0x210-byte pages, 32 pages per block.
 *
 * GPL-3.0. See LICENSE.
 *
 * Layout (checked against xeBuild images and a console read-back):
 *   - The root is one 0x4000 block whose pages carry FsBlockType 0x30 in the
 *     low 6 bits of spare byte 0xC. Several generations can exist; the one
 *     with the highest sequence number (spare bytes 0, 3, 4, 6) is current.
 *   - Even pages of the root hold the block map: big-endian u16 per block,
 *     "next block of this file"; 0x1FFF ends a chain, 0x1FFE is free.
 *   - Odd pages hold 32-byte entries: name[22], start block (BE u16),
 *     size (BE u32), timestamp (BE u32). name[0] == 0 ends the list,
 *     name[0] == 5 marks a deleted entry.
 *
 * Only images without remapped (bad) blocks are supported: every block that
 * is read must carry its own physical number in the spare.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define FFS_BLOCK_BYTES   0x4000u
#define FFS_PAGES_PER_BLK 32u
#define FFS_BLOCKS        0x400u          /* 16 MB / 0x4000 */
#define FFS_MAX_CHAIN     16              /* 256 KB: more than any per-console file */
#define FFS_TYPE_ROOT     0x30u

typedef struct {
    uint32_t root_block;
    uint32_t seq;
    uint8_t  map[16 * 0x200];             /* raw big-endian block map */
    uint8_t  ent[16 * 0x200];             /* raw directory entries    */
} ffs_t;

typedef struct {
    uint32_t size;                        /* bytes                                     */
    uint32_t entry_index;                 /* index of the 32-byte entry in the root    */
    uint16_t chain[FFS_MAX_CHAIN];        /* blocks, in order                          */
    int      nchain;
} ffs_file_t;

/* Spare helpers for a raw page. */
static inline uint32_t ffs_spare_block_id(const uint8_t *page) { return page[0x201] | ((uint32_t)(page[0x202] & 0x0F) << 8); }
static inline uint32_t ffs_spare_type(const uint8_t *page)     { return page[0x20C] & 0x3Fu; }
static inline uint32_t ffs_spare_seq(const uint8_t *page)
{
    return page[0x200] | ((uint32_t)page[0x203] << 8) | ((uint32_t)page[0x204] << 16) | ((uint32_t)page[0x206] << 24);
}

/* Read one raw page. Returns false on I/O error. */
bool ffs_read_page(FILE *raw, uint32_t page_no, uint8_t page[0x210]);

/* Read `len` logical bytes starting at logical offset `off` (page aligned). */
bool ffs_read_logical(FILE *raw, uint32_t off, uint8_t *dst, uint32_t len);

/* Find and load the current root. Returns false if there is none. */
bool ffs_open(FILE *raw, ffs_t *fs);

/* Look a file up by name (exact, case-sensitive). */
bool ffs_find(const ffs_t *fs, const char *name, ffs_file_t *out);

/* Read a whole file (out must hold nchain * 0x4000 bytes; the tail past
 * `size` is whatever the block holds). Fails if a block of the chain is
 * remapped (spare block id != physical block). */
bool ffs_read_file(FILE *raw, const ffs_file_t *f, uint8_t *out);

/* Logical byte offset of the size field (BE u32) of entry `entry_index`. */
uint32_t ffs_entry_size_offset(const ffs_t *fs, uint32_t entry_index);

#endif
