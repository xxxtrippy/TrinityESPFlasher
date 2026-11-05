#ifndef PATCHES_H
#define PATCHES_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
/* xeBuild glitch patch-file format (patches_g2*.bin):
 *   <CB_B patches> 0xFFFFFFFF <CD patches> 0xFFFFFFFF <kernel/HV patches> 0xFFFFFFFF
 * each patch: BE u32 byte-address, BE u32 word-count, then count BE u32 words.
 * CB_B and CD patches apply directly to the decrypted bootloader; kernel/HV
 * patches are placed in the NAND patch region and loaded by CD at boot. */
typedef struct { const uint8_t *cbb,*cd,*hv; size_t cbb_len,cd_len,hv_len; } patchset_t;
/* Split a patch file into its three sections. Returns false if malformed. */
bool patches_split(const uint8_t *file, size_t len, patchset_t *out);
/* Apply one section's {addr,data} patches to a bootloader buffer (byte offset =
 * word-address*4). Returns false if any patch would fall outside buf. */
bool patches_apply(const uint8_t *section, size_t sec_len, uint8_t *buf, size_t buf_len);
#endif
