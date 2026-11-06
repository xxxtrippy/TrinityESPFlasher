#include "xellnet.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* ---- CPU key ECC validation (J-Runner CalculateCPUKeyECD + hamming==53) ---- */
static int hexval(char c){ if(c>='0'&&c<='9')return c-'0'; c|=0x20; if(c>='a'&&c<='f')return c-'a'+10; return -1; }
static bool hex_to_bytes(const char*h, uint8_t out[16]){
    for(int i=0;i<16;i++){int a=hexval(h[i*2]),b=hexval(h[i*2+1]); if(a<0||b<0)return false; out[i]=(uint8_t)(a<<4|b);} return true;
}
static void calc_ecd(uint8_t ecd[16]){
    uint32_t acc1=0,acc2=0;
    for(int cnt=0;cnt<0x80;cnt++,acc1>>=1){
        uint8_t bt=ecd[cnt>>3]; uint32_t dw=(bt>>(cnt&7))&1;
        if(cnt<0x6A){ acc1=dw^acc1; if(acc1&1)acc1^=0x360325; acc2=dw^acc2; }
        else if(cnt<0x7F){ if(dw!=(acc1&1))ecd[cnt>>3]=(uint8_t)((1<<(cnt&7))^bt); acc2=(acc1&1)^acc2; }
        else if(dw!=acc2) ecd[0xF]=(uint8_t)(0x80^bt);
    }
}
bool cpukey_ecc_valid(const char *h){
    uint8_t k[16],e[16];
    if(strlen(h)<32||!hex_to_bytes(h,k)) return false;
    int ham=0;
    for(int i=0;i<13;i++){ for(int b=0;b<8;b++){ if(k[i]&(1<<b)) ham++; } }
    if(k[13]&1) ham++;
    if(k[13]&2) ham++;
    if(ham!=53) return false;
    memcpy(e,k,16); calc_ecd(e);
    return memcmp(e,k,16)==0;
}

/* ---- parse the key out of XeLL's page: first 32-hex run after "CPU" ---- */
bool xell_parse_key(const char *page, char out[33]){
    const char *p = strstr(page,"CPU Key:");
    if(!p) p = strstr(page,"CPU");
    if(!p) return false;
    for(const char *q=p+3; *q; q++){
        int n=0; while(q[n] && hexval(q[n])>=0) n++;
        if(n>=32){ memcpy(out,q,32); out[32]=0;
                   for(int i=0;i<32;i++) out[i]=(char)toupper((unsigned char)out[i]);
                   return true; }
        if(n) q+=n;   /* skip the short run we just scanned */
    }
    return false;
}

#ifndef XELLNET_HOST_TEST
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "sdcard.h"
#include "jobs.h"
static const char *TAG="xell";

typedef struct { char *buf; int len, cap; } body_t;
static esp_err_t ev(esp_http_client_event_t *e){
    if(e->event_id==HTTP_EVENT_ON_DATA){
        body_t *b=e->user_data;
        int n=e->data_len; if(b->len+n>b->cap-1) n=b->cap-1-b->len;
        if(n>0){ memcpy(b->buf+b->len,e->data,n); b->len+=n; b->buf[b->len]=0; }
    }
    return ESP_OK;
}
static bool probe(const char *ip, char key[33]){
    char url[40]; snprintf(url,sizeof(url),"http://%s/",ip);
    static char page[4096]; body_t b={page,0,sizeof(page)};
    esp_http_client_config_t c={ .url=url, .event_handler=ev, .user_data=&b,
        .timeout_ms=400, .disable_auto_redirect=true };
    esp_http_client_handle_t h=esp_http_client_init(&c);
    bool ok=false;
    if(esp_http_client_perform(h)==ESP_OK && b.len>0){
        if(strstr(page,"Reloaded")||strstr(page,"XeLL")||strstr(page,"CPU"))
            if(xell_parse_key(page,key) && cpukey_ecc_valid(key)) ok=true;
    }
    esp_http_client_cleanup(h);
    return ok;
}
bool xellnet_fetch(char cpukey_out[33]){
    esp_netif_t *nif=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if(!nif || esp_netif_get_ip_info(nif,&info)!=ESP_OK || info.ip.addr==0){
        ESP_LOGW(TAG,"not on WiFi"); return false;
    }
    uint32_t ip=info.ip.addr, mask=info.netmask.addr;
    uint32_t net=ip&mask, host_lo=ntohl(net)+1, host_hi=ntohl(net|~mask)-1;
    ESP_LOGI(TAG,"scanning for XeLL...");
    for(uint32_t h=host_lo; h<=host_hi; h++){
        uint32_t a=htonl(h); if(a==ip) continue;
        char s[16]; snprintf(s,sizeof(s),"%u.%u.%u.%u",
            (unsigned)(a&0xff),(unsigned)((a>>8)&0xff),(unsigned)((a>>16)&0xff),(unsigned)((a>>24)&0xff));
        if(probe(s,cpukey_out)){
            ESP_LOGI(TAG,"XeLL at %s, CPU key captured",s);
            if(sd_is_mounted()){
                FILE *f=fopen(SD_MOUNT_POINT "/cpukey.txt","wb");
                if(f){ fwrite(cpukey_out,1,32,f); fputc('\n',f); fclose(f); }
            }
            jobs_set_cpukey_hex(cpukey_out);
            return true;
        }
    }
    ESP_LOGW(TAG,"XeLL not found on the subnet");
    return false;
}
#endif
