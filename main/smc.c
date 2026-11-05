#include "smc.h"
void smc_decrypt(uint8_t *s, size_t n){
    int k[4]={0x42,0x75,0x4E,0x79};
    for(size_t i=0;i<n;i++){int mod=s[i]*0xFB;s[i]^=(k[i&3]&0xFF);k[(i+1)&3]+=mod;k[(i+2)&3]+=mod>>8;}
}
void smc_encrypt(uint8_t *s, size_t n){
    int k[4]={0x42,0x75,0x4E,0x79};
    for(size_t i=0;i<n;i++){int mod=(s[i]^(k[i&3]&0xFF))*0xFB;s[i]^=(k[i&3]&0xFF);k[(i+1)&3]+=mod;k[(i+2)&3]+=mod>>8;}
}
bool smc_patch(uint8_t *s, size_t n){
    bool found=false;
    for(size_t i=0;i+8<n;i++)
        if(s[i]==0x05 && s[i+2]==0xE5 && s[i+4]==0xB4 && s[i+5]==0x05){ s[i]=0;s[i+1]=0;found=true; }
    return found;
}
