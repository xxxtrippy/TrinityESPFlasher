#ifndef XECRYPT_BL_H
#define XECRYPT_BL_H
/*
 * Xbox 360 bootloader crypto (RC4 + HMAC-SHA1 chain).
 * Ported from the open-source xeBuild / libxenon / J-Runner implementations
 * (public-domain / MIT / GPL). The 1BL key is the long-published public value.
 * Used only to rebuild an image for a console you own.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void xc_sha1(const uint8_t *d, size_t n, uint8_t out[20]);
void xc_hmac_sha1(const uint8_t *key, size_t klen,
                  const uint8_t *msg, size_t mlen, uint8_t out[20]);
void xc_rc4(const uint8_t *key, size_t klen, uint8_t *buf, size_t n);

/* In-place decrypt/encrypt of a bootloader. Each works on the whole loader
 * buffer; header bytes are preserved per the documented layout.
 * key_out (when non-NULL) receives the 16-byte RC4 key derived (the "CB_A key"
 * that the next stage needs). All buffers are the loader's own length. */
void xc_cb_decrypt(uint8_t *cb, size_t len, const uint8_t key1bl[16], uint8_t key_out[16]);
void xc_cba_encrypt(uint8_t *cba, size_t len, const uint8_t nonce[16], const uint8_t key1bl[16], uint8_t cba_key_out[16]);
bool xc_cba_new_crypto(const uint8_t *cba);
void xc_cbb_encrypt_cpukey(uint8_t *cbb, size_t len, const uint8_t cba_key[16],
                           bool new_crypto, const uint8_t cpukey[16]);
void xc_cd_encrypt(uint8_t *cd, size_t len, const uint8_t nonce[16], const uint8_t cbb_key[16]);
void xc_cf_decrypt(uint8_t *cf, size_t len, const uint8_t key1bl[16]);
void xc_cg_decrypt(uint8_t *cg, size_t len, const uint8_t *cf_decrypted);

#endif

/* Decrypt CB_B (needs the CB_A key + console CPU key) and CD (needs CB_B key),
 * the inverses of the encrypt functions, for rebuilding from a retail dump. */
void xc_cbb_decrypt_cpukey(uint8_t *cbb, size_t len, const uint8_t cba_key[16],
                           bool new_crypto, const uint8_t cpukey[16]);
void xc_cd_decrypt(uint8_t *cd, size_t len, const uint8_t cbb_key[16]);
