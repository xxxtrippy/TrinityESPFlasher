#ifndef XENON_KEYS_H
#define XENON_KEYS_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Per-console secrets recovered from a NAND dump.
 *
 * On Trinity (and all post-Zephyr consoles) the CPU key is NOT stored in NAND
 * and cannot be read directly — it lives in eFuses. J-Runner recovers it one
 * of two ways:
 *   1. If you already know it (from a prior glitch/ECC), you supply it.
 *   2. First-time: the glitch chip boots xeLL-reloaded, which prints the CPU
 *      key over video/serial; the user enters it. There is no way to compute
 *      the CPU key purely from a retail NAND dump.
 *
 * So this struct separates what we CAN derive from the dump (keyvault data,
 * fuses if present, DSN, SMC config, flash layout) from the CPU key, which is
 * an INPUT the user provides on first install.
 */

typedef struct {
    bool     cpu_key_known;
    uint8_t  cpu_key[16];     /* user-supplied on first install, or cached     */

    bool     have_keyvault;
    uint8_t  keyvault[0x4000];/* decrypted KV extracted from the dump          */

    uint8_t  dsn[6];          /* console serial-derived, from KV               */
    uint32_t flash_config;    /* as read from the SB                            */

    /* fuse lines, if recoverable from the dump's fuse region (console-dependent) */
    bool     have_fuses;
    uint8_t  fuses[12][8];
} xenon_secrets_t;

/*
 * Parse a raw NAND dump (data+spare, NAND_PAGE_RAW stride) that has been
 * de-ECC'd into a flat logical image. Fills what is derivable.
 *
 * Returns 0 on success, negative on structural failure (bad magic, not a
 * recognisable 360 image). A return of 0 does NOT mean the CPU key is known —
 * check cpu_key_known.
 *
 * STATUS: keyvault location/extraction is implemented against the documented
 * NAND layout; keyvault DECRYPTION requires the CPU key and is gated on it.
 * This path needs bench parity-checking against a known dump+key pair before
 * it can be trusted. It is NOT yet verified against real hardware output.
 */
int xenon_extract_secrets(const uint8_t *logical_image, uint32_t image_len,
                          xenon_secrets_t *out);

/* Set the user-supplied CPU key (16 bytes, hex-decoded by the caller). */
void xenon_set_cpu_key(xenon_secrets_t *s, const uint8_t cpu_key[16]);

#endif
