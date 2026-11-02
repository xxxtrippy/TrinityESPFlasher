/*
 * ui.c — standalone on-screen UI for TrinityWeb360, driven by two buttons.
 *
 * GPL-3.0. See LICENSE.
 *
 * Layout (320x240 landscape):
 *   title bar
 *   status line (current stage / result)
 *   console info line (flash config)
 *   progress bar + percent
 *   hint line (next step)
 *   description of the highlighted button
 *   4 buttons: FND | DMP | BLD | FLSH   (one is highlighted)
 *
 * Controls (see pins.h): RIGHT cycles the highlight and wraps from FLSH back
 * to FND; SELECT runs the highlighted action. No touch screen, WiFi, or PC is
 * needed. The CPU key for Build is read from /sd/cpukey.txt.
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

/* lcd.h has no maroon; define it here if it isn't provided (RGB565). */
#ifndef C_MAROON
#define C_MAROON 0x8000
#endif

#define BG       C_BLACK
#define TITLE_BG C_MAROON
#define BTN_BG   C_DGRAY
#define BTN_HI   C_BLUE

#define N_BTNS 4

typedef enum { BS_NORMAL, BS_SELECTED, BS_PRESSED } btn_state_t;

typedef struct {
    int x, y, w, h;
    const char *label;
    const char *desc;
    job_t job;
} btn_t;

/* bottom row of 4 buttons */
static const btn_t s_btns[N_BTNS] = {
    { 4,   188, 74, 46, "FND",  "FND: find console",          JOB_NONE  },
    { 84,  188, 74, 46, "DMP",  "DMP: dump NAND (3 reads)",   JOB_DUMP  },
    { 164, 188, 74, 46, "BLD",  "BLD: build image",           JOB_BUILD },
    { 244, 188, 72, 46, "FLSH", "FLSH: flash + verify",       JOB_FLASH },
};

static int s_sel = 0; /* highlighted button */

/* scale 1 = 8px/char, scale 2 = 16px/char */
static void text_center(int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    int tw = (int)strlen(s) * 8 * scale;
    int x = (LCD_W - tw) / 2;
    if (x < 0) x = 0;               /* too long: pin to left edge */
    lcd_text(x, y, s, fg, bg, scale);
}

static void draw_button(const btn_t *b, btn_state_t state)
{
    uint16_t fill = (state == BS_PRESSED)  ? C_GREEN
                  : (state == BS_SELECTED) ? BTN_HI
                  :                          BTN_BG;
    lcd_fill_rect(b->x, b->y, b->w, b->h, fill);
    lcd_rect(b->x, b->y, b->w, b->h, state == BS_NORMAL ? C_GRAY : C_WHITE);
    if (state != BS_NORMAL) {       /* double border = highlighted */
        lcd_rect(b->x + 1, b->y + 1, b->w - 2, b->h - 2, C_WHITE);
    }
    int tw = (int)strlen(b->label) * 16; /* scale 2 -> 16px/char */
    int tx = b->x + (b->w - tw) / 2;
    int ty = b->y + (b->h - 16) / 2;
    lcd_text(tx, ty, b->label, C_WHITE, fill, 2);
}

static void draw_desc(void)
{
    lcd_fill_rect(0, 160, LCD_W, 12, BG);
    text_center(162, s_btns[s_sel].desc, C_WHITE, BG, 1);
}

static void draw_all_buttons(void)
{
    for (int i = 0; i < N_BTNS; i++) {
        draw_button(&s_btns[i], i == s_sel ? BS_SELECTED : BS_NORMAL);
    }
}

static void draw_static(void)
{
    lcd_fill(BG);
    lcd_fill_rect(0, 0, LCD_W, 28, TITLE_BG);
    text_center(6, "Trinity Flasher", C_WHITE, TITLE_BG, 2);
    draw_all_buttons();
    draw_desc();
}

static void draw_status(const job_status_t *s)
{
    uint16_t col = s->done ? (s->ok ? C_GREEN : C_RED) : C_AMBER;

    /* status line (centered) */
    lcd_fill_rect(0, 34, LCD_W, 20, BG);
    text_center(36, s->stage, col, BG, 2);

    /* console info (centered) */
    lcd_fill_rect(0, 62, LCD_W, 18, BG);
    char line[48];
    if (s->linked) {
        snprintf(line, sizeof(line), "cfg 0x%08lX  dump:%s",
                 (unsigned long)s->flash_config, s->have_dump ? "yes" : "no");
    } else {
        strcpy(line, "console: not detected");
    }
    text_center(62, line, C_GRAY, BG, 1);

    /* progress bar */
    int bx = 8, by = 92, bw = LCD_W - 16, bh = 26;
    lcd_rect(bx, by, bw, bh, C_GRAY);
    int fillw = (bw - 4) * s->percent / 100;
    lcd_fill_rect(bx + 2, by + 2, bw - 4, bh - 4, BG);
    if (fillw > 0) lcd_fill_rect(bx + 2, by + 2, fillw, bh - 4, col);

    /* percent label, below the bar, centered, solid background */
    char pc[8];
    snprintf(pc, sizeof(pc), "%d%%", s->percent);
    lcd_fill_rect(0, 120, LCD_W, 10, BG);
    text_center(121, pc, C_WHITE, BG, 1);

    /* hint line (centered) */
    lcd_fill_rect(0, 134, LCD_W, 16, BG);
    const char *hint = s->flashable ? "img ready - select FLSH"
                       : (s->have_dump ? "DMP saved - select BLD" : "select FND, then DMP");
    text_center(136, hint, C_GRAY, BG, 1);
}

/* ---- push buttons (active low, internal pull-up) ---- */

typedef struct { int pin; int prev; } pushbtn_t;

static pushbtn_t s_right  = { PIN_BTN_RIGHT,  1 };
static pushbtn_t s_select = { PIN_BTN_SELECT, 1 };

static void buttons_init(void)
{
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_BTN_RIGHT) | (1ULL << PIN_BTN_SELECT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);
}

/* True once per press (falling edge, debounced). Holding does not repeat. */
static bool pushed(pushbtn_t *b)
{
    int lvl = gpio_get_level(b->pin);
    bool ev = false;
    if (lvl == 0 && b->prev == 1) {
        vTaskDelay(pdMS_TO_TICKS(15));          /* debounce */
        ev = (gpio_get_level(b->pin) == 0);
    }
    b->prev = lvl;
    return ev;
}

static void move_right(void)
{
    int old = s_sel;
    s_sel = (s_sel + 1) % N_BTNS;               /* FLSH wraps back to FND */
    draw_button(&s_btns[old], BS_NORMAL);
    draw_button(&s_btns[s_sel], BS_SELECTED);
    draw_desc();
}

static void run_selected(void)
{
    const btn_t *b = &s_btns[s_sel];

    draw_button(b, BS_PRESSED);
    vTaskDelay(pdMS_TO_TICKS(150));
    draw_button(b, BS_SELECTED);

    if (b->job == JOB_NONE) {
        jobs_detect();
    } else {
        if (b->job == JOB_BUILD) {
            jobs_load_cpukey_from_sd(); /* best-effort */
        }
        jobs_start(b->job);
    }
}

static void ui_task(void *arg)
{
    buttons_init();
    draw_static();

    job_status_t st;
    jobs_get(&st);
    draw_status(&st);

    int last_pct = -1;
    bool last_done = false, last_linked = false, last_dump = false;
    char last_stage[32] = "";
    uint32_t tick = 0;

    for (;;) {
        /* refresh status when it changes (every ~100 ms) */
        if ((tick++ % 5) == 0) {
            jobs_get(&st);
            if (st.percent != last_pct || st.done != last_done ||
                st.linked != last_linked || st.have_dump != last_dump ||
                strncmp(st.stage, last_stage, sizeof(last_stage)) != 0) {
                draw_status(&st);
                last_pct = st.percent;
                last_done = st.done;
                last_linked = st.linked;
                last_dump = st.have_dump;
                strncpy(last_stage, st.stage, sizeof(last_stage) - 1);
                last_stage[sizeof(last_stage) - 1] = '\0';
            }
        }

        if (pushed(&s_right)) {
            move_right();
        }

        if (pushed(&s_select)) {
            jobs_get(&st);
            if (!st.busy) {             /* ignore SELECT while a job runs */
                run_selected();
                last_pct = -1;          /* force status redraw */
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void ui_start(void)
{
    xTaskCreate(ui_task, "ui", 6144, NULL, 4, NULL);
}
