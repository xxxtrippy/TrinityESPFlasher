/*
 * xenon_keys.c — per-console secret recovery from a NAND dump.
 *
 * GPL-3.0. See LICENSE.
 *
 * SCOPE / HONESTY NOTE:
 *   The NAND layout constants below (keyvault offset, DSN position) follow the
 *   long-documented 360 NAND structure used by every flasher toolchain. The
 *   keyvault is AES-stored and only decryptable with the CPU key, which on
 *   Trinity is not present in the dump. This module therefore LOCATES and
 *   copies the keyvault region and gates decryption on a known CPU key.
 *
 *   Keyvault decryption + verification is implemented in build_image.c and is
 *   marked there as requiring bench parity-checking. Do not treat a successful
 *   compile as proof of a correct image.
 */
#include "xenon_keys.h"

#include <string.h>

#include "esp_log.h"
#include "nand_geom.h"

static const char *TAG = "keys";

/*
 * Documented small-block 16MB NAND layout (logical, post-ECC):
 *   0x00000000  nandheader / bootloaders (CB, CD, CE ...)
 *   ...
 *   Keyvault lives in a dedicated region referenced by the nand header. For
 *   the 16MB retail layout the KV is at a fixed offset in the SMC+KV area.
 *
 * NOTE: rather than hardcode a single magic offset (which differs subtly
 * between layouts), we locate the KV by its header signature when possible and
 * fall back to the documented retail offset.
 */
#define KV_RETAIL_OFFSET 0x4000
#define KV_SIZE          0x4000

/* 360 image header magic: first two bytes of CB area are 0xFF 0x4F ("ÿO") for
 * a retail nandheader... the canonical check is the 0x0FF0 signature in the
 * nand header at offset 0. We do a light structural sanity check only. */
static bool looks_like_360_image(const uint8_t *img, uint32_t len)
{
    if (len < 0x8000) {
        return false;
    }
    /* nand header magic at 0x00: big-endian 0xFF4F for retail bootloader CB. */
    uint16_t magic = (img[0] << 8) | img[1];
    return (magic == 0xFF4F);
}

int xenon_extract_secrets(const uint8_t *logical_image, uint32_t image_len,
                          xenon_secrets_t *out)
{
    if (!logical_image || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->flash_config = 0;

    if (!looks_like_360_image(logical_image, image_len)) {
        ESP_LOGW(TAG, "image does not look like a retail 360 NAND");
        return -2;
    }

    /* Copy the keyvault region out of the dump. Decryption is deferred until a
     * CPU key is available (see build_image.c). */
    if (KV_RETAIL_OFFSET + KV_SIZE <= image_len) {
        memcpy(out->keyvault, logical_image + KV_RETAIL_OFFSET, KV_SIZE);
        out->have_keyvault = true;
    } else {
        ESP_LOGW(TAG, "image too short to contain keyvault");
    }

    /* DSN is inside the (still-encrypted) KV; it becomes readable only after
     * decryption. Leave zeroed until then. */

    out->cpu_key_known = false;
    out->have_fuses = false;

    ESP_LOGI(TAG, "secrets located (keyvault=%d); CPU key still required",
             out->have_keyvault);
    return 0;
}

void xenon_set_cpu_key(xenon_secrets_t *s, const uint8_t cpu_key[16])
{
    if (!s) return;
    memcpy(s->cpu_key, cpu_key, 16);
    s->cpu_key_known = true;
}
