/*
 * flashfs.c — read-only Xbox 360 NAND filesystem access (small-block raw image).
 *
 * GPL-3.0. See LICENSE. Format notes are in flashfs.h.
 */
#include "flashfs.h"

#include <string.h>

#include "nand_geom.h"

bool ffs_read_page(FILE *raw, uint32_t page_no, uint8_t page[0x210])
{
    if (fseek(raw, (long)page_no * (long)NAND_PAGE_RAW, SEEK_SET) != 0) return false;
    return fread(page, 1, NAND_PAGE_RAW, raw) == NAND_PAGE_RAW;
}

bool ffs_read_logical(FILE *raw, uint32_t off, uint8_t *dst, uint32_t len)
{
    uint8_t pg[NAND_PAGE_RAW];
    if (off % NAND_PAGE_DATA) return false;
    for (uint32_t done = 0; done < len; done += NAND_PAGE_DATA) {
        uint32_t n = len - done < NAND_PAGE_DATA ? len - done : NAND_PAGE_DATA;
        if (!ffs_read_page(raw, (off + done) / NAND_PAGE_DATA, pg)) return false;
        memcpy(dst + done, pg, n);
    }
    return true;
}

static bool name_ok(const uint8_t *e)
{
    /* first entry of a real root: printable name, start block in range */
    if (e[0] < 0x20 || e[0] > 0x7E) return false;
    for (int i = 0; i < 22 && e[i]; i++)
        if (e[i] < 0x20 || e[i] > 0x7E) return false;
    uint32_t start = ((uint32_t)e[22] << 8) | e[23];
    return start < FFS_BLOCKS;
}

bool ffs_open(FILE *raw, ffs_t *fs)
{
    uint8_t pg[NAND_PAGE_RAW];
    bool found = false;
    uint32_t best_blk = 0, best_seq = 0;

    for (uint32_t b = 0; b < FFS_BLOCKS; b++) {
        if (!ffs_read_page(raw, b * FFS_PAGES_PER_BLK, pg)) return false;
        if (ffs_spare_type(pg) != FFS_TYPE_ROOT) continue;
        if (pg[0x205] != 0xFF) continue;                    /* bad-block mark  */
        if (ffs_spare_block_id(pg) != b) continue;          /* remapped: skip  */
        uint32_t seq = ffs_spare_seq(pg);
        /* the first directory page (page 1) must look like a directory */
        uint8_t e[NAND_PAGE_RAW];
        if (!ffs_read_page(raw, b * FFS_PAGES_PER_BLK + 1, e) || !name_ok(e)) continue;
        if (!found || seq >= best_seq) { found = true; best_seq = seq; best_blk = b; }
    }
    if (!found) return false;

    fs->root_block = best_blk;
    fs->seq = best_seq;
    for (uint32_t i = 0; i < 16; i++) {
        if (!ffs_read_page(raw, best_blk * FFS_PAGES_PER_BLK + i * 2, pg)) return false;
        memcpy(fs->map + i * 0x200, pg, 0x200);
        if (!ffs_read_page(raw, best_blk * FFS_PAGES_PER_BLK + i * 2 + 1, pg)) return false;
        memcpy(fs->ent + i * 0x200, pg, 0x200);
    }
    return true;
}

static uint32_t map_next(const ffs_t *fs, uint32_t blk)
{
    return ((uint32_t)fs->map[blk * 2] << 8) | fs->map[blk * 2 + 1];
}

bool ffs_find(const ffs_t *fs, const char *name, ffs_file_t *out)
{
    size_t nl = strlen(name);
    if (nl == 0 || nl > 22) return false;

    for (uint32_t i = 0; i < sizeof(fs->ent) / 32; i++) {
        const uint8_t *e = fs->ent + i * 32;
        if (e[0] == 0) break;                               /* end of list */
        if (e[0] == 5) continue;                            /* deleted     */
        if (memcmp(e, name, nl) != 0) continue;
        if (nl < 22 && e[nl] != 0) continue;

        uint32_t start = ((uint32_t)e[22] << 8) | e[23];
        uint32_t size  = ((uint32_t)e[24] << 24) | ((uint32_t)e[25] << 16) | ((uint32_t)e[26] << 8) | e[27];
        uint32_t need  = (size + FFS_BLOCK_BYTES - 1) / FFS_BLOCK_BYTES;
        if (need == 0) need = 1;
        if (need > FFS_MAX_CHAIN) return false;

        out->size = size;
        out->entry_index = i;
        out->nchain = 0;
        uint32_t blk = start;
        for (uint32_t k = 0; k < need; k++) {
            if (blk >= FFS_BLOCKS) return false;            /* chain ends early */
            out->chain[out->nchain++] = (uint16_t)blk;
            blk = map_next(fs, blk);
        }
        return true;
    }
    return false;
}

bool ffs_read_file(FILE *raw, const ffs_file_t *f, uint8_t *out)
{
    uint8_t pg[NAND_PAGE_RAW];
    for (int k = 0; k < f->nchain; k++) {
        uint32_t b = f->chain[k];
        for (uint32_t p = 0; p < FFS_PAGES_PER_BLK; p++) {
            if (!ffs_read_page(raw, b * FFS_PAGES_PER_BLK + p, pg)) return false;
            if (p == 0 && (ffs_spare_block_id(pg) != b || pg[0x205] != 0xFF)) return false;
            memcpy(out + (size_t)k * FFS_BLOCK_BYTES + p * NAND_PAGE_DATA, pg, NAND_PAGE_DATA);
        }
    }
    return true;
}

uint32_t ffs_entry_size_offset(const ffs_t *fs, uint32_t entry_index)
{
    uint32_t page = 1 + 2 * (entry_index / 16);             /* odd pages hold entries */
    return fs->root_block * FFS_BLOCK_BYTES + page * NAND_PAGE_DATA + (entry_index % 16) * 32 + 24;
}
