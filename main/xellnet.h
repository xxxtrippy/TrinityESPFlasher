#ifndef XELLNET_H
#define XELLNET_H
#include <stdbool.h>
/* Find XeLL on the local WiFi and grab the CPU key from its web page.
 * Scans the ESP's own /24 subnet for XeLL's HTTP server, scrapes "CPU Key:",
 * validates the 32-hex key with the CPU-key ECC, writes /sd/cpukey.txt and
 * loads it into the job engine. Returns true on a validated capture. */
bool xellnet_fetch(char cpukey_out[33]);

/* Pure, host-testable helpers. */
bool xell_parse_key(const char *page, char out[33]);   /* extract 32-hex after "CPU Key" */
bool cpukey_ecc_valid(const char *hex32);              /* true if a valid 360 CPU key */
#endif
