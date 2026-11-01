/*
 * ui.c — standalone on-screen touch UI for TrinityWeb360.
 *
 * GPL-3.0. See LICENSE.
 *
 * Layout (320x240 landscape):
 *   title bar
 *   status line (current stage / result)
 *   console info line (flash config)
 *   progress bar
 *   4 buttons: Detect | Dump | Build | Flash
 *
 * No WiFi, no web, no PC: the screen is the whole interface. The CPU key for
 * Build is read from /sd/cpukey.txt.
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lcd.h"
#include "touch.h"
#include "jobs.h"
#include "sdcard.h"

#define BG       C_BLACK
#define TITLE_BG C_PURPLE
#define BTN_BG   C_DGRAY
#define BTN_HI   C_BLUE

typedef struct { int x, y, w, h; const char *label; job_t job; } btn_t;

/* bottom row of 4 buttons */
static btn_t s_btns[4] = {
    { 4,   188, 74, 46, "Detect", JOB_NONE  },
    { 84,  188, 74, 46, "Dump",   JOB_DUMP  },
    { 164, 188, 74, 46, "Build",  JOB_BUILD },
    { 244, 188, 72, 46, "Flash",  JOB_FLASH },
};

static void draw_button(const btn_t *b, bool pressed)
{
    uint16_t fill = pressed ? BTN_HI : BTN_BG;
    lcd_fill_rect(b->x, b->y, b->w, b->h, fill);
    lcd_rect(b->x, b->y, b->w, b->h, C_GRAY);
    int tw = (int)strlen(b->label) * 16; /* scale 2 -> 16px/char */
    int tx = b->x + (b->w - tw) / 2;
    int ty = b->y + (b->h - 16) / 2;
    lcd_text(tx, ty, b->label, C_WHITE, fill, 2);
}

static void draw_static(void)
{
    lcd_fill(BG);
    lcd_fill_rect(0, 0, LCD_W, 28, TITLE_BG);
    lcd_text(8, 6, "Trinity Flasher", C_WHITE, TITLE_BG, 2);
    for (int i = 0; i < 4; i++) draw_button(&s_btns[i], false);
}

static void draw_status(const job_status_t *s)
{
    /* status line */
    lcd_fill_rect(0, 34, LCD_W, 20, BG);
    uint16_t col = s->done ? (s->ok ? C_GREEN : C_RED) : C_AMBER;
    lcd_text(8, 36, s->stage, col, BG, 2);

    /* console info */
    lcd_fill_rect(0, 62, LCD_W, 18, BG);
    char line[48];
    if (s->linked) {
        snprintf(line, sizeof(line), "cfg 0x%08lX  dump:%s",
                 (unsigned long)s->flash_config, s->have_dump ? "yes" : "no");
    } else {
        strcpy(line, "console: not detected");
    }
    lcd_text(8, 62, line, C_GRAY, BG, 1);

    /* progress bar */
    int bx = 8, by = 92, bw = LCD_W - 16, bh = 26;
    lcd_rect(bx, by, bw, bh, C_GRAY);
    int fillw = (bw - 4) * s->percent / 100;
    lcd_fill_rect(bx + 2, by + 2, bw - 4, bh - 4, BG);
    if (fillw > 0) lcd_fill_rect(bx + 2, by + 2, fillw, bh - 4, col);
    char pc[8];
    snprintf(pc, sizeof(pc), "%d%%", s->percent);
    lcd_text(bx + bw / 2 - 12, by + 6, pc, C_WHITE, (fillw > bw/2 ? col : BG), 1);

    /* hint line */
    lcd_fill_rect(0, 128, LCD_W, 16, BG);
    const char *hint = s->flashable ? "image ready - tap Flash"
                       : (s->have_dump ? "dump saved - tap Build" : "tap Detect, then Dump");
    lcd_text(8, 128, hint, C_GRAY, BG, 1);
}

static int hit_button(int x, int y)
{
    for (int i = 0; i < 4; i++) {
        btn_t *b = &s_btns[i];
        if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h)
            return i;
    }
    return -1;
}

static void ui_task(void *arg)
{
    draw_static();
    job_status_t st;
    jobs_get(&st);
    draw_status(&st);

    int last_pct = -1;
    bool last_done = false;
    uint32_t touch_lockout = 0;

    for (;;) {
        /* refresh status when it changes */
        jobs_get(&st);
        if (st.percent != last_pct || st.done != last_done) {
            draw_status(&st);
            last_pct = st.percent;
            last_done = st.done;
        }

        /* handle touch (debounced) */
        int tx, ty;
        if (touch_lockout == 0 && touch_get(&tx, &ty)) {
            int b = hit_button(tx, ty);
            if (b >= 0) {
                draw_button(&s_btns[b], true);
                vTaskDelay(pdMS_TO_TICKS(120));
                draw_button(&s_btns[b], false);

                if (s_btns[b].job == JOB_NONE) {
                    jobs_detect();
                } else {
                    if (s_btns[b].job == JOB_BUILD) {
                        jobs_load_cpukey_from_sd(); /* best-effort */
                    }
                    jobs_start(s_btns[b].job);
                }
                touch_lockout = 6; /* ~600ms */
                last_pct = -1;     /* force redraw */
            }
        }
        if (touch_lockout) touch_lockout--;

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void ui_start(void)
{
    xTaskCreate(ui_task, "ui", 6144, NULL, 4, NULL);
}
