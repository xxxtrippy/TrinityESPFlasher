#include "xecrypt_bl.h"
#include <string.h>

/* ---- SHA1 (public domain, Steve Reid) ---- */
typedef struct { uint32_t s[5], c[2]; uint8_t b[64]; } SHA1_CTX;
#define ROL(v,b) (((v)<<(b))|((v)>>(32-(b))))
#define BLK0(i) (blk.l[i]=(ROL(blk.l[i],24)&0xFF00FF00)|(ROL(blk.l[i],8)&0x00FF00FF))
#define BLK(i) (blk.l[i&15]=ROL(blk.l[(i+13)&15]^blk.l[(i+8)&15]^blk.l[(i+2)&15]^blk.l[i&15],1))
#define R0(v,w,x,y,z,i) z+=((w&(x^y))^y)+BLK0(i)+0x5A827999+ROL(v,5);w=ROL(w,30);
#define R1(v,w,x,y,z,i) z+=((w&(x^y))^y)+BLK(i)+0x5A827999+ROL(v,5);w=ROL(w,30);
#define R2(v,w,x,y,z,i) z+=(w^x^y)+BLK(i)+0x6ED9EBA1+ROL(v,5);w=ROL(w,30);
#define R3(v,w,x,y,z,i) z+=(((w|x)&y)|(w&x))+BLK(i)+0x8F1BBCDC+ROL(v,5);w=ROL(w,30);
#define R4(v,w,x,y,z,i) z+=(w^x^y)+BLK(i)+0xCA62C1D6+ROL(v,5);w=ROL(w,30);
static void sha1_tr(uint32_t st[5], const uint8_t buf[64]) {
    uint32_t a,b,c,d,e; union{uint8_t c[64];uint32_t l[16];} blk; memcpy(blk.c,buf,64);
    a=st[0];b=st[1];c=st[2];d=st[3];e=st[4];
    R0(a,b,c,d,e,0)R0(e,a,b,c,d,1)R0(d,e,a,b,c,2)R0(c,d,e,a,b,3)R0(b,c,d,e,a,4)
    R0(a,b,c,d,e,5)R0(e,a,b,c,d,6)R0(d,e,a,b,c,7)R0(c,d,e,a,b,8)R0(b,c,d,e,a,9)
    R0(a,b,c,d,e,10)R0(e,a,b,c,d,11)R0(d,e,a,b,c,12)R0(c,d,e,a,b,13)R0(b,c,d,e,a,14)R0(a,b,c,d,e,15)
    R1(e,a,b,c,d,16)R1(d,e,a,b,c,17)R1(c,d,e,a,b,18)R1(b,c,d,e,a,19)
    R2(a,b,c,d,e,20)R2(e,a,b,c,d,21)R2(d,e,a,b,c,22)R2(c,d,e,a,b,23)R2(b,c,d,e,a,24)
    R2(a,b,c,d,e,25)R2(e,a,b,c,d,26)R2(d,e,a,b,c,27)R2(c,d,e,a,b,28)R2(b,c,d,e,a,29)
    R2(a,b,c,d,e,30)R2(e,a,b,c,d,31)R2(d,e,a,b,c,32)R2(c,d,e,a,b,33)R2(b,c,d,e,a,34)
    R2(a,b,c,d,e,35)R2(e,a,b,c,d,36)R2(d,e,a,b,c,37)R2(c,d,e,a,b,38)R2(b,c,d,e,a,39)
    R3(a,b,c,d,e,40)R3(e,a,b,c,d,41)R3(d,e,a,b,c,42)R3(c,d,e,a,b,43)R3(b,c,d,e,a,44)
    R3(a,b,c,d,e,45)R3(e,a,b,c,d,46)R3(d,e,a,b,c,47)R3(c,d,e,a,b,48)R3(b,c,d,e,a,49)
    R3(a,b,c,d,e,50)R3(e,a,b,c,d,51)R3(d,e,a,b,c,52)R3(c,d,e,a,b,53)R3(b,c,d,e,a,54)
    R3(a,b,c,d,e,55)R3(e,a,b,c,d,56)R3(d,e,a,b,c,57)R3(c,d,e,a,b,58)R3(b,c,d,e,a,59)
    R4(a,b,c,d,e,60)R4(e,a,b,c,d,61)R4(d,e,a,b,c,62)R4(c,d,e,a,b,63)R4(b,c,d,e,a,64)
    R4(a,b,c,d,e,65)R4(e,a,b,c,d,66)R4(d,e,a,b,c,67)R4(c,d,e,a,b,68)R4(b,c,d,e,a,69)
    R4(a,b,c,d,e,70)R4(e,a,b,c,d,71)R4(d,e,a,b,c,72)R4(c,d,e,a,b,73)R4(b,c,d,e,a,74)
    R4(a,b,c,d,e,75)R4(e,a,b,c,d,76)R4(d,e,a,b,c,77)R4(c,d,e,a,b,78)R4(b,c,d,e,a,79)
    st[0]+=a;st[1]+=b;st[2]+=c;st[3]+=d;st[4]+=e;
}
static void sha1_init(SHA1_CTX*c){c->s[0]=0x67452301;c->s[1]=0xEFCDAB89;c->s[2]=0x98BADCFE;c->s[3]=0x10325476;c->s[4]=0xC3D2E1F0;c->c[0]=c->c[1]=0;}
static void sha1_upd(SHA1_CTX*c,const uint8_t*d,uint32_t len){uint32_t i,j=(c->c[0]>>3)&63;if((c->c[0]+=len<<3)<(len<<3))c->c[1]++;c->c[1]+=len>>29;
    if(j+len>63){memcpy(&c->b[j],d,(i=64-j));sha1_tr(c->s,c->b);for(;i+63<len;i+=64)sha1_tr(c->s,&d[i]);j=0;}else i=0;memcpy(&c->b[j],&d[i],len-i);}
static void sha1_fin(SHA1_CTX*c,uint8_t o[20]){uint8_t fc;uint8_t cnt[8];uint32_t i;
    for(i=0;i<8;i++)cnt[i]=(uint8_t)(c->c[(i>=4?0:1)]>>((3-(i&3))*8));
    fc=0x80;sha1_upd(c,&fc,1);while((c->c[0]&504)!=448){fc=0;sha1_upd(c,&fc,1);}sha1_upd(c,cnt,8);
    for(i=0;i<20;i++)o[i]=(uint8_t)(c->s[i>>2]>>((3-(i&3))*8));}
void xc_sha1(const uint8_t*d,size_t n,uint8_t o[20]){SHA1_CTX c;sha1_init(&c);sha1_upd(&c,d,(uint32_t)n);sha1_fin(&c,o);}

void xc_hmac_sha1(const uint8_t*key,size_t kl,const uint8_t*msg,size_t ml,uint8_t out[20]){
    uint8_t k[64],ip[64],op[64],ih[20];size_t i;SHA1_CTX c;
    memset(k,0,64); if(kl>64){xc_sha1(key,kl,k);}else memcpy(k,key,kl);
    for(i=0;i<64;i++){ip[i]=k[i]^0x36;op[i]=k[i]^0x5c;}
    sha1_init(&c);sha1_upd(&c,ip,64);sha1_upd(&c,msg,(uint32_t)ml);sha1_fin(&c,ih);
    sha1_init(&c);sha1_upd(&c,op,64);sha1_upd(&c,ih,20);sha1_fin(&c,out);
}
void xc_rc4(const uint8_t*key,size_t kl,uint8_t*buf,size_t n){
    uint8_t s[256];int i,j=0;for(i=0;i<256;i++)s[i]=(uint8_t)i;
    for(i=0;i<256;i++){j=(j+s[i]+key[i%kl])&255;uint8_t t=s[i];s[i]=s[j];s[j]=t;}
    i=j=0;for(size_t n2=0;n2<n;n2++){i=(i+1)&255;j=(j+s[i])&255;uint8_t t=s[i];s[i]=s[j];s[j]=t;buf[n2]^=s[(s[i]+s[j])&255];}
}

/* ---- bootloader chain ----
 * Layout of an encrypted loader: [0x00..0x10 header][0x10..0x20 nonce][0x20.. body].
 * key = HMAC(secret, nonce)[:16]; RC4 decrypts body in place; the stored copy
 * keeps the key where the nonce was. */
void xc_cb_decrypt(uint8_t*cb,size_t len,const uint8_t k1[16],uint8_t ko[16]){
    uint8_t h[20];xc_hmac_sha1(k1,16,cb+0x10,0x10,h);xc_rc4(h,16,cb+0x20,len-0x20);
    memcpy(cb+0x10,h,0x10); if(ko)memcpy(ko,h,16);
}
bool xc_cba_new_crypto(const uint8_t*cba){ uint16_t f=(cba[6]<<8)|cba[7]; return (f&0x1000)!=0; }
void xc_cba_encrypt(uint8_t*cba,size_t len,const uint8_t nonce[16],const uint8_t k1[16],uint8_t ko[16]){
    uint8_t h[20];xc_hmac_sha1(k1,16,nonce,16,h);memcpy(cba+0x10,nonce,16);xc_rc4(h,16,cba+0x20,len-0x20);
    if(ko)memcpy(ko,h,16);
}
void xc_cbb_encrypt_cpukey(uint8_t*cbb,size_t len,const uint8_t cba_key[16],bool nc,const uint8_t cpukey[16]){
    uint8_t nonce[16],msg[48],h[20];memcpy(nonce,cbb+0x10,16);
    memcpy(msg,nonce,16);memcpy(msg+16,cpukey,16);size_t ml=32;
    uint8_t secret[16];memcpy(secret,cba_key,16);
    if(nc){uint8_t ck[16];memcpy(ck,cba_key,16);ck[6]=0;ck[7]=0;memcpy(msg+32,ck,16);ml=48;}
    xc_hmac_sha1(secret,16,msg,ml,h);xc_rc4(h,16,cbb+0x20,len-0x20);/* nonce stays at 0x10 */
}
void xc_cd_encrypt(uint8_t*cd,size_t len,const uint8_t nonce[16],const uint8_t cbb_key[16]){
    uint8_t h[20];xc_hmac_sha1(cbb_key,16,nonce,16,h);memcpy(cd+0x10,nonce,16);xc_rc4(h,16,cd+0x20,len-0x20);
}
void xc_cf_decrypt(uint8_t*cf,size_t len,const uint8_t k1[16]){
    uint8_t h[20];xc_hmac_sha1(k1,16,cf+0x20,0x10,h);xc_rc4(h,16,cf+0x30,len-0x30);memcpy(cf+0x20,h,0x10);
}
void xc_cg_decrypt(uint8_t*cg,size_t len,const uint8_t*cf_dec){
    uint8_t h[20];xc_hmac_sha1(cf_dec+0x330,0x10,cg+0x10,0x10,h);xc_rc4(h,16,cg+0x20,len-0x20);memcpy(cg+0x10,h,0x10);
}

/* Build the CB_B RC4 key exactly as the encrypt path does, then RC4 (RC4 is its
 * own inverse), so decrypt == encrypt with the same derived key. */
void xc_cbb_decrypt_cpukey(uint8_t*cbb,size_t len,const uint8_t cba_key[16],bool nc,const uint8_t cpukey[16]){
    xc_cbb_encrypt_cpukey(cbb,len,cba_key,nc,cpukey);
}
void xc_cd_decrypt(uint8_t*cd,size_t len,const uint8_t cbb_key[16]){
    uint8_t nonce[16];memcpy(nonce,cd+0x10,16);
    xc_cd_encrypt(cd,len,nonce,cbb_key);  /* same derived key; RC4 self-inverse */
}
