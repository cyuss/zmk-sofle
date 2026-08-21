/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Peripheral screen, inspired by GPeye/hammerbeam-slideshow: most of the
 * screen is a slideshow that cycles through a handful of animated 1-bit
 * scenes, with a small battery + link row at the bottom.
 *
 * The art area is one tall 68 x 128 picture spread over two 68x68 canvases.
 * Every canvas is rotated 90 deg when it is blitted, so inside the drawing
 * code a pixel at source row y lands at x = 68 - y on the screen. Working in
 * "art" coordinates (u along the long axis of the screen, v across it) the
 * two canvases simply tile: canvas A holds u 0..67, canvas B holds u 68..127.
 * Each scene is therefore drawn twice, once per canvas with its own offset,
 * and LVGL clips whatever falls outside. u = 0 is the bottom of the picture.
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
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/usb.h>
#include <zmk/ble.h>

#include "peripheral_status.h"

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct peripheral_status_state {
    bool connected;
};

// Canvas children, in creation order. The art canvases come last so they
// paint over the edge of the info canvas they overlap (which only ever draws
// in rows y < 40, far from that overlap).
#define CHILD_INFO 0
#define CHILD_ART_B 1
#define CHILD_ART_A 2

// Art coordinates: u runs along the screen (0 = bottom, next to the info
// row), v runs across it.
#define ART_U 128
#define ART_V 68
#define ART_SPLIT 68

#ifndef CONFIG_NICE_VIEW_SLIDESHOW_SECONDS
#define CONFIG_NICE_VIEW_SLIDESHOW_SECONDS 10
#endif

#define ANIM_MS 100
#define SLIDE_FRAMES (CONFIG_NICE_VIEW_SLIDESHOW_SECONDS * (1000 / ANIM_MS))
#define SCENE_COUNT 5

static uint32_t anim_frame = 0;
static struct k_work_delayable anim_work;

// cos/sin * 100 for 36 steps of 10 degrees
static const int16_t TRIG[36][2] = {
    {100, 0},   {98, 17},   {94, 34},   {87, 50},   {77, 64},   {64, 77},
    {50, 87},   {34, 94},   {17, 98},   {0, 100},   {-17, 98},  {-34, 94},
    {-50, 87},  {-64, 77},  {-77, 64},  {-87, 50},  {-94, 34},  {-98, 17},
    {-100, 0},  {-98, -17}, {-94, -34}, {-87, -50}, {-77, -64}, {-64, -77},
    {-50, -87}, {-34, -94}, {-17, -98}, {0, -100},  {17, -98},  {34, -94},
    {50, -87},  {64, -77},  {77, -64},  {87, -50},  {94, -34},  {98, -17},
};

/* ------------------------------------------------------------------ atom */

#define ATOM_CU 64
#define ATOM_CV 34
#define ATOM_A 52 // semi-major axis, along the long side of the screen
#define ATOM_B 14 // semi-minor axis
#define ORBIT_STEPS 36
#define TRAIL_LEN 5

// {cos, sin} * 100 of the three orbit tilts (0 and +/- 25 degrees). Anything
// steeper would push the orbits off the narrow side of the screen.
static const int16_t ORB[3][2] = {{100, 0}, {91, 42}, {91, -42}};

static void ellipse_point(int orbit, int t, int off, lv_point_t *p) {
    int eu = ATOM_A * TRIG[t][0] / 100;
    int ev = ATOM_B * TRIG[t][1] / 100;
    p->x = ATOM_CV + (eu * ORB[orbit][1] + ev * ORB[orbit][0]) / 100;
    p->y = ATOM_CU + (eu * ORB[orbit][0] - ev * ORB[orbit][1]) / 100 - off;
}

static const uint8_t ATOM_STARS[8][3] = {
    {6, 6, 0}, {12, 60, 3}, {120, 8, 5}, {114, 58, 1}, {30, 4, 4}, {98, 64, 2}, {70, 3, 1}, {22, 33, 5},
};

static void scene_atom(lv_obj_t *canvas, int off, uint32_t frame, lv_draw_rect_dsc_t *fg,
                       lv_draw_line_dsc_t *line, lv_draw_arc_dsc_t *arc,
                       lv_draw_label_dsc_t *label) {
    for (int s = 0; s < 8; s++) {
        if (((frame / 4) + ATOM_STARS[s][2]) % 3 != 0) {
            lv_canvas_draw_rect(canvas, ATOM_STARS[s][1], ATOM_STARS[s][0] - off, 1, 1, fg);
        }
    }

    for (int o = 0; o < 3; o++) {
        lv_point_t pts[ORBIT_STEPS + 1];
        for (int t = 0; t <= ORBIT_STEPS; t++) {
            ellipse_point(o, t % ORBIT_STEPS, off, &pts[t]);
        }
        lv_canvas_draw_line(canvas, pts, ORBIT_STEPS + 1, line);
    }

    for (int o = 0; o < 3; o++) {
        int speed = (o == 1) ? 3 : 2;
        int head = (o == 2) ? (ORBIT_STEPS - (int)((frame * speed) % ORBIT_STEPS)) % ORBIT_STEPS
                            : (frame * speed + o * 12) % ORBIT_STEPS;

        for (int k = TRAIL_LEN - 1; k >= 0; k--) {
            int t = (o == 2) ? (head + k) % ORBIT_STEPS : (head - k + ORBIT_STEPS) % ORBIT_STEPS;
            lv_point_t e;
            ellipse_point(o, t, off, &e);
            if (k == 0) {
                lv_canvas_draw_rect(canvas, e.x - 2, e.y - 2, 5, 5, fg);
            } else if (k < 3) {
                lv_canvas_draw_rect(canvas, e.x - 1, e.y - 1, 3, 3, fg);
            } else {
                lv_canvas_draw_rect(canvas, e.x, e.y, 2, 2, fg);
            }
        }
    }

    uint32_t rp = frame % 40;
    if (rp < 14) {
        int rr = 14 + (int)rp;
        int gap = 24 + (int)rp * 3;
        for (int j = 0; j < 6; j++) {
            lv_canvas_draw_arc(canvas, ATOM_CV, ATOM_CU - off, rr, 60 * j + gap / 2,
                               60 * (j + 1) - gap / 2, arc);
        }
    }

    int pulse = (frame / 3) % 4;
    lv_canvas_draw_arc(canvas, ATOM_CV, ATOM_CU - off, 11 + (pulse < 2 ? pulse : 4 - pulse), 0, 360,
                       arc);
    lv_canvas_draw_text(canvas, 0, ATOM_CU - off - 11, ART_V, label, "Y");
}

/* ------------------------------------------------------------- starfield */

// x = v, y = u, phase
static const uint8_t SKY[22][3] = {
    {5, 118, 0},  {14, 104, 2}, {26, 121, 4}, {40, 112, 1}, {55, 124, 3}, {62, 100, 5},
    {9, 92, 1},   {33, 96, 5},  {48, 88, 0},  {60, 80, 2},  {18, 78, 3},  {29, 68, 4},
    {44, 72, 1},  {7, 62, 5},   {56, 58, 0},  {21, 50, 2},  {38, 44, 4},  {63, 40, 3},
    {12, 32, 0},  {30, 24, 5},  {50, 18, 1},  {24, 8, 2},
};

static void scene_stars(lv_obj_t *canvas, int off, uint32_t frame, lv_draw_rect_dsc_t *fg,
                        lv_draw_line_dsc_t *line) {
    for (int s = 0; s < 22; s++) {
        int phase = ((frame / 3) + SKY[s][2]) % 6;
        int y = SKY[s][1] - off;
        if (phase == 0) {
            continue;
        }
        if (phase == 3) {
            // brief twinkle: a tiny cross
            lv_canvas_draw_rect(canvas, SKY[s][0] - 1, y, 3, 1, fg);
            lv_canvas_draw_rect(canvas, SKY[s][0], y - 1, 1, 3, fg);
        } else {
            lv_canvas_draw_rect(canvas, SKY[s][0], y, 1, 1, fg);
        }
    }

    // a shooting star crosses the picture every ~6 s
    uint32_t sp = frame % 60;
    if (sp < 18) {
        int t = (int)sp;
        int u = 120 - t * 6;
        int v = 4 + t * 3;
        lv_point_t p[2] = {{v, u - off}, {v - 9, u + 18 - off}};
        lv_canvas_draw_line(canvas, p, 2, line);
        lv_canvas_draw_rect(canvas, v - 1, u - off - 1, 3, 3, fg);
    }
}

/* ------------------------------------------------------ moon over ridges */

// silhouette height (in u) sampled every 4 px across v
static const uint8_t RIDGE[18] = {14, 20, 30, 24, 18, 26, 38, 30, 22, 16, 24, 34, 44, 36, 26, 20, 16, 12};

static void scene_moon(lv_obj_t *canvas, int off, uint32_t frame, lv_draw_rect_dsc_t *fg,
                       lv_draw_rect_dsc_t *bg, lv_draw_arc_dsc_t *arc) {
    // moon slowly drifting up, with a bite taken out of it to make a crescent
    int mu = 78 + (int)((frame / 6) % 22);
    int mv = 22;
    for (int r = 0; r <= 10; r++) {
        lv_canvas_draw_arc(canvas, mv, mu - off, r + 1, 0, 360, arc);
    }
    for (int r = 0; r <= 9; r++) {
        lv_draw_arc_dsc_t hole = *arc;
        hole.color = bg->bg_color;
        lv_canvas_draw_arc(canvas, mv + 5, mu - off + 1, r + 1, 0, 360, &hole);
    }

    for (int s = 0; s < 22; s += 2) {
        if (SKY[s][1] < 60) {
            continue;
        }
        if (((frame / 4) + SKY[s][2]) % 4 != 0) {
            lv_canvas_draw_rect(canvas, SKY[s][0], SKY[s][1] - off, 1, 1, fg);
        }
    }

    // ridge line, filled down to the bottom of the picture
    for (int v = 0; v < ART_V; v++) {
        int i = v / 4;
        int a = RIDGE[i], b = RIDGE[i + 1 > 17 ? 17 : i + 1];
        int h = a + (b - a) * (v % 4) / 4;
        lv_canvas_draw_rect(canvas, v, 0 - off, 1, h, fg);
    }
}

/* ----------------------------------------------------------------- waves */

static void scene_waves(lv_obj_t *canvas, int off, uint32_t frame, lv_draw_line_dsc_t *line) {
    for (int w = 0; w < 6; w++) {
        lv_point_t p[24];
        int base = 12 + w * 20;
        int amp = 4 + (w % 3);
        for (int i = 0; i < 24; i++) {
            int v = i * 3;
            int t = (i * 3 + (int)frame * 2 + w * 7) % 36;
            p[i].x = v;
            p[i].y = base + amp * TRIG[t][1] / 100 - off;
        }
        lv_canvas_draw_line(canvas, p, 24, line);
    }
}

/* ------------------------------------------------------------------ cube */

static const int8_t CUBE_V[8][3] = {
    {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
    {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1},
};
static const uint8_t CUBE_E[12][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
    {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
};

static void scene_cube(lv_obj_t *canvas, int off, uint32_t frame, lv_draw_rect_dsc_t *fg,
                       lv_draw_line_dsc_t *line) {
    int a = (int)((frame * 2) % 36), b = (int)(frame % 36);
    int ca = TRIG[a][0], sa = TRIG[a][1], cb = TRIG[b][0], sb = TRIG[b][1];

    lv_point_t p[8];
    for (int i = 0; i < 8; i++) {
        int x = CUBE_V[i][0] * 100, y = CUBE_V[i][1] * 100, z = CUBE_V[i][2] * 100;
        int x1 = (x * ca - z * sa) / 100;
        int z1 = (x * sa + z * ca) / 100;
        int y2 = (y * cb - z1 * sb) / 100;
        p[i].x = ATOM_CV + x1 * 20 / 100;
        p[i].y = ATOM_CU + y2 * 30 / 100 - off;
    }
    for (int e = 0; e < 12; e++) {
        lv_point_t seg[2] = {p[CUBE_E[e][0]], p[CUBE_E[e][1]]};
        lv_canvas_draw_line(canvas, seg, 2, line);
    }
    for (int i = 0; i < 8; i++) {
        lv_canvas_draw_rect(canvas, p[i].x - 1, p[i].y - 1, 3, 3, fg);
    }
}

/* ------------------------------------------------------------- slideshow */

static void draw_scene(lv_obj_t *canvas, int off, uint32_t frame, int scene) {
    lv_draw_rect_dsc_t bg;
    init_rect_dsc(&bg, LVGL_BACKGROUND);
    lv_draw_rect_dsc_t fg;
    init_rect_dsc(&fg, LVGL_FOREGROUND);
    lv_draw_line_dsc_t line;
    init_line_dsc(&line, LVGL_FOREGROUND, 2);
    lv_draw_line_dsc_t thin;
    init_line_dsc(&thin, LVGL_FOREGROUND, 1);
    lv_draw_arc_dsc_t arc;
    init_arc_dsc(&arc, LVGL_FOREGROUND, 2);
    lv_draw_label_dsc_t label;
    init_label_dsc(&label, LVGL_FOREGROUND, &lv_font_montserrat_20, LV_TEXT_ALIGN_CENTER);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &bg);

    switch (scene) {
    case 0:
        scene_atom(canvas, off, frame, &fg, &line, &arc, &label);
        break;
    case 1:
        scene_stars(canvas, off, frame, &fg, &thin);
        break;
    case 2:
        scene_moon(canvas, off, frame, &fg, &bg, &arc);
        break;
    case 3:
        scene_waves(canvas, off, frame, &line);
        break;
    default:
        scene_cube(canvas, off, frame, &fg, &line);
        break;
    }
}

static void draw_art(struct zmk_widget_status *widget, uint32_t frame) {
    int scene = (int)((frame / SLIDE_FRAMES) % SCENE_COUNT);

    lv_obj_t *a = lv_obj_get_child(widget->obj, CHILD_ART_A);
    lv_obj_t *b = lv_obj_get_child(widget->obj, CHILD_ART_B);

    draw_scene(a, 0, frame, scene);
    draw_scene(b, ART_SPLIT, frame, scene);

    rotate_canvas(a, widget->cbuf2);
    rotate_canvas(b, widget->cbuf3);
}

/* ------------------------------------------------------------------ info */

// Battery icon + percentage + link symbol, kept in rows 8 <= y < 40: away
// from the edge of the screen and clear of the art canvas overlapping it.
static void draw_top(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, CHILD_INFO);

    lv_draw_label_dsc_t label_dsc;
    init_label_dsc(&label_dsc, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_RIGHT);
    lv_draw_label_dsc_t pct_dsc;
    init_label_dsc(&pct_dsc, LVGL_FOREGROUND, &lv_font_unscii_8, LV_TEXT_ALIGN_LEFT);
    lv_draw_rect_dsc_t rect_bg;
    init_rect_dsc(&rect_bg, LVGL_BACKGROUND);
    lv_draw_rect_dsc_t rect_fg;
    init_rect_dsc(&rect_fg, LVGL_FOREGROUND);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &rect_bg);

    lv_canvas_draw_rect(canvas, 0, 12, 15, 9, &rect_fg);
    lv_canvas_draw_rect(canvas, 1, 13, 13, 7, &rect_bg);
    lv_canvas_draw_rect(canvas, 2, 14, (state->battery * 11 + 50) / 100, 5, &rect_fg);
    lv_canvas_draw_rect(canvas, 15, 14, 2, 5, &rect_fg);

    if (state->charging) {
        lv_canvas_draw_rect(canvas, 7, 13, 1, 2, &rect_bg);
        lv_canvas_draw_rect(canvas, 5, 15, 4, 1, &rect_bg);
        lv_canvas_draw_rect(canvas, 7, 16, 1, 2, &rect_bg);
        lv_canvas_draw_rect(canvas, 6, 13, 1, 3, &rect_fg);
        lv_canvas_draw_rect(canvas, 7, 15, 1, 3, &rect_fg);
    }

    char pct[6] = {};
    snprintf(pct, sizeof(pct), "%d%%", state->battery);
    lv_canvas_draw_text(canvas, 19, 13, 33, &pct_dsc, pct);

    lv_canvas_draw_text(canvas, 0, 8, CANVAS_SIZE, &label_dsc,
                        state->connected ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE);

    rotate_canvas(canvas, cbuf);
}

static void anim_tick(struct k_work *work) {
    anim_frame++;

    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { draw_art(widget, anim_frame); }
    k_work_schedule(&anim_work, K_MSEC(ANIM_MS));
}

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

    // Created in CHILD_* order; the two art canvases come last so they paint
    // over the empty edge of the info canvas they overlap.
    lv_obj_t *info = lv_canvas_create(widget->obj);
    lv_obj_align(info, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_canvas_set_buffer(info, widget->cbuf, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);
    lv_obj_t *art_b = lv_canvas_create(widget->obj);
    lv_obj_align(art_b, LV_ALIGN_TOP_LEFT, -8, 0);
    lv_canvas_set_buffer(art_b, widget->cbuf3, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);
    lv_obj_t *art_a = lv_canvas_create(widget->obj);
    lv_obj_align(art_a, LV_ALIGN_TOP_LEFT, 60, 0);
    lv_canvas_set_buffer(art_a, widget->cbuf2, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    sys_slist_append(&widgets, &widget->node);
    widget_battery_status_init();
    widget_peripheral_status_init();

    k_work_init_delayable(&anim_work, anim_tick);
    k_work_schedule(&anim_work, K_MSEC(ANIM_MS));

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }
