/**
 * @file stopwatch.h
 * Pure wall-clock stopwatch: running flag, accumulated elapsed, and a
 * monotonic start timestamp. Elapsed derives from the wall clock so it stays
 * correct while the owning mode is hidden. No LVGL dependency (host-testable).
 *
 * Times are milliseconds. The caller supplies "now" (a monotonic ms tick), so
 * the core is deterministic and testable.
 */
#ifndef KDESKDASH_STOPWATCH_H
#define KDESKDASH_STOPWATCH_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     running;
    uint32_t start_ms;       /* tick when last started (valid while running) */
    uint32_t accumulated_ms; /* elapsed banked from previous run segments */
} stopwatch_t;

/* Reset to a stopped, zeroed stopwatch. */
void stopwatch_init(stopwatch_t *sw);

/* Start counting from `now` (no-op if already running). */
void stopwatch_start(stopwatch_t *sw, uint32_t now_ms);

/* Stop and bank the current segment into accumulated (no-op if stopped). */
void stopwatch_stop(stopwatch_t *sw, uint32_t now_ms);

/* Toggle running state at `now`. */
void stopwatch_toggle(stopwatch_t *sw, uint32_t now_ms);

/* Reset elapsed to zero. If running, keep running (restart the segment at
 * `now`); if stopped, stay stopped at zero. */
void stopwatch_reset(stopwatch_t *sw, uint32_t now_ms);

/* Total elapsed ms at `now` (accumulated plus current segment if running). */
uint32_t stopwatch_elapsed_ms(const stopwatch_t *sw, uint32_t now_ms);

/* Format elapsed as M:SS.s (tenths) into `buf` (needs >= 16 bytes). */
void stopwatch_format(uint32_t elapsed_ms, char *buf, int buf_size);

#endif /* KDESKDASH_STOPWATCH_H */
