/**
 * @file gol.c
 * Pure Conway's Game of Life core (toroidal, with fade trails). See gol.h.
 */
#include "gol.h"

#include <stdlib.h>
#include <string.h>

uint32_t gol_rand_u32(uint32_t *state) {
    uint32_t x = *state ? *state : 0x9e3779b9u; /* avoid the zero fixed point */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static int idx(const gol_t *g, int x, int y) {
    return y * g->cols + x;
}

bool gol_init(gol_t *g, int cols, int rows, const gol_settings_t *cfg) {
    if (!g || cols <= 0 || rows <= 0)
        return false;
    memset(g, 0, sizeof(*g));
    size_t n = (size_t)cols * (size_t)rows;
    g->cur = calloc(n, 1);
    g->next = calloc(n, 1);
    g->trail = calloc(n, 1);
    if (!g->cur || !g->next || !g->trail) {
        gol_free(g);
        return false;
    }
    g->cols = cols;
    g->rows = rows;
    if (cfg)
        g->cfg = *cfg;
    if (g->cfg.trail_turns < 1)
        g->cfg.trail_turns = 1;
    return true;
}

void gol_free(gol_t *g) {
    if (!g)
        return;
    free(g->cur);
    free(g->next);
    free(g->trail);
    g->cur = g->next = g->trail = NULL;
    g->cols = g->rows = 0;
}

void gol_clear(gol_t *g) {
    if (!g || !g->cur)
        return;
    size_t n = (size_t)g->cols * (size_t)g->rows;
    memset(g->cur, 0, n);
    memset(g->trail, 0, n);
}

void gol_set(gol_t *g, int x, int y, bool alive) {
    if (!g || x < 0 || y < 0 || x >= g->cols || y >= g->rows)
        return;
    int i = idx(g, x, y);
    g->cur[i] = alive ? 1 : 0;
    if (alive)
        g->trail[i] = (uint8_t)g->cfg.trail_turns;
}

bool gol_get(const gol_t *g, int x, int y) {
    if (!g || x < 0 || y < 0 || x >= g->cols || y >= g->rows)
        return false;
    return g->cur[idx(g, x, y)] != 0;
}

uint8_t gol_trail(const gol_t *g, int x, int y) {
    if (!g || x < 0 || y < 0 || x >= g->cols || y >= g->rows)
        return 0;
    return g->trail[idx(g, x, y)];
}

int gol_live_count(const gol_t *g) {
    if (!g || !g->cur)
        return 0;
    size_t n = (size_t)g->cols * (size_t)g->rows;
    int count = 0;
    for (size_t i = 0; i < n; i++)
        count += g->cur[i];
    return count;
}

void gol_seed(gol_t *g, uint32_t *rng_state) {
    if (!g || !g->cur || !rng_state)
        return;
    double density = g->cfg.density;
    if (density < 0.0)
        density = 0.0;
    if (density > 1.0)
        density = 1.0;
    /* Threshold against the full u32 range to decide alive/dead per cell. */
    uint32_t threshold = (uint32_t)(density * 4294967295.0);
    size_t n = (size_t)g->cols * (size_t)g->rows;
    for (size_t i = 0; i < n; i++) {
        bool alive = gol_rand_u32(rng_state) <= threshold && density > 0.0;
        g->cur[i] = alive ? 1 : 0;
        g->trail[i] = alive ? (uint8_t)g->cfg.trail_turns : 0;
    }
}

static int live_neighbors(const gol_t *g, int x, int y) {
    int count = 0;
    for (int dy = -1; dy <= 1; dy++) {
        int ny = y + dy;
        if (ny < 0)
            ny += g->rows;
        else if (ny >= g->rows)
            ny -= g->rows;
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0)
                continue;
            int nx = x + dx;
            if (nx < 0)
                nx += g->cols;
            else if (nx >= g->cols)
                nx -= g->cols;
            count += g->cur[ny * g->cols + nx];
        }
    }
    return count;
}

void gol_step(gol_t *g) {
    if (!g || !g->cur)
        return;

    for (int y = 0; y < g->rows; y++) {
        for (int x = 0; x < g->cols; x++) {
            int i = idx(g, x, y);
            int n = live_neighbors(g, x, y);
            bool alive = g->cur[i] != 0;
            g->next[i] = (alive ? (n == 2 || n == 3) : (n == 3)) ? 1 : 0;
        }
    }

    size_t n = (size_t)g->cols * (size_t)g->rows;
    for (size_t i = 0; i < n; i++) {
        if (g->next[i]) {
            g->trail[i] = (uint8_t)g->cfg.trail_turns; /* alive -> full */
        } else if (g->cfg.trail && g->trail[i] > 0) {
            g->trail[i]--; /* fade out over trail_turns generations */
        } else {
            g->trail[i] = 0; /* trail off, or fully faded -> dark */
        }
    }

    uint8_t *tmp = g->cur;
    g->cur = g->next;
    g->next = tmp;
}
