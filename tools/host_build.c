/*
 * Run the firmware's image builder on a PC: same C files, same result as BLD
 * on the device. Use it to make a reference image (REFIMG.BIN) for the on-device
 * parity check, or to build without the hardware.
 *
 * Build (from tools/):
 *   gcc -O2 -Ihost_shim -I../main -DBUILD_BASE_FILE='"RGH3BASE.BIN"' \
 *       -DBUILD_REF_FILE='"REFIMG.BIN"' host_build.c ../main/build_image.c \
 *       ../main/flashfs.c ../main/nand_ecc.c ../main/xecrypt_bl.c -o host_build
 * Run in a folder that holds RGH3BASE.BIN:
 *   host_build NANDDUMP.BIN IMAGE.BIN [32-hex CPU key]
 *
 * Keep your images and key private; nothing here leaves the PC.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "build_image.h"

static const char *SRC[] = { "base", "dump", "fresh" };

int main(int argc, char **argv)
{
    uint8_t key[16];
    const uint8_t *kp = NULL;

    if (argc < 3) {
        fprintf(stderr, "usage: %s dump.bin out.bin [cpukey hex]\n", argv[0]);
        return 2;
    }
    if (argc > 3) {
        if (strlen(argv[3]) != 32) { fprintf(stderr, "CPU key must be 32 hex digits\n"); return 2; }
        for (int i = 0; i < 16; i++) {
            unsigned v;
            if (sscanf(argv[3] + 2 * i, "%2x", &v) != 1) { fprintf(stderr, "bad CPU key\n"); return 2; }
            key[i] = (uint8_t)v;
        }
        kp = key;
    }

    build_progress_t prog = { 0 };
    build_report_t rep;
    build_status_t st = build_rgh_image_ex(argv[1], kp, argv[2], &prog, &rep);

    printf("status: %s\n", st == BUILD_OK ? "Verified" : build_status_str(st));
    if (st == BUILD_OK) {
        printf("KeyVault authenticated: %s, same console as base: %s\n",
               rep.kv_verified ? "yes" : "no (no CPU key)", rep.same_console ? "yes" : "no");
        printf("files: secdata=%s extended=%s crl=%s dae=%s fcrt=%s\n",
               SRC[rep.file_src[0]], SRC[rep.file_src[1]], SRC[rep.file_src[2]],
               SRC[rep.file_src[3]], SRC[rep.file_src[4]]);
        printf("pages patched: %u, mobile blocks erased: %u, dump ECC fixed: %u\n",
               (unsigned)rep.pages_patched, (unsigned)rep.mobile_erased, (unsigned)rep.dump_ecc_fixed);
    }
    return st == BUILD_OK ? 0 : 1;
}
