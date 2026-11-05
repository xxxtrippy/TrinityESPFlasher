#include "boot_region.h"
#include "xecrypt_bl.h"
#include "smc.h"
#include "patches.h"
#include <string.h>
#include "esp_log.h"
static const char *TAG="boot";
static const uint8_t K1BL[16]={0xDD,0x88,0xAD,0x0C,0x9E,0xD6,0x69,0xE7,0xB5,0x67,0x94,0xFB,0x68,0x56,0x3E,0xFA};
static uint32_t be32(const uint8_t*p){return (uint32_t)p[0]<<24|p[1]<<16|p[2]<<8|p[3];}

#define SMC_OFF 0x1000u
#define SMC_LEN 0x3000u

br_status_t boot_region_build(uint8_t *img, size_t len, const uint8_t cpukey[16],
                              const uint8_t *pfile, size_t plen)
{
    if (len < 0x18000) return BR_ERR_CHAIN;

    /* --- SMC: decrypt, remove reset limit, re-encrypt (size-preserving) --- */
    smc_decrypt(img+SMC_OFF, SMC_LEN);
    if (!smc_patch(img+SMC_OFF, SMC_LEN))
        ESP_LOGW(TAG,"SMC reset-limit pattern not found");
    smc_encrypt(img+SMC_OFF, SMC_LEN);

    /* --- walk the loader chain to find CB_A, CB_B, CD --- */
    patchset_t ps;
    if (!patches_split(pfile, plen, &ps)) return BR_ERR_CHAIN;

    uint32_t off = be32(img+8);
    if (off+16 > len || memcmp(img+off,"CB",2)!=0) return BR_ERR_CHAIN;
    uint32_t cba_off = off, cba_sz = be32(img+off+12);
    uint32_t cbb_off = cba_off + cba_sz;
    if (cbb_off+16 > len || memcmp(img+cbb_off,"CB",2)!=0) return BR_ERR_CHAIN;
    uint32_t cbb_sz = be32(img+cbb_off+12);
    uint32_t cd_off = cbb_off + cbb_sz;
    if (cd_off+16 > len || memcmp(img+cd_off,"CD",2)!=0) return BR_ERR_CHAIN;
    uint32_t cd_sz = be32(img+cd_off+12);

    /* --- CB_A: decrypt with 1BL to recover the key the next stage needs --- */
    uint8_t cba_key[16];
    bool nc = xc_cba_new_crypto(img+cba_off);
    /* work on a copy to get the key without disturbing the stored CB_A */
    static uint8_t cba_tmp[0x4000];
    if (cba_sz > sizeof(cba_tmp)) return BR_ERR_CHAIN;
    memcpy(cba_tmp, img+cba_off, cba_sz);
    xc_cb_decrypt(cba_tmp, cba_sz, K1BL, cba_key);

    /* --- CB_B: decrypt (cba_key+cpukey), apply CB_B patches, re-encrypt --- */
    xc_cbb_decrypt_cpukey(img+cbb_off, cbb_sz, cba_key, nc, cpukey);
    /* sanity: decrypted retail CB_B carries the XBOX_ROM tag at 0x392 */
    if (memcmp(img+cbb_off+0x392,"XBOX_ROM",8)!=0 &&
        memcmp(img+cbb_off+0x392,"\0\0\0\0\0\0\0\0",8)!=0) {
        ESP_LOGE(TAG,"CB_B decrypt failed (wrong CPU key?)");
        return BR_ERR_SIG;
    }
    if (!patches_apply(ps.cbb, ps.cbb_len, img+cbb_off, cbb_sz)) return BR_ERR_CHAIN;
    xc_cbb_encrypt_cpukey(img+cbb_off, cbb_sz, cba_key, nc, cpukey);

    /* --- CD: the glitch CD patches grow CD past its retail slot (0x4F20 ->
     * 0x6000) and shift CE/flashfs. That size-changing relocation + the HV
     * patch region placement is the bench-gated stage. Signal it. --- */
    (void)cd_sz;
    return BR_NEED_CD_EXTEND;
}
