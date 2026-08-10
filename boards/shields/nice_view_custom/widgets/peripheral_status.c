/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Animated peripheral screen:
 *   top    : battery + link status
 *   middle : atom "Y" — pulsing nucleus, electrons with trails on three
 *            tilted elliptical orbits, twinkling stars in the background
 *   bottom : twinkling sky with shooting stars, and a segmented equalizer
 *            with peak-hold markers that reacts to this half's keypresses
 *
 */

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/battery.h>
#include <zmk/display.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/usb.h>
#include <zmk/ble.h>

#include "peripheral_status.h"

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct peripheral_status_state {
    bool connected;
};

#define EQ_BARS 12
#define EQ_MAX 16

static uint32_t anim_frame = 0;
static uint32_t key_count = 0;
static uint8_t eq_levels[EQ_BARS];
static struct k_work_delayable anim_work;

#define ATOM_CX 34
#define ATOM_CY 34
#define ATOM_A 30 // ellipse semi-major axis
#define ATOM_B 11 // ellipse semi-minor axis
#define ORBIT_STEPS 36
#define TRAIL_LEN 5

// cos/sin * 100 for 36 steps of 10 degrees
static const int16_t TRIG[ORBIT_STEPS][2] = {
    {100, 0},   {98, 17},   {94, 34},   {87, 50},   {77, 64},   {64, 77},
    {50, 87},   {34, 94},   {17, 98},   {0, 100},   {-17, 98},  {-34, 94},
    {-50, 87},  {-64, 77},  {-77, 64},  {-87, 50},  {-94, 34},  {-98, 17},
    {-100, 0},  {-98, -17}, {-94, -34}, {-87, -50}, {-77, -64}, {-64, -77},
    {-50, -87}, {-34, -94}, {-17, -98}, {0, -100},  {17, -98},  {34, -94},
    {50, -87},  {64, -77},  {77, -64},  {87, -50},  {94, -34},  {98, -17},
};

// {cos, sin} * 100 of the three orbit tilts (30, 90, 150 degrees)
static const int16_t ORB[3][2] = {{87, 50}, {0, 100}, {-87, 50}};

// background stars (x, y, phase)
static const uint8_t STARS[7][3] = {
    {6, 6, 0}, {60, 10, 3}, {10, 56, 6}, {62, 60, 1}, {34, 4, 4}, {4, 34, 2}, {63, 36, 5},
};

static void ellipse_point(int orbit, int t, lv_point_t *p) {
    int ex = ATOM_A * TRIG[t][0] / 100;
    int ey = ATOM_B * TRIG[t][1] / 100;
    p->x = ATOM_CX + (ex * ORB[orbit][0] - ey * ORB[orbit][1]) / 100;
    p->y = ATOM_CY + (ex * ORB[orbit][1] + ey * ORB[orbit][0]) / 100;
}

static void draw_atom(lv_obj_t *widget, lv_color_t cbuf[], uint32_t frame) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 1);

    lv_draw_rect_dsc_t rect_bg;
    init_rect_dsc(&rect_bg, LVGL_BACKGROUND);
    lv_draw_rect_dsc_t rect_fg;
    init_rect_dsc(&rect_fg, LVGL_FOREGROUND);
    lv_draw_line_dsc_t line_dsc;
    init_line_dsc(&line_dsc, LVGL_FOREGROUND, 1);
    lv_draw_arc_dsc_t nucleus_dsc;
    init_arc_dsc(&nucleus_dsc, LVGL_FOREGROUND, 1);
    lv_draw_label_dsc_t y_dsc;
    init_label_dsc(&y_dsc, LVGL_FOREGROUND, &lv_font_montserrat_20, LV_TEXT_ALIGN_CENTER);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_bg);

    // twinkling stars: each blinks on/off with its own phase
    for (int s = 0; s < 7; s++) {
        if (((frame / 4) + STARS[s][2]) % 3 != 0) {
            lv_canvas_draw_rect(canvas, STARS[s][0], STARS[s][1], 1, 1, &rect_fg);
        }
    }

    // orbit paths
    for (int o = 0; o < 3; o++) {
        lv_point_t pts[ORBIT_STEPS + 1];
        for (int t = 0; t <= ORBIT_STEPS; t++) {
            ellipse_point(o, t % ORBIT_STEPS, &pts[t]);
        }
        lv_canvas_draw_line(canvas, pts, ORBIT_STEPS + 1, &line_dsc);
    }

    // electrons with fading trails (electrons run at different speeds/directions)
    for (int o = 0; o < 3; o++) {
        int speed = (o == 1) ? 3 : 2;
        int head = (o == 2) ? (ORBIT_STEPS - (int)((frame * speed) % ORBIT_STEPS)) % ORBIT_STEPS
                            : (frame * speed + o * 12) % ORBIT_STEPS;

        for (int k = TRAIL_LEN - 1; k >= 0; k--) {
            int t = (o == 2) ? (head + k) % ORBIT_STEPS
                             : (head - k + ORBIT_STEPS) % ORBIT_STEPS;
            lv_point_t e;
            ellipse_point(o, t, &e);
            if (k == 0) {
                lv_canvas_draw_rect(canvas, e.x - 1, e.y - 1, 3, 3, &rect_fg); // head
                // occasional sparkle: a small 4-point flash around the electron
                if (((frame + o * 3) % 10) < 2) {
                    lv_canvas_draw_rect(canvas, e.x - 3, e.y, 1, 1, &rect_fg);
                    lv_canvas_draw_rect(canvas, e.x + 3, e.y, 1, 1, &rect_fg);
                    lv_canvas_draw_rect(canvas, e.x, e.y - 3, 1, 1, &rect_fg);
                    lv_canvas_draw_rect(canvas, e.x, e.y + 3, 1, 1, &rect_fg);
                }
            } else if (k < 3) {
                lv_canvas_draw_rect(canvas, e.x, e.y, 2, 2, &rect_fg); // near trail
            } else {
                lv_canvas_draw_rect(canvas, e.x, e.y, 1, 1, &rect_fg); // far trail
            }
        }
    }

    // expanding ripple ring emanating from the nucleus every ~4 s
    {
        uint32_t rp = frame % 40;
        if (rp < 14) {
            int rr = 14 + (int)rp;
            int gap = 24 + (int)rp * 3; // ring dissolves as it expands
            for (int j = 0; j < 6; j++) {
                lv_canvas_draw_arc(canvas, ATOM_CX, ATOM_CY, rr, 60 * j + gap / 2,
                                   60 * (j + 1) - gap / 2, &nucleus_dsc);
            }
        }
    }

    // pulsing nucleus ring around the "Y" (radius breathes 11 -> 13)
    int pulse = (frame / 3) % 4;
    int r = 11 + (pulse < 2 ? pulse : 4 - pulse);
    lv_canvas_draw_arc(canvas, ATOM_CX, ATOM_CY, r, 0, 360, &nucleus_dsc);
    lv_canvas_draw_text(canvas, 0, ATOM_CY - 10, CANVAS_SIZE, &y_dsc, "Y");

    rotate_canvas(canvas, cbuf);
}

// Top: small battery icon + small percentage right of it, link symbol at the right.
static void draw_top(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 0);

    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_RIGHT);
    lv_draw_label_dsc_t pct_dsc;
    init_label_dsc(&pct_dsc, LVGL_FOREGROUND, &lv_font_unscii_8, LV_TEXT_ALIGN_LEFT);
    lv_draw_rect_dsc_t rect_bg;
    init_rect_dsc(&rect_bg, LVGL_BACKGROUND);
    lv_draw_rect_dsc_t rect_fg;
    init_rect_dsc(&rect_fg, LVGL_FOREGROUND);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_bg);

    // Small battery icon (16x9 + nub) with proportional fill
    lv_canvas_draw_rect(canvas, 0, 4, 16, 9, &rect_fg);
    lv_canvas_draw_rect(canvas, 1, 5, 14, 7, &rect_bg);
    lv_canvas_draw_rect(canvas, 2, 6, (state->battery * 12 + 50) / 100, 5, &rect_fg);
    lv_canvas_draw_rect(canvas, 16, 6, 2, 5, &rect_fg);

    if (state->charging) {
        // tiny lightning zigzag over the battery
        lv_canvas_draw_rect(canvas, 8, 5, 1, 2, &rect_bg);
        lv_canvas_draw_rect(canvas, 6, 7, 4, 1, &rect_bg);
        lv_canvas_draw_rect(canvas, 8, 8, 1, 2, &rect_bg);
        lv_canvas_draw_rect(canvas, 7, 5, 1, 3, &rect_fg);
        lv_canvas_draw_rect(canvas, 8, 7, 1, 3, &rect_fg);
    }

    // Battery percentage, small, right of the icon
    char pct[6] = {};
    snprintf(pct, sizeof(pct), "%d%%", state->battery);
    lv_canvas_draw_text(canvas, 20, 5, 30, &pct_dsc, pct);

    // Link status with the central half
    lv_canvas_draw_text(canvas, 0, 0, CANVAS_SIZE, &label_dsc,
                        state->connected ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE);

    rotate_canvas(canvas, cbuf);
}

// fixed twinkle stars in the sky zone (x, y, phase)
static const uint8_t BSTARS[6][3] = {
    {6, 4, 1}, {20, 16, 4}, {34, 6, 0}, {50, 12, 3}, {62, 20, 5}, {12, 24, 2},
};

// Bottom: twinkling sky with shooting stars (top), segmented equalizer (bottom).
static void draw_bottom(lv_obj_t *widget, lv_color_t cbuf[], uint32_t frame) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 2);

    lv_draw_rect_dsc_t rect_bg;
    init_rect_dsc(&rect_bg, LVGL_BACKGROUND);
    lv_draw_rect_dsc_t rect_fg;
    init_rect_dsc(&rect_fg, LVGL_FOREGROUND);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_bg);

    // ---- twinkle stars across the full sky
    for (int s = 0; s < 6; s++) {
        if (((frame / 4) + BSTARS[s][2]) % 3 != 0) {
            lv_canvas_draw_rect(canvas, BSTARS[s][0], BSTARS[s][1], 1, 1, &rect_fg);
        }
    }

    // ---- a shooting star streaks across the sky every ~5 s
    uint32_t cycle = frame / 50;
    uint32_t p = frame % 50;
    if (p < 12) {
        int sx = 2 + (int)(cycle * 37 % 42);
        int sy = 2 + (int)(cycle * 23 % 14);
        for (int k = 0; k < 4; k++) { // head + fading tail
            int tx = sx + (int)p * 4 - k * 4;
            int ty = sy + (int)p * 2 - k * 2;
            if (tx < 0 || tx > 66 || ty < 0 || ty > 26) {
                continue;
            }
            int sz = (k == 0) ? 2 : 1;
            lv_canvas_draw_rect(canvas, tx, ty, sz, sz, &rect_fg);
        }
    }

    // ---- mirrored equalizer: thin bars grow up AND down from a center line
    const int mid = 46, bw = 3, step = 5;
    lv_canvas_draw_rect(canvas, 4, mid, 60, 1, &rect_fg); // center line, always on
    for (int i = 0; i < EQ_BARS; i++) {
        int x = 5 + i * step;
        int h = eq_levels[i];
        if (h > 0) {
            lv_canvas_draw_rect(canvas, x, mid - h, bw, h, &rect_fg);     // above
            lv_canvas_draw_rect(canvas, x, mid + 1, bw, h, &rect_fg);     // below
        } else {
            lv_canvas_draw_rect(canvas, x, mid - 1, bw, 3, &rect_fg);     // resting tick
        }
    }

    rotate_canvas(canvas, cbuf);
}

static void anim_tick(struct k_work *work) {
    anim_frame++;

    // equalizer decay
    for (int i = 0; i < EQ_BARS; i++) {
        eq_levels[i] = eq_levels[i] > 2 ? eq_levels[i] - 2 : 0;
    }

    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        draw_atom(widget->obj, widget->cbuf2, anim_frame);
        draw_bottom(widget->obj, widget->cbuf3, anim_frame);
    }
    k_work_schedule(&anim_work, K_MSEC(100));
}

// bump equalizer bars on this half's own key presses
static int keystroke_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev != NULL && ev->state) {
        key_count++;
        int bar = (int)((ev->position * 7 + key_count) % EQ_BARS);
        int left = (bar + EQ_BARS - 1) % EQ_BARS;
        int right = (bar + 1) % EQ_BARS;
        eq_levels[bar] = EQ_MAX;
        eq_levels[right] = eq_levels[right] > 10 ? eq_levels[right] : 10;
        eq_levels[left] = eq_levels[left] > 7 ? eq_levels[left] : 7;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(widget_keystroke_count, keystroke_listener)
ZMK_SUBSCRIPTION(widget_keystroke_count, zmk_position_state_changed);

static void set_battery_status(struct zmk_widget_status *widget,
                               struct battery_status_state state) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    widget->state.charging = state.usb_present;
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

    widget->state.battery = state.level;

    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void battery_status_update_cb(struct battery_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_status(widget, state); }
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    return (struct battery_status_state){
        .level = zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state)

ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */

static struct peripheral_status_state get_state(const zmk_event_t *_eh) {
    return (struct peripheral_status_state){.connected = zmk_split_bt_peripheral_is_connected()};
}

static void set_connection_status(struct zmk_widget_status *widget,
                                  struct peripheral_status_state state) {
    widget->state.connected = state.connected;

    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void output_status_update_cb(struct peripheral_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_connection_status(widget, state); }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_peripheral_status, struct peripheral_status_state,
                            output_status_update_cb, get_state)
ZMK_SUBSCRIPTION(widget_peripheral_status, zmk_split_peripheral_status_changed);

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 160, 68);

    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_obj_align(top, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_canvas_set_buffer(top, widget->cbuf, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);
    lv_obj_t *middle = lv_canvas_create(widget->obj);
    lv_obj_align(middle, LV_ALIGN_TOP_LEFT, 24, 0);
    lv_canvas_set_buffer(middle, widget->cbuf2, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);
    lv_obj_t *bottom = lv_canvas_create(widget->obj);
    lv_obj_align(bottom, LV_ALIGN_TOP_LEFT, -44, 0);
    lv_canvas_set_buffer(bottom, widget->cbuf3, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    sys_slist_append(&widgets, &widget->node);
    widget_battery_status_init();
    widget_peripheral_status_init();

    k_work_init_delayable(&anim_work, anim_tick);
    k_work_schedule(&anim_work, K_MSEC(100));

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }
