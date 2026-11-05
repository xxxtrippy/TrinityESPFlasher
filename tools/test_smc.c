#include "smc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t rd(const char*p,uint8_t**b){FILE*f=fopen(p,"rb");fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);*b=malloc(n);if(fread(*b,1,n,f)!=(size_t)n){}fclose(f);return n;}
int main(int c,char**v){
  uint8_t*s;size_t n=rd(v[1],&s);
  uint8_t*orig=malloc(n);memcpy(orig,s,n);
  int type=(s[0x100]>>4)&0xF, ver1=s[0x101], ver2=s[0x102];
  printf("raw SMC type=%d ver=%d.%d\n",type,ver1,ver2);
  /* encrypt then decrypt must roundtrip */
  smc_encrypt(s,n); int enc_changed=memcmp(s,orig,n)!=0; smc_decrypt(s,n);
  printf("encrypt/decrypt roundtrip %s (enc changed=%d)\n",memcmp(s,orig,n)==0?"OK":"FAIL",enc_changed);
  /* patch on the plaintext */
  bool f=smc_patch(s,n);
  printf("reset-limit patch found=%s\n",f?"yes":"no");
  return 0;
}
