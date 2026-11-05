#include "patches.h"
#include <stdio.h>
#include <stdlib.h>
static size_t rd(const char*p,uint8_t**b){FILE*f=fopen(p,"rb");fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);*b=malloc(n);if(fread(*b,1,n,f)!=(size_t)n){}fclose(f);return n;}
int main(int c,char**v){
  uint8_t*f;size_t n=rd(v[1],&f); patchset_t ps; patches_split(f,n,&ps);
  uint8_t*cbb=calloc(1,0x7800),*cd=calloc(1,0x6000);
  printf("CB_B(0x7800) %s  CD(0x6000) %s\n",
    patches_apply(ps.cbb,ps.cbb_len,cbb,0x7800)?"OK":"FAIL",
    patches_apply(ps.cd,ps.cd_len,cd,0x6000)?"OK":"FAIL");
  return 0;
}
