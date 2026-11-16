# Host tests

`test_xecrypt_bl.c` validates the bootloader crypto in `main/xecrypt_bl.c`
against the published xeBuild `.ini` CRCs. Build and run on a PC:

    gcc -O2 -I../main test_xecrypt_bl.c ../main/xecrypt_bl.c -o t_bl
    ./t_bl xboxupd.bin cba_9188.bin cbb_9188.bin cd_9452.bin

All lines should print matching CRCs and "OK".

## Image builder test (`test_build_image.c`)

Runs the firmware's own builder (`main/build_image.c`, `flashfs.c`,
`nand_ecc.c`) on a PC against real files for one Trinity: a J-Runner
`updflash.bin`, a raw NAND read of the same console, and its CPU key.

    gcc -O2 -Ihost_shim -I../main -DBUILD_BASE_FILE='"base.bin"' \
        -DBUILD_REF_FILE='"ref.bin"' test_build_image.c ../main/build_image.c \
        ../main/flashfs.c ../main/nand_ecc.c ../main/xecrypt_bl.c -o t_build
    mkdir run && cd run
    ../t_build /path/updflash.bin /path/nanddump1.bin <32 hex cpu key>

It should end with `ALL PASSED`. Scratch images are written to the current
directory; delete them afterwards and never commit them.

## The builder as a PC command (`host_build.c`)

Same C files as the firmware, so its output is what BLD writes on the device.

    gcc -O2 -Ihost_shim -I../main -DBUILD_BASE_FILE='"RGH3BASE.BIN"' \
        -DBUILD_REF_FILE='"REFIMG.BIN"' host_build.c ../main/build_image.c \
        ../main/flashfs.c ../main/nand_ecc.c ../main/xecrypt_bl.c -o host_build
    cd /folder/with/RGH3BASE.BIN
    host_build NANDDUMP.BIN IMAGE.BIN <32 hex cpu key>

Copy the result to the SD card as `REFIMG.BIN` to have the device check its own
build against it (delete it afterwards).
