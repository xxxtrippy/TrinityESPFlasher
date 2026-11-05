#ifndef SMC_H
#define SMC_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
/* Xbox 360 SMC crypt + reset-glitch patch. Ported from open-source J-Runner
 * (GPL) decrypt_SMC/encrypt_SMC/patch_SMC. SMC data files (TRINITY_CLEAN.bin
 * etc.) are stored decrypted; encrypt before placing in the NAND image. */
void smc_decrypt(uint8_t *smc, size_t n);
void smc_encrypt(uint8_t *smc, size_t n);
/* Removes the 5-reset limit (freeboot). Returns true if the pattern was found. */
bool smc_patch(uint8_t *smc, size_t n);
#endif
