/*
 * picolink.c — USB-host client for the PicoFlasher (CDC command protocol).
 *
 * GPL-3.0. See LICENSE.
 *
 * Protocol (from the PicoFlasher firmware, VID 600D / PID 7001):
 *   command on bulk OUT = packed { u8 cmd; u32 lba }   (5 bytes, little-endian)
 *     0x00 GET_VERSION        -> u32 version
 *     0x01 GET_FLASH_CONFIG   -> u32 flash config
 *     0x02 READ_FLASH   (lba) -> u32 ret; if ret==0 then 0x210 bytes
 *     0x03 WRITE_FLASH  (lba) -> [host also sends 0x210 bytes] -> u32 ret
 *   responses arrive on bulk IN, ending in a short packet.
 *
 * Three tasks:
 *   daemon  - runs usb_host_lib_handle_events()
 *   client  - runs usb_host_client_handle_events(); this is what dispatches
 *             every transfer-completion callback, so no callback may block it.
 *   setup   - opens/configures a newly attached device (control transfers), off
 *             the client task so it can block waiting on those callbacks.
 * The job worker calls picolink_read_block()/etc, which block on the same
 * callbacks; that is fine because they run on the jobs task, not the client.
 */
#include "picolink.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"
#include "esp_log.h"

#define PICO_VID 0x600D
#define PICO_PID 0x7001

#define CMD_GET_VERSION      0x00
#define CMD_GET_FLASH_CONFIG 0x01
#define CMD_READ_FLASH       0x02
#define CMD_WRITE_FLASH      0x03

#define PAGE_RAW  0x210            /* 0x200 data + 0x10 spare */
#define RX_CAP    0x240            /* >= 4 + PAGE_RAW, rounded up to a 64-byte multiple */
#define TX_CAP    (5 + PAGE_RAW)

#define XFER_TIMEOUT_MS 3000

static const char *TAG = "pico";

static usb_host_client_handle_t s_client;
static usb_device_handle_t      s_dev;
static volatile bool            s_ready;
static volatile uint8_t         s_new_addr;      /* 0 = none pending */
static volatile bool            s_gone;

static uint8_t  s_ep_in, s_ep_out;
static uint16_t s_mps_in = 64;
static uint8_t  s_comm_itf, s_data_itf;
static bool     s_data_claimed;

static usb_transfer_t *s_tx, *s_rx, *s_ctrl;
static SemaphoreHandle_t s_tx_done, s_rx_done, s_ctrl_done;
static SemaphoreHandle_t s_api_lock;      /* one command at a time */
static SemaphoreHandle_t s_evt;           /* new-device / gone signal to setup task */

static volatile usb_transfer_status_t s_tx_st, s_rx_st, s_ctrl_st;
static volatile int s_rx_actual;

static char s_status[40] = "USB: no PicoFlasher";
static uint32_t s_version;

/* ---- transfer callbacks (run on the client task) ---- */
static void tx_cb(usb_transfer_t *t)   { s_tx_st = t->status;   xSemaphoreGive(s_tx_done); }
static void rx_cb(usb_transfer_t *t)   { s_rx_st = t->status; s_rx_actual = t->actual_num_bytes; xSemaphoreGive(s_rx_done); }
static void ctrl_cb(usb_transfer_t *t) { s_ctrl_st = t->status; xSemaphoreGive(s_ctrl_done); }

/* ---- host library + client event loops ---- */
static void daemon_task(void *arg)
{
    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void client_event_cb(const usb_host_client_event_msg_t *e, void *arg)
{
    if (e->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        s_new_addr = e->new_dev.address;
        xSemaphoreGive(s_evt);
    } else if (e->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        s_gone = true;
        xSemaphoreGive(s_evt);
    }
}

static void client_task(void *arg)
{
    while (1) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
    }
}

/* ---- enumeration setup (off the client task) ---- */

/* Walk the config descriptor and record the CDC-data interface's bulk
 * endpoints and the comm/data interface numbers. */
static bool parse_config(const usb_config_desc_t *cfg)
{
    const uint8_t *p = (const uint8_t *)cfg;
    const uint8_t *end = p + cfg->wTotalLength;
    int cur_itf = -1, cur_class = -1;
    bool got_in = false, got_out = false;
    s_comm_itf = 0; s_data_itf = 0;

    while (p + 2 <= end && p[0] >= 2) {
        uint8_t len = p[0], type = p[1];
        if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE && len >= 9) {
            cur_itf = p[2];
            cur_class = p[5];
            if (cur_class == USB_CLASS_COMM) s_comm_itf = cur_itf;
            if (cur_class == USB_CLASS_CDC_DATA) s_data_itf = cur_itf;
        } else if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && len >= 7 &&
                   cur_class == USB_CLASS_CDC_DATA) {
            uint8_t addr = p[2];
            uint8_t attr = p[3];
            uint16_t mps = (uint16_t)(p[4] | (p[5] << 8));
            if ((attr & 0x03) == USB_BM_ATTRIBUTES_XFER_BULK) {
                if (addr & 0x80) { s_ep_in = addr; s_mps_in = mps ? mps : 64; got_in = true; }
                else             { s_ep_out = addr; got_out = true; }
            }
        }
        p += len;
    }
    return got_in && got_out;
}

/* Blocking control transfer (host-to-device, class, interface). */
static bool control_out(uint8_t req, uint16_t value, uint16_t index,
                        const uint8_t *data, uint16_t wlen)
{
    usb_setup_packet_t *sp = (usb_setup_packet_t *)s_ctrl->data_buffer;
    sp->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS |
                        USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    sp->bRequest = req;
    sp->wValue = value;
    sp->wIndex = index;
    sp->wLength = wlen;
    if (wlen && data) memcpy(s_ctrl->data_buffer + sizeof(usb_setup_packet_t), data, wlen);
    s_ctrl->num_bytes = sizeof(usb_setup_packet_t) + wlen;
    s_ctrl->bEndpointAddress = 0;
    s_ctrl->callback = ctrl_cb;
    s_ctrl->context = NULL;
    xSemaphoreTake(s_ctrl_done, 0);
    if (usb_host_transfer_submit_control(s_client, s_ctrl) != ESP_OK) return false;
    if (xSemaphoreTake(s_ctrl_done, pdMS_TO_TICKS(XFER_TIMEOUT_MS)) != pdTRUE) return false;
    return s_ctrl_st == USB_TRANSFER_STATUS_COMPLETED;
}

static void close_device(void)
{
    s_ready = false;
    if (s_data_claimed) { usb_host_interface_release(s_client, s_dev, s_data_itf); s_data_claimed = false; }
    if (s_dev) { usb_host_device_close(s_client, s_dev); s_dev = NULL; }
    snprintf(s_status, sizeof(s_status), "USB: no PicoFlasher");
}

static void open_device(uint8_t addr)
{
    if (usb_host_device_open(s_client, addr, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "device_open(%u) failed", addr);
        return;
    }
    const usb_device_desc_t *dd;
    if (usb_host_get_device_descriptor(s_dev, &dd) != ESP_OK) { close_device(); return; }
    if (dd->idVendor != PICO_VID || dd->idProduct != PICO_PID) {
        ESP_LOGI(TAG, "ignoring USB %04X:%04X (not a PicoFlasher)", dd->idVendor, dd->idProduct);
        usb_host_device_close(s_client, s_dev); s_dev = NULL;
        return;
    }
    const usb_config_desc_t *cfg;
    if (usb_host_get_active_config_descriptor(s_dev, &cfg) != ESP_OK || !parse_config(cfg)) {
        ESP_LOGW(TAG, "no CDC bulk endpoints"); close_device(); return;
    }
    if (usb_host_interface_claim(s_client, s_dev, s_data_itf, 0) != ESP_OK) {
        ESP_LOGW(TAG, "claim data itf %u failed", s_data_itf); close_device(); return;
    }
    s_data_claimed = true;

    /* Standard CDC bring-up: 115200 8N1, then DTR+RTS asserted. */
    const uint8_t line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08 };
    control_out(0x20 /*SET_LINE_CODING*/, 0, s_comm_itf, line_coding, 7);
    control_out(0x22 /*SET_CONTROL_LINE_STATE*/, 0x0003 /*DTR|RTS*/, s_comm_itf, NULL, 0);

    s_version = 0;
    picolink_get_version(&s_version);   /* also proves the data path */
    s_ready = true;
    snprintf(s_status, sizeof(s_status), "USB: PicoFlasher v%lu", (unsigned long)s_version);
    ESP_LOGI(TAG, "PicoFlasher ready: in=0x%02X out=0x%02X mps=%u ver=%lu",
             s_ep_in, s_ep_out, s_mps_in, (unsigned long)s_version);
}

static void setup_task(void *arg)
{
    while (1) {
        if (xSemaphoreTake(s_evt, portMAX_DELAY) != pdTRUE) continue;
        if (s_gone) {
            s_gone = false;
            if (s_dev) close_device();
        }
        uint8_t addr = s_new_addr;
        if (addr) {
            s_new_addr = 0;
            if (!s_dev) open_device(addr);
        }
    }
}

/* ---- bulk helpers (job worker side) ---- */
static bool bulk_out(const uint8_t *data, int len)
{
    memcpy(s_tx->data_buffer, data, len);
    s_tx->num_bytes = len;
    s_tx->bEndpointAddress = s_ep_out;
    s_tx->callback = tx_cb;
    s_tx->context = NULL;
    xSemaphoreTake(s_tx_done, 0);
    if (usb_host_transfer_submit(s_tx) != ESP_OK) return false;
    if (xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(XFER_TIMEOUT_MS)) != pdTRUE) {
        usb_host_endpoint_halt(s_dev, s_ep_out); usb_host_endpoint_flush(s_dev, s_ep_out);
        usb_host_endpoint_clear(s_dev, s_ep_out); return false;
    }
    return s_tx_st == USB_TRANSFER_STATUS_COMPLETED;
}

/* One bulk-IN transfer; returns bytes received or -1. */
static int bulk_in(int want_max)
{
    int rounded = ((want_max + s_mps_in - 1) / s_mps_in) * s_mps_in;
    if (rounded > RX_CAP) rounded = RX_CAP;
    s_rx->num_bytes = rounded;
    s_rx->bEndpointAddress = s_ep_in;
    s_rx->callback = rx_cb;
    s_rx->context = NULL;
    xSemaphoreTake(s_rx_done, 0);
    if (usb_host_transfer_submit(s_rx) != ESP_OK) return -1;
    if (xSemaphoreTake(s_rx_done, pdMS_TO_TICKS(XFER_TIMEOUT_MS)) != pdTRUE) {
        usb_host_endpoint_halt(s_dev, s_ep_in); usb_host_endpoint_flush(s_dev, s_ep_in);
        usb_host_endpoint_clear(s_dev, s_ep_in); return -1;
    }
    if (s_rx_st != USB_TRANSFER_STATUS_COMPLETED) return -1;
    return s_rx_actual;
}

/* Send a 5-byte command (optionally with a write payload). */
static bool send_cmd(uint8_t cmd, uint32_t lba, const uint8_t *payload, int payload_len)
{
    uint8_t hdr[5 + PAGE_RAW];
    hdr[0] = cmd;
    hdr[1] = (uint8_t)lba; hdr[2] = (uint8_t)(lba >> 8);
    hdr[3] = (uint8_t)(lba >> 16); hdr[4] = (uint8_t)(lba >> 24);
    int len = 5;
    if (payload && payload_len) { memcpy(hdr + 5, payload, payload_len); len += payload_len; }
    return bulk_out(hdr, len);
}

/* Collect exactly `need` bytes across one or more IN transfers. */
static bool read_exact(uint8_t *dst, int need)
{
    int got = 0;
    while (got < need) {
        int n = bulk_in(need - got);
        if (n <= 0) return false;
        if (n > need - got) n = need - got;
        memcpy(dst + got, s_rx->data_buffer, n);
        got += n;
    }
    return true;
}

/* ---- public API ---- */
bool picolink_present(void) { return s_ready; }
const char *picolink_status(void) { return s_status; }

static bool simple_u32(uint8_t cmd, uint32_t *out)
{
    if (!s_ready) return false;
    bool ok = false;
    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    if (send_cmd(cmd, 0, NULL, 0)) {
        uint8_t r[4];
        if (read_exact(r, 4)) {
            *out = (uint32_t)r[0] | ((uint32_t)r[1] << 8) | ((uint32_t)r[2] << 16) | ((uint32_t)r[3] << 24);
            ok = true;
        }
    }
    xSemaphoreGive(s_api_lock);
    return ok;
}

bool picolink_get_version(uint32_t *ver)       { return simple_u32(CMD_GET_VERSION, ver); }
bool picolink_get_flash_config(uint32_t *cfg)  { return simple_u32(CMD_GET_FLASH_CONFIG, cfg); }

int picolink_read_block(uint32_t lba, uint8_t *buf210)
{
    if (!s_ready) return -1;
    int rc = -1;
    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    if (send_cmd(CMD_READ_FLASH, lba, NULL, 0)) {
        uint8_t ret[4];
        if (read_exact(ret, 4)) {
            uint32_t r = (uint32_t)ret[0] | ((uint32_t)ret[1] << 8) |
                         ((uint32_t)ret[2] << 16) | ((uint32_t)ret[3] << 24);
            if (r != 0) rc = (int)r;                       /* NAND error, no page follows */
            else if (read_exact(buf210, PAGE_RAW)) rc = 0; /* page + spare */
        }
    }
    xSemaphoreGive(s_api_lock);
    return rc;
}

int picolink_write_block(uint32_t lba, const uint8_t *buf210)
{
    if (!s_ready) return -1;
    int rc = -1;
    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    if (send_cmd(CMD_WRITE_FLASH, lba, buf210, PAGE_RAW)) {
        uint8_t ret[4];
        if (read_exact(ret, 4)) {
            uint32_t r = (uint32_t)ret[0] | ((uint32_t)ret[1] << 8) |
                         ((uint32_t)ret[2] << 16) | ((uint32_t)ret[3] << 24);
            rc = (int)r;
        }
    }
    xSemaphoreGive(s_api_lock);
    return rc;
}

bool picolink_start(void)
{
    s_api_lock  = xSemaphoreCreateMutex();
    s_tx_done   = xSemaphoreCreateBinary();
    s_rx_done   = xSemaphoreCreateBinary();
    s_ctrl_done = xSemaphoreCreateBinary();
    s_evt       = xSemaphoreCreateBinary();
    if (!s_api_lock || !s_tx_done || !s_rx_done || !s_ctrl_done || !s_evt) return false;

    usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    if (usb_host_install(&host_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed (OTG already in use?)");
        return false;
    }
    usb_host_client_config_t ccfg = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = { .client_event_callback = client_event_cb, .callback_arg = NULL },
    };
    if (usb_host_client_register(&ccfg, &s_client) != ESP_OK) {
        ESP_LOGE(TAG, "client_register failed"); return false;
    }
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 16, 0, &s_ctrl) != ESP_OK ||
        usb_host_transfer_alloc(TX_CAP, 0, &s_tx) != ESP_OK ||
        usb_host_transfer_alloc(RX_CAP, 0, &s_rx) != ESP_OK) {
        ESP_LOGE(TAG, "transfer_alloc failed"); return false;
    }

    xTaskCreate(daemon_task, "usb_daemon", 4096, NULL, 4, NULL);
    xTaskCreate(client_task, "usb_client", 4096, NULL, 4, NULL);
    xTaskCreate(setup_task,  "usb_setup",  4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "USB host up, watching for a PicoFlasher");
    return true;
}
