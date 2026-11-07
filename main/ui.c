
/*
 * ui.c — on-screen UI for TrinityWeb360, driven by two push buttons.
 *
 * GPL-3.0. See LICENSE.
 *
 * Layout (320x240 landscape):
 *   header bar with a console-link dot
 *   status card: stage, console info, progress bar, percent, hint
 *   description of the highlighted action
 *   5 buttons: FND | DMP | XEL | BLD | FLSH, each with a "done" marker
 *   control hint
 *
 * Controls:
 *   RIGHT cycles the highlight and wraps from FLSH back to FND.
 *   SELECT runs the highlighted action.
 *   The CPU key for Build is read from /sd/cpukey.txt.
 */

#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "pins.h"
#include "lcd.h"
#include "jobs.h"
#include "sdcard.h"

/* ---- Xbox green palette ---- */

#define COL_BG     RGB565(0x03, 0x0B, 0x06)
#define COL_HEAD   RGB565(0x05, 0x2B, 0x13)
#define COL_PANEL  RGB565(0x0B, 0x1C, 0x11)
#define COL_EDGE   RGB565(0x1B, 0x5A, 0x32)
#define COL_TRACK  RGB565(0x02, 0x08, 0x04)
#define COL_DIM    RGB565(0x78, 0xB8, 0x8A)
#define COL_ACCENT RGB565(0x39, 0xFF, 0x70)
#define COL_SEL    RGB565(0x0B, 0x5C, 0x2A)
#define COL_OK     RGB565(0x65, 0xFF, 0x8A)
#define COL_WARN   RGB565(0xC8, 0xF5, 0x4B)
#define COL_ERR    RGB565(0xFF, 0x4A, 0x4A)

/* ---- geometry ---- */

#define CARD_X 8
#define CARD_Y 38
#define CARD_W 304
#define CARD_H 100

#define BAR_X 18
#define BAR_Y 80
#define BAR_W 284
#define BAR_H 20

#define BTN_Y 160
#define BTN_H 60

#define N_BTNS 6

typedef enum { BS_NORMAL, BS_SELECTED, BS_PRESSED, BS_DISABLED } btn_state_t;

typedef struct {
    int x, w;
    const char *label;
    const char *desc;
    job_t job;
} btn_t;

static const btn_t s_btns[N_BTNS] = {
    { 4,   58, "FND",  "FND: find console",          JOB_NONE   },
    { 64,  58, "DMP",  "DMP: dump NAND (2 reads)",   JOB_DUMP   },
    { 124, 58, "XEL",  "XEL: write RGH3 XeLL",       JOB_XELL   },
    { 184, 58, "BLD",  "BLD: build image",           JOB_BUILD  },
    { 4,   72, "KEY",  "KEY: grab CPU key (WiFi)",   JOB_GETKEY },
    { 244, 72, "FLSH", "FLSH: flash + verify",       JOB_FLASH  },
};

static int s_sel = 0;
static bool s_console_ready = false;   /* FND succeeded (console linked)  */
static bool s_have_dump = false;       /* a verified dump is on the card  */
static bool s_flashable = false;       /* a verified image is ready       */

/*
 * Progressive unlock: each step opens only once the previous one is complete.
 *   FND  : always available
 *   DMP  : after FND detects a console
 *   BLD  : after a verified dump exists
 *   FLSH : after a verified image is built
 */
static bool btn_enabled(int i)
{
    switch (i) {
        case 0: return true;                                /* FND  */
        case 1: return s_console_ready;                     /* DMP  */
        case 2: return s_console_ready && s_have_dump;      /* XEL  */
        case 3: return s_console_ready && s_have_dump;      /* BLD  */
        case 4: return true;                                /* KEY: WiFi, console in XeLL */
        case 5: return s_console_ready && s_flashable;      /* FLSH */
    }
    return false;
}

/* ---- small drawing helpers ---- */

static uint16_t mix(uint16_t a, uint16_t b, int t)
{
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;

    int r = ar + (br - ar) * t / 255;
    int g = ag + (bg - ag) * t / 255;
    int l = ab + (bb - ab) * t / 255;

    return (uint16_t)((r << 11) | (g << 5) | l);
}

/* filled rectangle with 2px rounded corners */

static void rrect(int x, int y, int w, int h, uint16_t c)
{
    lcd_fill_rect(x + 2, y,         w - 4, 1, c);
    lcd_fill_rect(x + 1, y + 1,     w - 2, 1, c);
    lcd_fill_rect(x,     y + 2,     w,     h - 4, c);
    lcd_fill_rect(x + 1, y + h - 2, w - 2, 1, c);
    lcd_fill_rect(x + 2, y + h - 1, w - 4, 1, c);
}

/* rounded panel */

static void panel(int x, int y, int w, int h,
                  uint16_t fill, uint16_t edge)
{
    rrect(x + 1, y + 1, w - 2, h - 2, fill);

    lcd_fill_rect(x + 2, y,         w - 4, 1, edge);
    lcd_fill_rect(x + 2, y + h - 1, w - 4, 1, edge);
    lcd_fill_rect(x,     y + 2,     1, h - 4, edge);
    lcd_fill_rect(x + w - 1, y + 2, 1, h - 4, edge);

    lcd_fill_rect(x + 1,     y + 1,     1, 1, edge);
    lcd_fill_rect(x + w - 2, y + 1,     1, 1, edge);
    lcd_fill_rect(x + 1,     y + h - 2, 1, 1, edge);
    lcd_fill_rect(x + w - 2, y + h - 2, 1, 1, edge);
}

/* centered text */

static void text_c(int left, int width, int y, const char *s,
                   uint16_t fg, uint16_t bg, int scale)
{
    int tw = (int)strlen(s) * 8 * scale;

    if (tw > width && scale > 1) {
        scale = 1;
        tw = (int)strlen(s) * 8;
    }

    int x = left + (width - tw) / 2;

    if (x < left) x = left;

    lcd_text(x, y, s, fg, bg, scale);
}

/* ---- buttons ---- */

static void draw_button(int i, btn_state_t state)
{
    const btn_t *b = &s_btns[i];

    bool disabled = !btn_enabled(i);

    uint16_t fill = disabled ? RGB565(0x18, 0x20, 0x1A)
                  : (state == BS_PRESSED) ? COL_OK
                  : (state == BS_SELECTED) ? COL_SEL
                  : COL_PANEL;

    uint16_t edge = disabled ? RGB565(0x35, 0x3B, 0x36)
                  : (state == BS_NORMAL) ? COL_EDGE : COL_ACCENT;

    uint16_t label_col = disabled ? RGB565(0x70, 0x78, 0x72) : C_WHITE;

    panel(b->x, BTN_Y, b->w, BTN_H, fill, edge);

    if (!disabled && state != BS_NORMAL) {
        lcd_rect(b->x + 2, BTN_Y + 2,
                 b->w - 4, BTN_H - 4,
                 mix(COL_ACCENT, fill, 150));
    }

    /* Main label only — disabled actions are visibly greyed out. */
    text_c(b->x, b->w, BTN_Y + 20,
           b->label, label_col, fill, 2);
}

static void draw_all_buttons(void)
{
    for (int i = 0; i < N_BTNS; i++) {
        draw_button(i, i == s_sel ? BS_SELECTED : BS_NORMAL);
    }
}

static void draw_desc(void)
{
    lcd_fill_rect(0, 146, LCD_W, 10, COL_BG);

    text_c(0, LCD_W, 147,
           s_btns[s_sel].desc,
           mix(COL_ACCENT, C_WHITE, 90),
           COL_BG, 1);
}

/* ---- progress bar ---- */

static int s_bar_px = 0;

static void bar_track(void)
{
    lcd_rect(BAR_X, BAR_Y, BAR_W, BAR_H, COL_EDGE);

    lcd_fill_rect(BAR_X + 1, BAR_Y + 1,
                  BAR_W - 2, BAR_H - 2, COL_TRACK);

    s_bar_px = 0;
}

static void bar_segment(int px0, int w, uint16_t col)
{
    int inner_h = BAR_H - 4;
    int top_h = inner_h * 4 / 10;

    lcd_fill_rect(BAR_X + 2 + px0, BAR_Y + 2,
                  w, top_h, mix(col, C_WHITE, 70));

    lcd_fill_rect(BAR_X + 2 + px0, BAR_Y + 2 + top_h,
                  w, inner_h - top_h, col);
}

static void draw_bar(int pct, uint16_t col, bool full)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    int px = (BAR_W - 4) * pct / 100;

    if (full || px < s_bar_px) {
        lcd_fill_rect(BAR_X + 1, BAR_Y + 1,
                      BAR_W - 2, BAR_H - 2, COL_TRACK);

        s_bar_px = 0;
    }

    if (px > s_bar_px) {
        bar_segment(s_bar_px, px - s_bar_px, col);
        s_bar_px = px;
    }
}

/* ---- static frame ---- */

static char s_last_stage[48];
static char s_last_info[48];
static char s_last_hint[32];

static uint16_t s_last_scol;
static uint16_t s_last_bar_col;

static int s_last_pct;
static bool s_last_linked;

static void draw_static(void)
{
    lcd_fill(COL_BG);

    /* Header */
    lcd_fill_rect(0, 0, LCD_W, 28, COL_HEAD);
    lcd_fill_rect(0, 28, LCD_W, 2, COL_ACCENT);

    text_c(0, LCD_W, 6,
           "Trinity Flasher", C_WHITE, COL_HEAD, 2);

    /* Status card */
    panel(CARD_X, CARD_Y, CARD_W, CARD_H, COL_PANEL, COL_EDGE);

    bar_track();

    draw_all_buttons();
    draw_desc();

    text_c(0, LCD_W, 228,
           "SELECT: run    RIGHT: next",
           COL_DIM, COL_BG, 1);

    /* Force next status update to repaint */
    s_last_stage[0] = '\0';
    s_last_info[0] = '\0';
    s_last_hint[0] = '\0';

    s_last_pct = -1;
    s_last_scol = 0;
    s_last_bar_col = 0;

    rrect(12, 9, 12, 12,
          mix(COL_HEAD, C_WHITE, 70));

    s_last_linked = false;
}

/* ---- status card ---- */

static void draw_status(const job_status_t *s)
{
    if (s->linked != s_console_ready ||
        s->have_dump != s_have_dump ||
        s->flashable != s_flashable) {

        /* Auto-advance the highlight to each newly unlocked step. */
        if (s->linked && !s_console_ready) {
            s_sel = 1;                                   /* FND done -> DMP  */
        } else if (s->linked && s->have_dump && !s_have_dump) {
            s_sel = 2;                                   /* DMP done -> XEL  */
        } else if (s->linked && s->flashable && !s_flashable) {
            s_sel = 5;                                   /* BLD done -> FLSH */
        }

        s_console_ready = s->linked;
        s_have_dump = s->have_dump;
        s_flashable = s->flashable;

        if (!btn_enabled(s_sel)) s_sel = 0;  /* never sit on a locked step */

        draw_all_buttons();
        draw_desc();
    }

    uint16_t col = s->done
                 ? (s->ok ? COL_OK : COL_ERR)
                 : COL_WARN;

    bool idle = (!s->done && strcmp(s->stage, "idle") == 0);

    uint16_t scol = idle ? COL_DIM : col;

    /* Console-link indicator */
    if (s->linked != s_last_linked) {
        rrect(12, 9, 12, 12,
              s->linked ? COL_OK : mix(COL_HEAD, C_WHITE, 70));

        s_last_linked = s->linked;
    }

    /* Stage */
    if (strcmp(s->stage, s_last_stage) != 0 ||
        scol != s_last_scol) {

        lcd_fill_rect(CARD_X + 2, 44,
                      CARD_W - 4, 16, COL_PANEL);

        text_c(CARD_X, CARD_W, 44,
               s->stage, scol, COL_PANEL, 2);

        snprintf(s_last_stage, sizeof(s_last_stage),
                 "%s", s->stage);

        s_last_scol = scol;
    }

    /* Console information */
    char info[48];

    if (s->linked) {
        snprintf(info, sizeof(info),
                 "cfg 0x%08lX   dump: %s",
                 (unsigned long)s->flash_config,
                 s->have_dump ? "yes" : "no");
    } else {
        snprintf(info, sizeof(info),
                 "console: not detected");
    }

    if (strcmp(info, s_last_info) != 0) {
        lcd_fill_rect(CARD_X + 2, 64,
                      CARD_W - 4, 8, COL_PANEL);

        text_c(CARD_X, CARD_W, 64,
               info, COL_DIM, COL_PANEL, 1);

        snprintf(s_last_info, sizeof(s_last_info),
                 "%s", info);
    }

    /* Progress bar */
    draw_bar(s->percent, col, col != s_last_bar_col);

    s_last_bar_col = col;

    /* Percentage */
    if (s->percent != s_last_pct) {
        char pc[8];

        snprintf(pc, sizeof(pc), "%d%%", s->percent);

        lcd_fill_rect(CARD_X + 2, 106,
                      CARD_W - 4, 8, COL_PANEL);

        text_c(CARD_X, CARD_W, 106,
               pc, C_WHITE, COL_PANEL, 1);

        s_last_pct = s->percent;
    }

    /* Status hint */
    const char *hint = s->flashable
                     ? "image ready - select FLSH"
                     : (s->have_dump
                        ? "dump saved - XEL, then BLD"
                        : "select FND, then DMP");

    if (strcmp(hint, s_last_hint) != 0) {
        lcd_fill_rect(CARD_X + 2, 120,
                      CARD_W - 4, 8, COL_PANEL);

        text_c(CARD_X, CARD_W, 120,
               hint, COL_DIM, COL_PANEL, 1);

        snprintf(s_last_hint, sizeof(s_last_hint),
                 "%s", hint);
    }

}

/* ---- push buttons ---- */

typedef struct {
    int pin;
    int prev;
} pushbtn_t;

static pushbtn_t s_right = {
    PIN_BTN_RIGHT, 1
};

static pushbtn_t s_select = {
    PIN_BTN_SELECT, 1
};

static void buttons_init(void)
{
    gpio_config_t in = {
        .pin_bit_mask =
            (1ULL << PIN_BTN_RIGHT) |
            (1ULL << PIN_BTN_SELECT),

        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config(&in);
}

/* Debounced button press detection */

static bool pushed(pushbtn_t *b)
{
    int lvl = gpio_get_level(b->pin);

    bool ev = false;

    if (lvl == 0 && b->prev == 1) {
        vTaskDelay(pdMS_TO_TICKS(15));

        ev = (gpio_get_level(b->pin) == 0);
    }

    b->prev = lvl;

    return ev;
}

/* ---- navigation ---- */

static void move_right(void)
{
    int old = s_sel;

    /* Advance to the next unlocked step, wrapping; skip locked ones. */
    for (int k = 1; k <= N_BTNS; k++) {
        int cand = (s_sel + k) % N_BTNS;
        if (btn_enabled(cand)) {
            if (cand != s_sel) {
                s_sel = cand;
                draw_button(old, BS_NORMAL);
                draw_button(s_sel, BS_SELECTED);
                draw_desc();
            }
            return;
        }
    }
    /* nothing else unlocked yet: stay put */
}

/* ---- execute selected action ---- */

static void run_selected(void)
{
    if (!btn_enabled(s_sel)) return;      /* locked step: ignore */

    const btn_t *b = &s_btns[s_sel];

    draw_button(s_sel, BS_PRESSED);

    vTaskDelay(pdMS_TO_TICKS(150));

    draw_button(s_sel, BS_SELECTED);

    if (b->job == JOB_NONE) {
        jobs_detect();
    } else {
        job_status_t current;
        jobs_get(&current);

        if (!current.linked) {
            /* Safety: never start DMP/BLD/FLSH before successful FND. */
            return;
        }

        if (b->job == JOB_BUILD) {
            jobs_load_cpukey_from_sd();
        }

        jobs_start(b->job);
    }
}

/* ---- main UI task ---- */

static void ui_task(void *arg)
{
    buttons_init();

    draw_static();

    job_status_t st;

    jobs_get(&st);

    draw_status(&st);

    uint32_t tick = 0;

    for (;;) {
        if ((tick++ % 5) == 0) {
            jobs_get(&st);

            draw_status(&st);
        }

        if (pushed(&s_right)) {
            move_right();
        }

        if (pushed(&s_select)) {
            jobs_get(&st);

            if (!st.busy) {
                run_selected();

                jobs_get(&st);

                draw_status(&st);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void ui_start(void)
{
    xTaskCreate(ui_task, "ui", 6144, NULL, 4, NULL);
}
