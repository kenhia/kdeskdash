/**
 * @file stopwatch.c
 * Pure wall-clock stopwatch. See stopwatch.h.
 */
#include "stopwatch.h"

#include <stdio.h>

void stopwatch_init(stopwatch_t *sw) {
    sw->running = false;
    sw->start_ms = 0;
    sw->accumulated_ms = 0;
}

void stopwatch_start(stopwatch_t *sw, uint32_t now_ms) {
    if (sw->running)
        return;
    sw->running = true;
    sw->start_ms = now_ms;
}

void stopwatch_stop(stopwatch_t *sw, uint32_t now_ms) {
    if (!sw->running)
        return;
    sw->accumulated_ms += now_ms - sw->start_ms;
    sw->running = false;
}

void stopwatch_toggle(stopwatch_t *sw, uint32_t now_ms) {
    if (sw->running)
        stopwatch_stop(sw, now_ms);
    else
        stopwatch_start(sw, now_ms);
}

void stopwatch_reset(stopwatch_t *sw, uint32_t now_ms) {
    sw->accumulated_ms = 0;
    /* Keep running if it was running: restart the current segment at `now`. */
    if (sw->running)
        sw->start_ms = now_ms;
}

uint32_t stopwatch_elapsed_ms(const stopwatch_t *sw, uint32_t now_ms) {
    uint32_t total = sw->accumulated_ms;
    if (sw->running)
        total += now_ms - sw->start_ms;
    return total;
}

void stopwatch_format(uint32_t elapsed_ms, char *buf, int buf_size) {
    uint32_t tenths = (elapsed_ms / 100) % 10;
    uint32_t total_s = elapsed_ms / 1000;
    uint32_t minutes = total_s / 60;
    uint32_t seconds = total_s % 60;
    snprintf(buf, buf_size, "%u:%02u.%u", (unsigned)minutes, (unsigned)seconds,
             (unsigned)tenths);
}
