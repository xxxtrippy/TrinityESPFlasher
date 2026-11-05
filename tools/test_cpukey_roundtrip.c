#include "xecrypt_bl.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(void){
  uint8_t cpukey[16]; for(int i=0;i<16;i++)cpukey[i]=i*7+1;
  uint8_t cba_key[16]; for(int i=0;i<16;i++)cba_key[i]=i*3+9;
  /* fake CB_B: header+nonce+body */
  size_t n=0x7800; uint8_t*cbb=malloc(n),*orig=malloc(n);
  for(size_t i=0;i<n;i++)cbb[i]=(uint8_t)(i*131+7); memcpy(orig,cbb,n);
  uint8_t keep_nonce[16];memcpy(keep_nonce,cbb+0x10,16);
  xc_cbb_encrypt_cpukey(cbb,n,cba_key,false,cpukey);
  int changed=memcmp(cbb+0x20,orig+0x20,n-0x20)!=0;
  xc_cbb_decrypt_cpukey(cbb,n,cba_key,false,cpukey);
  printf("CB_B roundtrip(nc=0) %s changed=%d\n",memcmp(cbb,orig,n)==0?"OK":"FAIL",changed);
  /* new-crypto variant */
  memcpy(cbb,orig,n);uint8_t ck2[16];memcpy(ck2,cba_key,16);
  xc_cbb_encrypt_cpukey(cbb,n,ck2,true,cpukey);
  memcpy(ck2,cba_key,16);xc_cbb_decrypt_cpukey(cbb,n,ck2,true,cpukey);
  printf("CB_B roundtrip(nc=1) %s\n",memcmp(cbb,orig,n)==0?"OK":"FAIL");
  /* CD */
  size_t m=0x6000; uint8_t*cd=malloc(m),*cdo=malloc(m);
  for(size_t i=0;i<m;i++)cd[i]=(uint8_t)(i*77+3); memcpy(cdo,cd,m);
  uint8_t nonce[16];memcpy(nonce,cd+0x10,16);
  xc_cd_encrypt(cd,m,nonce,cba_key);
  xc_cd_decrypt(cd,m,cba_key);
  printf("CD roundtrip %s\n",memcmp(cd,cdo,m)==0?"OK":"FAIL");
  return 0;
}
