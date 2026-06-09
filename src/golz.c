/**
 * @file golz.c
 * Pure GoLZ core (two-faction Game of Life). See golz.h.
 */
#include "golz.h"

#include <stdlib.h>
#include <string.h>

static int gz_clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void golz_settings_clamp(golz_settings_t *cfg) {
    if (!cfg)
        return;
    cfg->initial_count = gz_clampi(cfg->initial_count, 0, 5);
    cfg->zombie_reinfect = gz_clampi(cfg->zombie_reinfect, 0, 100);
    cfg->zombie_spawn_chance = gz_clampi(cfg->zombie_spawn_chance, 0, 100);
    if (cfg->max_generations < 1)
        cfg->max_generations = 1;
}

bool golz_init(golz_t *g, int cols, int rows, const gol_settings_t *living_cfg,
               const golz_settings_t *cfg, uint32_t *rng) {
    if (!g || cols <= 0 || rows <= 0 || !rng)
        return false;
    memset(g, 0, sizeof(*g));
    if (!gol_init(&g->living, cols, rows, living_cfg))
        return false;
    size_t n = (size_t)cols * (size_t)rows;
    g->zombies = calloc(n, 1);
    g->z_new = calloc(n, 1);
    g->prev_living = calloc(n, 1);
    g->snapshot = calloc(n, 1);
    g->died_mask = calloc(n, 1);
    g->empties = calloc(n, sizeof(int));
    if (!g->zombies || !g->z_new || !g->prev_living || !g->snapshot ||
        !g->died_mask || !g->empties) {
        golz_free(g);
        return false;
    }
    g->cols = cols;
    g->rows = rows;
    if (cfg)
        g->cfg = *cfg;
    golz_settings_clamp(&g->cfg);
    g->rng = rng;
    gol_cycle_reset(&g->cycle);
    g->generation = 0;
    return true;
}

void golz_free(golz_t *g) {
    if (!g)
        return;
    gol_free(&g->living);
    free(g->zombies);
    free(g->z_new);
    free(g->prev_living);
    free(g->snapshot);
    free(g->died_mask);
    free(g->empties);
    g->zombies = g->z_new = g->prev_living = g->snapshot = g->died_mask = NULL;
    g->empties = NULL;
    g->cols = g->rows = 0;
}

void golz_clear(golz_t *g) {
    if (!g || !g->zombies)
        return;
    size_t n = (size_t)g->cols * (size_t)g->rows;
    gol_clear(&g->living);
    memset(g->zombies, 0, n);
    memset(g->z_new, 0, n);
    memset(g->prev_living, 0, n);
    memset(g->snapshot, 0, n);
    memset(g->died_mask, 0, n);
    gol_cycle_reset(&g->cycle);
    g->generation = 0;
}

int golz_sample_empty(golz_t *g, int want) {
    if (!g || !g->empties || want <= 0)
        return 0;
    size_t n = (size_t)g->cols * (size_t)g->rows;
    int m = 0;
    for (size_t i = 0; i < n; i++) {
        if (g->living.cur[i] == 0 && g->zombies[i] == 0 && g->z_new[i] == 0)
            g->empties[m++] = (int)i;
    }
    int k = want < m ? want : m;
    /* Partial Fisher-Yates: draw k distinct indices without replacement. */
    for (int i = 0; i < k; i++) {
        int j = i + (int)(gol_rand_u32(g->rng) % (uint32_t)(m - i));
        int tmp = g->empties[i];
        g->empties[i] = g->empties[j];
        g->empties[j] = tmp;
    }
    return k;
}

int golz_zombie_count(const golz_t *g) {
    if (!g || !g->zombies)
        return 0;
    size_t n = (size_t)g->cols * (size_t)g->rows;
    int c = 0;
    for (size_t i = 0; i < n; i++)
        c += g->zombies[i];
    return c;
}

void golz_seed(golz_t *g) {
    if (!g || !g->zombies || !g->rng)
        return;
    size_t n = (size_t)g->cols * (size_t)g->rows;
    gol_clear(&g->living);
    memset(g->zombies, 0, n);
    memset(g->z_new, 0, n);
    memset(g->prev_living, 0, n);
    memset(g->snapshot, 0, n);
    memset(g->died_mask, 0, n);
    gol_cycle_reset(&g->cycle);
    g->generation = 0;

    gol_seed(&g->living, g->rng);
    int k = golz_sample_empty(g, g->cfg.initial_count);
    for (int i = 0; i < k; i++)
        g->zombies[g->empties[i]] = 1;
}
