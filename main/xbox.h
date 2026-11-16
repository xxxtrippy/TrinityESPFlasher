#ifndef XBOX_H
#define XBOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void xbox_init(void);

extern bool xbox_smc_stopped;

void xbox_start_smc(void);
void xbox_stop_smc(void);

uint32_t xbox_get_flash_config(void);

/* After a failed detect: one short line for the screen, e.g.
 * "nolink 0000 00000000 w=FFFLLH D1R1": MISO during the command, the value
 * read, each wire released and probed (SS, CLK, MOSI, MISO, DBG_EN, RST:
 * F floating, L held low, H held high), DBG_EN and RST read back while driven.
 * Also logs the raw bytes. */
void xbox_link_diag(char *out, size_t out_sz);
int xbox_nand_read_block(uint32_t lba, uint8_t *buffer, uint8_t *spare);
int xbox_nand_erase_block(uint32_t lba);
int xbox_nand_write_block(uint32_t lba, uint8_t *buffer, uint8_t *spare);

int xbox_emmc_init(void);
int xbox_emmc_read_cid(uint8_t *cid);
int xbox_emmc_read_csd(uint8_t *csd);
int xbox_emmc_read_ext_csd(uint8_t *ext_csd);
int xbox_emmc_read_block(int lba, uint8_t *buf);
int xbox_emmc_write_block(int lba, uint8_t *buf);

#endif
