/*
 * wifi.c — network bring-up for TrinityWeb360.
 *
 * GPL-3.0. See LICENSE.
 *
 * Default mode is STATION: the board joins your existing home/bench WiFi and
 * gets an IP from your router, so you reach it from your PC browser at that IP
 * (printed to the serial monitor on boot). If it can't join after a few tries
 * (wrong password, network down), it falls back to its own access point so you
 * are never locked out.
 *
 *   1. Edit STA_SSID / STA_PASS below to match your network, then rebuild.
 *   2. After flashing, run `idf.py -p <PORT> monitor` and watch for:
 *         connected, IP: 192.168.x.y
 *         open http://192.168.x.y/ in your browser
 *   3. Open that URL on any PC/phone on the same network.
 *
 * Tip: give the board a fixed address by reserving a DHCP lease for its MAC in
 * your router, or enable the static-IP block below.
 */
#include "wifi.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"

/* ======================= EDIT THESE FOR YOUR NETWORK ======================= */
#define STA_SSID "Nacho.Wifi"
#define STA_PASS "Dakodaxj87"
/* ========================================================================== */

/*
 * Fixed IP without hardcoding your subnet.
 *
 * The board first gets a DHCP address (which reveals your router's network),
 * then pins its last octet to STATIC_HOST_OCTET on that same subnet — so it
 * lands on the same address every boot no matter what network you're on:
 *   192.168.1.x network -> 192.168.1.250
 *   10.0.0.x    network -> 10.0.0.250
 * You only ever edit SSID/password above.
 *
 * Set to 0 for plain DHCP (address may change). If .250 is already taken on
 * your LAN or outside its range, pick another free number (2-254).
 */
#define STATIC_HOST_OCTET 250

/* Fallback access point — only started if the home network can't be joined. */
#define AP_SSID "TrinityFlasher"
#define AP_PASS "flash360"

#define MAX_RETRY 8

static const char *TAG = "wifi";
static EventGroupHandle_t s_eg;
#define BIT_CONNECTED BIT0
#define BIT_FAILED    BIT1
static int s_retry = 0;
static esp_netif_t *s_sta_netif = NULL;

/* Re-pin the last octet onto the subnet DHCP just gave us. */
static void pin_static_ip(const esp_netif_ip_info_t *dhcp)
{
#if STATIC_HOST_OCTET
    uint32_t net = dhcp->ip.addr & dhcp->netmask.addr;      /* e.g. 192.168.1.0 */
    uint32_t want = net | ((uint32_t)(STATIC_HOST_OCTET) << 24);

    /* Already on the target address? nothing to do. */
    if (dhcp->ip.addr == want) {
        return;
    }

    esp_netif_dhcpc_stop(s_sta_netif);
    esp_netif_ip_info_t fixed = {
        .ip      = { .addr = want },
        .gw      = dhcp->gw,
        .netmask = dhcp->netmask,
    };
    if (esp_netif_set_ip_info(s_sta_netif, &fixed) == ESP_OK) {
        ESP_LOGI(TAG, "pinned fixed IP: " IPSTR, IP2STR(&fixed.ip));
        ESP_LOGI(TAG, "open http://" IPSTR "/ in your browser",
                 IP2STR(&fixed.ip));
    } else {
        ESP_LOGW(TAG, "could not pin static IP; staying on DHCP address");
    }
#else
    (void)dhcp;
#endif
}

static void on_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "join failed, retry %d/%d", s_retry, MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_eg, BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "joined, DHCP offered: " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry = 0;
        pin_static_ip(&e->ip_info);
#if !STATIC_HOST_OCTET
        ESP_LOGI(TAG, "open http://" IPSTR "/ in your browser",
                 IP2STR(&e->ip_info.ip));
#endif
        xEventGroupSetBits(s_eg, BIT_CONNECTED);
    }
}

static void start_ap_fallback(void)
{
    ESP_LOGW(TAG, "could not join '%s' — starting fallback AP '%s'",
             STA_SSID, AP_SSID);
    esp_wifi_stop();
    esp_netif_create_default_wifi_ap();

    wifi_config_t ap = { 0 };
    strncpy((char *)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(AP_SSID);
    strncpy((char *)ap.ap.password, AP_PASS, sizeof(ap.ap.password));
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = strlen(AP_PASS) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGW(TAG, "fallback AP up: join '%s' then open http://192.168.4.1/",
             AP_SSID);
}

void wifi_init(void)
{
    s_eg = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_evt, NULL, NULL));

    wifi_config_t sta = { 0 };
    strncpy((char *)sta.sta.ssid, STA_SSID, sizeof(sta.sta.ssid));
    strncpy((char *)sta.sta.password, STA_PASS, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "joining '%s' ...", STA_SSID);
    EventBits_t bits = xEventGroupWaitBits(
        s_eg, BIT_CONNECTED | BIT_FAILED, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(20000));

    if (!(bits & BIT_CONNECTED)) {
        start_ap_fallback();
    }
}
