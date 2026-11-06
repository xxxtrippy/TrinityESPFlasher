#define XELLNET_HOST_TEST
#include "xellnet.c"
#include <stdio.h>
int main(void){
  const char *page="<html><title>XeLL - Reloaded</title><body><table>"
    "<tr><td>CPU Key:</td><td>0944CC0BE4794B9F52A727BD33A2B7AD</td></tr>"
    "<tr><td>DVD Key:</td><td>8CFC1DFDCF5C37A52AE427C397A45363</td></tr></table></body>";
  char k[33];
  printf("parse %s -> %s\n", xell_parse_key(page,k)?"OK":"FAIL", k);
  printf("ecc valid(correct key) = %s (want yes)\n", cpukey_ecc_valid("0944CC0BE4794B9F52A727BD33A2B7AD")?"yes":"no");
  printf("ecc valid(bad key)     = %s (want no)\n",  cpukey_ecc_valid("0944CC0BE4794B9F52A727BD33A287AD")?"yes":"no");
  return 0;
}
