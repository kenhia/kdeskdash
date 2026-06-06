/**
 * @file shell.c
 * Mode shell: registration, active-mode lifecycle, and swipe navigation.
 */
#include "shell.h"

#include <string.h>

#include "lvgl.h"
#include "registry.h"

#define SHELL_MAX_CONTENT 8

static kd_mode_t *s_content[SHELL_MAX_CONTENT];
static int        s_content_count;
static kd_mode_t *s_menu;
static kd_mode_t *s_active;

/* Screens that already have the gesture handler attached, so we wire each one
 * exactly once. Content modes + menu, so the same bound applies. */
static lv_obj_t *s_wired[SHELL_MAX_CONTENT + 1];
static int       s_wired_count;

static int active_content_index(void) {
    for (int i = 0; i < s_content_count; i++)
        if (s_content[i] == s_active)
            return i;
    return -1;
}

static void gesture_cb(lv_event_t *e) {
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev)
        return;
    /* Direction reported by LVGL's gesture detector. The physical-swipe ->
     * LV_DIR mapping is verified on hardware; adjust here if inverted. */
    switch (lv_indev_get_gesture_dir(indev)) {
    case LV_DIR_LEFT:
        shell_set_active(shell_next_content());
        break;
    case LV_DIR_RIGHT:
        shell_set_active(shell_prev_content());
        break;
    case LV_DIR_BOTTOM:
        shell_set_active(s_menu ? s_menu : shell_next_content());
        break;
    default:
        break;
    }
}

static void wire_gesture_once(lv_obj_t *screen) {
    if (!screen)
        return;
    for (int i = 0; i < s_wired_count; i++)
        if (s_wired[i] == screen)
            return;
    if (s_wired_count < (int)(sizeof(s_wired) / sizeof(s_wired[0]))) {
        lv_obj_add_event_cb(screen, gesture_cb, LV_EVENT_GESTURE, NULL);
        s_wired[s_wired_count++] = screen;
    }
}

void shell_init(void) {
    memset(s_content, 0, sizeof(s_content));
    s_content_count = 0;
    s_menu = NULL;
    s_active = NULL;
    memset(s_wired, 0, sizeof(s_wired));
    s_wired_count = 0;
}

void shell_register_content_mode(kd_mode_t *m) {
    if (m && s_content_count < SHELL_MAX_CONTENT)
        s_content[s_content_count++] = m;
}

void shell_register_menu(kd_mode_t *m) {
    s_menu = m;
}

kd_mode_t *shell_find_mode(const char *id) {
    if (!id)
        return NULL;
    if (s_menu && s_menu->id && strcmp(s_menu->id, id) == 0)
        return s_menu;
    for (int i = 0; i < s_content_count; i++)
        if (s_content[i]->id && strcmp(s_content[i]->id, id) == 0)
            return s_content[i];
    return NULL;
}

kd_mode_t *shell_active(void) {
    return s_active;
}

kd_mode_t *shell_next_content(void) {
    if (s_content_count == 0)
        return s_active;
    int idx = active_content_index();
    if (idx < 0)
        return s_content[0];
    return s_content[registry_wrap_index(s_content_count, idx, +1)];
}

kd_mode_t *shell_prev_content(void) {
    if (s_content_count == 0)
        return s_active;
    int idx = active_content_index();
    if (idx < 0)
        return s_content[0];
    return s_content[registry_wrap_index(s_content_count, idx, -1)];
}

int shell_content_count(void) {
    return s_content_count;
}

kd_mode_t *shell_content_at(int index) {
    if (index < 0 || index >= s_content_count)
        return NULL;
    return s_content[index];
}

void shell_set_active(kd_mode_t *m) {
    if (!m || m == s_active)
        return;
    if (s_active && s_active->deactivate)
        s_active->deactivate(s_active);
    s_active = m;
    if (m->activate)
        m->activate(m); /* must leave m->screen non-NULL */
    if (m->screen) {
        wire_gesture_once(m->screen);
        lv_screen_load(m->screen);
    }
}

void shell_tick(void) {
    if (s_active && s_active->tick)
        s_active->tick(s_active);
}

void shell_start(const char *restore_id) {
    kd_mode_t *start = shell_find_mode(restore_id);
    if (!start)
        start = s_menu;
    if (!start && s_content_count > 0)
        start = s_content[0];
    if (start)
        shell_set_active(start);
}
