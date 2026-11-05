#ifndef BOOT_REGION_H
#define BOOT_REGION_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
/* Rebuild the glitch boot region in a LOGICAL (de-ECC'd) image buffer, using
 * the console CPU key and an xeBuild glitch patch file. Operates in place on
 * the first ~0x18000 bytes (header/SMC/KV/CB/CD). Returns:
 *   0  = SMC + CB_B stages applied (size-preserving)
 *  >0  = a stage needs the size-changing CD extension / patch region (bench-gated)
 *  <0  = error (bad chain, decrypt failure) */
typedef enum { BR_OK=0, BR_NEED_CD_EXTEND=1, BR_ERR_CHAIN=-1, BR_ERR_SIG=-2 } br_status_t;
br_status_t boot_region_build(uint8_t *logical, size_t loglen,
                              const uint8_t cpukey[16],
                              const uint8_t *patchfile, size_t patchlen);
#endif
