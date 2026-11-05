#include "patches.h"
#include <string.h>
static uint32_t be32(const uint8_t *p){
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static const uint8_t* scan(const uint8_t *p, const uint8_t *end, size_t *seclen){
    const uint8_t *s=p;
    while(p+4<=end){
        if(be32(p)==0xFFFFFFFFu){ *seclen=(size_t)(p-s); return p+4; }
        if(p+8>end) return NULL;
        uint32_t n=be32(p+4);
        p+=8+(size_t)n*4;
    }
    return NULL;
}
bool patches_split(const uint8_t *f, size_t len, patchset_t *o){
    const uint8_t *end=f+len, *n;
    size_t l;
    o->cbb=f;      n=scan(o->cbb,end,&l); if(!n) return false; o->cbb_len=l;
    o->cd=n;       n=scan(o->cd,end,&l);  if(!n) return false; o->cd_len=l;
    o->hv=n;       n=scan(o->hv,end,&l);  if(!n) return false; o->hv_len=l;
    return true;
}
bool patches_apply(const uint8_t *sec, size_t sl, uint8_t *buf, size_t bl){
    const uint8_t *p=sec, *end=sec+sl;
    while(p+8<=end){
        uint32_t addr=be32(p), n=be32(p+4);   /* addr is a byte offset */
        p+=8;
        if(p+(size_t)n*4>end) return false;
        if((size_t)addr+(size_t)n*4>bl) return false;
        memcpy(buf+addr,p,(size_t)n*4);
        p+=(size_t)n*4;
    }
    return true;
}
