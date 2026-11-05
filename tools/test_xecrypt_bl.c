#include "xecrypt_bl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t crc32_(const uint8_t*d,size_t n){uint32_t c=0xFFFFFFFF;for(size_t i=0;i<n;i++){c^=d[i];for(int k=0;k<8;k++)c=(c>>1)^(0xEDB88320&-(int)(c&1));}return ~c;}
static size_t rd(const char*p,uint8_t**b){FILE*f=fopen(p,"rb");if(!f){perror(p);exit(1);}fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);*b=malloc(n);fread(*b,1,n,f);fclose(f);return n;}
static uint32_t be32(const uint8_t*p){return p[0]<<24|p[1]<<16|p[2]<<8|p[3];}
int main(int c,char**v){
    const uint8_t k1[16]={0xDD,0x88,0xAD,0x0C,0x9E,0xD6,0x69,0xE7,0xB5,0x67,0x94,0xFB,0x68,0x56,0x3E,0xFA};
    /* sanity: known SHA1("abc") */
    uint8_t h[20];xc_sha1((const uint8_t*)"abc",3,h);
    printf("sha1(abc)=%02x%02x%02x%02x expect a9993e36\n",h[0],h[1],h[2],h[3]);
    /* CF/CG from xboxupd.bin */
    uint8_t*x;size_t xn=rd(v[1],&x);
    uint32_t cfsz=be32(x+0xc);uint8_t*cf=malloc(cfsz);memcpy(cf,x,cfsz);
    uint8_t*cgp=x+cfsz;uint32_t cgsz=be32(cgp+0xc);uint8_t*cg=malloc(cgsz);memcpy(cg,cgp,cgsz);
    xc_cf_decrypt(cf,cfsz,k1);
    { uint8_t*t=malloc(cfsz);memcpy(t,cf,cfsz);memset(t+0x20,0,0x210);printf("CF dec crc %08x want 0883e155\n",crc32_(t,cfsz));free(t);}
    xc_cg_decrypt(cg,cgsz,cf);
    { uint8_t*t=malloc(cgsz);memcpy(t,cg,cgsz);memset(t+0x10,0,0x10);printf("CG dec crc %08x want 10fbc84d\n",crc32_(t,cgsz));free(t);}
    /* CB_A/CB_B/CD decrypt-CRC (stored encrypted form, zero nonce region) */
    struct{const char*p;int o,n;uint32_t want;}T[]={
      {v[2],0x10,0x30,0x5a76752d},{v[3],0x10,0x30,0xfebb1074},{v[4],0x10,0x10,0x455fa02c}};
    for(int i=0;i<3;i++){uint8_t*b;size_t n=rd(T[i].p,&b);uint32_t sz=be32(b+0xc);uint8_t*t=malloc(sz);memcpy(t,b,sz);memset(t+T[i].o,0,T[i].n);printf("%s crc %08x want %08x\n",T[i].p,crc32_(t,sz),T[i].want);free(b);free(t);}
    /* CB_A decrypt->re-encrypt roundtrip */
    {uint8_t*b;size_t n=rd(v[2],&b);uint32_t sz=be32(b+0xc);uint8_t*orig=malloc(sz);memcpy(orig,b,sz);
     uint8_t nonce[16];memcpy(nonce,b+0x10,16);uint8_t key[16];
     xc_cb_decrypt(b,sz,k1,key);
     uint8_t*e=malloc(sz);memcpy(e,b,sz);xc_cba_encrypt(e,sz,nonce,k1,NULL);
     printf("CB_A roundtrip %s\n",memcmp(e,orig,sz)==0?"OK":"FAIL");free(b);free(orig);free(e);}
    return 0;
}
