#define XELLNET_HOST_TEST
#include "xellnet.c"
#include <stdio.h>
int main(void){
  const char *page="<html><title>XeLL - Reloaded</title><body><table>"
    "<tr><td>CPU Key:</td><td>0D65D670E58E0351D8AE8E4F6ECACCCC</td></tr>"
    "<tr><td>DVD Key:</td><td>00112233445566778899AABBCCDDEEFF</td></tr></table></body>";
  char k[33];
  /* Synthetic ECC-valid test vector (NOT a real console key): generated with
   * the J-Runner CalculateCPUKeyECD algorithm so cpukey_ecc_valid() accepts it;
   * the "bad" vector flips one ECD bit so it is rejected. */
  printf("parse %s -> %s\n", xell_parse_key(page,k)?"OK":"FAIL", k);
  printf("ecc valid(correct key) = %s (want yes)\n", cpukey_ecc_valid("0D65D670E58E0351D8AE8E4F6ECACCCC")?"yes":"no");
  printf("ecc valid(bad key)     = %s (want no)\n",  cpukey_ecc_valid("0D65D670E58E0351D8AE8E4F6ECACC8C")?"yes":"no");
  return 0;
}
