# Host tests

`test_xecrypt_bl.c` validates the bootloader crypto in `main/xecrypt_bl.c`
against the published xeBuild `.ini` CRCs. Build and run on a PC:

    gcc -O2 -I../main test_xecrypt_bl.c ../main/xecrypt_bl.c -o t_bl
    ./t_bl xboxupd.bin cba_9188.bin cbb_9188.bin cd_9452.bin

All lines should print matching CRCs and "OK".
