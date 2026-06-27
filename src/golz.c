/**
 * @file golz.c
 * Pure GoLZ core (two-faction Game of Life). See golz.h.
 */
#include "golz.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int gz_clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The eight neighbour offsets, in a fixed order; movement picks one at random.
 * The toroidal wrap below matches live_neighbors() in gol.c exactly. */
static const int GZ_DX[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
static const int GZ_DY[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

static int gz_wrap(int v, int n) {
    if (v < 0)
        return v + n;
    if (v >= n)
        return v - n;
    return v;
}

/* Full toroidal wrap for offsets larger than one (the machete reach is +/-2). */
static int gz_wrapn(int v, int n) {
    v %= n;
    if (v < 0)
        v += n;
    return v;
}

void golz_settings_clamp(golz_settings_t *cfg) {
    if (!cfg)
        return;
    cfg->initial_count = gz_clampi(cfg->initial_count, 0, 5);
    cfg->zombie_reinfect = gz_clampi(cfg->zombie_reinfect, 0, 100);
    cfg->zombie_spawn_chance = gz_clampi(cfg->zombie_spawn_chance, 0, 100);
    if (cfg->max_generations < 1)
        cfg->max_generations = 1;
    cfg->machete_percentage = gz_clampi(cfg->machete_percentage, 0, 100);
    cfg->human_kill_zombie = gz_clampi(cfg->human_kill_zombie, 0, 100);
    if (cfg->generations_to_win < 0)
        cfg->generations_to_win = 0; /* 0 disables the human-win threshold */
}

bool golz_init(golz_t *g, int cols, int rows, const gol_settings_t *living_cfg,
               const golz_settings_t *cfg, uint32_t *rng) {
    if (!g || cols <= 0 || rows <= 0 || !rng)
        return false;
    memset(g, 0, sizeof(*g));
    if (!gol_init(&g->living, cols, rows, living_cfg))
        return false;
    size_t n = (size_t)cols * (size_t)rows;
    g->machetes = calloc(n, 1);
    g->zombies = calloc(n, 1);
    g->z_new = calloc(n, 1);
    g->z_trail = calloc(n, 1);
    g->prev_living = calloc(n, 1);
    g->snapshot = calloc(n, 1);
    g->died_mask = calloc(n, 1);
    g->empties = calloc(n, sizeof(int));
    if (!g->machetes || !g->zombies || !g->z_new || !g->z_trail ||
        !g->prev_living || !g->snapshot || !g->died_mask || !g->empties) {
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
    free(g->machetes);
    free(g->zombies);
    free(g->z_new);
    free(g->z_trail);
    free(g->prev_living);
    free(g->snapshot);
    free(g->died_mask);
    free(g->empties);
    g->machetes = NULL;
    g->zombies = g->z_new = g->prev_living = g->snapshot = g->died_mask = NULL;
    g->z_trail = NULL;
    g->empties = NULL;
    g->cols = g->rows = 0;
}

void golz_clear(golz_t *g) {
    if (!g || !g->zombies)
        return;
    size_t n = (size_t)g->cols * (size_t)g->rows;
    gol_clear(&g->living);
    memset(g->machetes, 0, n);
    memset(g->zombies, 0, n);
    memset(g->z_new, 0, n);
    memset(g->z_trail, 0, n);
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
    memset(g->machetes, 0, n);
    memset(g->zombies, 0, n);
    memset(g->z_new, 0, n);
    memset(g->z_trail, 0, n);
    memset(g->prev_living, 0, n);
    memset(g->snapshot, 0, n);
    memset(g->died_mask, 0, n);
    gol_cycle_reset(&g->cycle);
    g->generation = 0;

    gol_seed(&g->living, g->rng);
    int k = golz_sample_empty(g, g->cfg.initial_count);
    for (int i = 0; i < k; i++)
        g->zombies[g->empties[i]] = 1;

    /* Static machete layer: each cell rolls independently. Drawn LAST and skipped
     * entirely at 0% so existing PRNG streams (and their tests) are unperturbed.
     * Lean into chaos: the realised count varies widely around the nominal rate. */
    if (g->cfg.machete_percentage > 0) {
        for (size_t i = 0; i < n; i++) {
            if ((int)(gol_rand_u32(g->rng) % 100u) < g->cfg.machete_percentage)
                g->machetes[i] = 1;
        }
    }
}

/* Promote zombies born last generation (reinfect/spawn) into the active layer
 * and clear the pending grid. R12: they only become active here, at the start
 * of the following step. */
static void gz_promote(golz_t *g) {
    size_t n = (size_t)g->cols * (size_t)g->rows;
    for (size_t i = 0; i < n; i++) {
        if (g->z_new[i]) {
            g->zombies[i] = 1;
            g->z_new[i] = 0;
        }
    }
}

/* Conway living turn: zombies live in a separate grid so they are never Conway
 * neighbours. After the step, suppress any birth that landed on a zombie cell
 * (clearing both the cell and its trail), then record genuine Conway deaths. */
static void gz_living_turn(golz_t *g) {
    size_t n = (size_t)g->cols * (size_t)g->rows;
    gol_step(&g->living);
    for (size_t i = 0; i < n; i++) {
        if (g->zombies[i] && g->living.cur[i]) {
            g->living.cur[i] = 0;   /* birth onto a zombie cell suppressed */
            g->living.trail[i] = 0; /* no phantom trail left behind */
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (g->prev_living[i] && !g->living.cur[i])
            g->died_mask[i] = 1;
    }
}

/* Machete turn (M3/M4): armed living cells strike before the zombies act. Each
 * living cell that also holds a machete, in reshuffled order, gathers the live
 * zombies within Chebyshev distance 2 (the 5x5 toroidal block minus its centre);
 * if any are present it rolls human_kill_zombie and, on success, kills one at
 * random. Kills read/write the live zombie grid, so a zombie killed earlier this
 * turn is no longer a candidate (one machete, one kill, no double-kill). Never
 * touches living cells or death bookkeeping; killed zombies fade via the red
 * trail like any other death. */
static void gz_machete(golz_t *g) {
    if (g->cfg.human_kill_zombie <= 0 || g->cfg.machete_percentage <= 0)
        return; /* no machetes seeded or no kill chance -> nothing to do */
    size_t n = (size_t)g->cols * (size_t)g->rows;
    int m = 0;
    for (size_t i = 0; i < n; i++)
        if (g->living.cur[i] && g->machetes[i])
            g->empties[m++] = (int)i;
    if (m == 0)
        return;
    /* Fisher-Yates the strike order so overlapping reach windows are fair. */
    for (int i = m - 1; i > 0; i--) {
        int j = (int)(gol_rand_u32(g->rng) % (uint32_t)(i + 1));
        int t = g->empties[i];
        g->empties[i] = g->empties[j];
        g->empties[j] = t;
    }
    for (int s = 0; s < m; s++) {
        int idx = g->empties[s];
        int x = idx % g->cols;
        int y = idx / g->cols;
        int targets[24]; /* 5x5 block minus the centre */
        int cnt = 0;
        for (int dy = -2; dy <= 2; dy++) {
            for (int dx = -2; dx <= 2; dx++) {
                if (dx == 0 && dy == 0)
                    continue;
                int nx = gz_wrapn(x + dx, g->cols);
                int ny = gz_wrapn(y + dy, g->rows);
                int ti = ny * g->cols + nx;
                if (g->zombies[ti])
                    targets[cnt++] = ti;
            }
        }
        if (cnt == 0)
            continue; /* no zombie in range -> no roll */
        if ((int)(gol_rand_u32(g->rng) % 100u) >= g->cfg.human_kill_zombie)
            continue; /* the kill roll missed */
        int victim = targets[(int)(gol_rand_u32(g->rng) % (uint32_t)cnt)];
        g->zombies[victim] = 0; /* machete kill */
    }
}

/* Sequential zombie movement: shuffle the order, then each zombie attempts one
 * of eight random directions; blocked by any occupied cell (living or zombie)
 * it stays put (single attempt, no retry). */
static void gz_move(golz_t *g) {
    size_t n = (size_t)g->cols * (size_t)g->rows;
    int m = 0;
    for (size_t i = 0; i < n; i++)
        if (g->zombies[i])
            g->empties[m++] = (int)i;
    /* Fisher-Yates shuffle the move order. */
    for (int i = m - 1; i > 0; i--) {
        int j = (int)(gol_rand_u32(g->rng) % (uint32_t)(i + 1));
        int t = g->empties[i];
        g->empties[i] = g->empties[j];
        g->empties[j] = t;
    }
    for (int s = 0; s < m; s++) {
        int idx = g->empties[s];
        int x = idx % g->cols;
        int y = idx / g->cols;
        int d = (int)(gol_rand_u32(g->rng) % 8u);
        int nx = gz_wrap(x + GZ_DX[d], g->cols);
        int ny = gz_wrap(y + GZ_DY[d], g->rows);
        int tidx = ny * g->cols + nx;
        if (g->living.cur[tidx] || g->zombies[tidx])
            continue; /* blocked: stay put */
        g->zombies[idx] = 0;
        g->zombies[tidx] = 1;
    }
}

golz_eat_action_t golz_eat_action(int living_neighbors) {
    switch (living_neighbors) {
    case 1:
    case 2:
        return GOLZ_EAT_ONE;
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
        return GOLZ_EAT_KILLED;
    default: /* 0, 3 */
        return GOLZ_EAT_NONE;
    }
}

int golz_spawn_count(int pct, int deaths) {
    if (deaths <= 0)
        return 0;
    return (pct * deaths + 99) / 100; /* ceil(pct/100 * deaths); 0 when pct==0 */
}

/* Eat/kill pass: counts read a frozen post-movement snapshot of the living
 * grid; mutations apply to the live grids. Reinfected cells go to z_new (act
 * next gen); other eaten cells are recorded as deaths. */
static void gz_eat_kill(golz_t *g) {
    size_t n = (size_t)g->cols * (size_t)g->rows;
    memcpy(g->snapshot, g->living.cur, n); /* R6 frozen snapshot */

    int m = 0;
    for (size_t i = 0; i < n; i++)
        if (g->zombies[i])
            g->empties[m++] = (int)i;
    for (int i = m - 1; i > 0; i--) { /* reshuffle the zombie turn order */
        int j = (int)(gol_rand_u32(g->rng) % (uint32_t)(i + 1));
        int t = g->empties[i];
        g->empties[i] = g->empties[j];
        g->empties[j] = t;
    }

    for (int s = 0; s < m; s++) {
        int idx = g->empties[s];
        int x = idx % g->cols;
        int y = idx / g->cols;
        int nbr[8];
        int cnt = 0;
        for (int d = 0; d < 8; d++) {
            int nx = gz_wrap(x + GZ_DX[d], g->cols);
            int ny = gz_wrap(y + GZ_DY[d], g->rows);
            int ti = ny * g->cols + nx;
            if (g->snapshot[ti])
                nbr[cnt++] = ti;
        }
        golz_eat_action_t act = golz_eat_action(cnt);
        if (act == GOLZ_EAT_ONE) {
            int pick = nbr[(int)(gol_rand_u32(g->rng) % (uint32_t)cnt)];
            if (g->living.cur[pick]) { /* double-eat guard: skip if already gone */
                g->living.cur[pick] = 0;
                if ((int)(gol_rand_u32(g->rng) % 100u) < g->cfg.zombie_reinfect)
                    g->z_new[pick] = 1;     /* R9 reinfect -> acts next gen */
                else
                    g->died_mask[pick] = 1; /* R9 death */
            }
        } else if (act == GOLZ_EAT_KILLED) {
            g->zombies[idx] = 0; /* R8 zombie killed */
        }
    }
}

/* Spawn-from-the-dead: deaths = this gen's recorded deaths (Conway + eaten,
 * reinfected excluded); on a successful roll, place ceil(pct/100 * deaths)
 * (>=1) new zombies on empty cells via the shared sampler, into z_new. */
static void gz_spawn(golz_t *g) {
    size_t n = (size_t)g->cols * (size_t)g->rows;
    int deaths = 0;
    for (size_t i = 0; i < n; i++)
        deaths += g->died_mask[i];
    if (deaths <= 0)
        return;
    if ((int)(gol_rand_u32(g->rng) % 100u) >= g->cfg.zombie_spawn_chance)
        return; /* R10 spawn roll missed */
    int pct = (int)(gol_rand_u32(g->rng) % 6u); /* 0..5 % */
    int want = golz_spawn_count(pct, deaths);
    int k = golz_sample_empty(g, want); /* R11 bounded placement */
    for (int i = 0; i < k; i++)
        g->z_new[g->empties[i]] = 1;
}

/* Maintain the zombie (red) fade trail, mirroring gol.c's trail handling: full
 * under an active zombie, fading otherwise (or instantly dark when trails off).
 * z_new (pending) zombies are not yet visible, so they get no trail. */
static void gz_update_ztrail(golz_t *g) {
    size_t n = (size_t)g->cols * (size_t)g->rows;
    int tt = g->living.cfg.trail_turns;
    if (tt < 1)
        tt = 1;
    bool trail = g->living.cfg.trail;
    for (size_t i = 0; i < n; i++) {
        if (g->zombies[i])
            g->z_trail[i] = (uint8_t)tt;
        else if (trail && g->z_trail[i] > 0)
            g->z_trail[i]--;
        else
            g->z_trail[i] = 0;
    }
}

uint32_t golz_compose_pixel(const golz_t *g, int x, int y) {
    int i = y * g->cols + x;
    int tt = g->living.cfg.trail_turns;
    /* A machete recolours the living faction's green channel to blue. Machetes are
     * static, so a fading living trail is blue iff this cell holds a machete. */
    bool machete = g->machetes[i];
    uint8_t r, gr, b;
    if (g->zombies[i]) {
        r = 255;
        gr = 0; /* live occupant overrides any trail */
        b = 0;
    } else if (g->living.cur[i]) {
        r = 0;
        gr = machete ? 0 : 255;
        b = machete ? 255 : 0;
    } else {
        uint8_t live = gol_channel_intensity(false, g->living.trail[i], tt);
        gr = machete ? 0 : live;
        b = machete ? live : 0;
        r = gol_channel_intensity(false, g->z_trail[i], tt);
    }
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)gr << 8) | (uint32_t)b;
}

void golz_step(golz_t *g) {
    if (!g || !g->zombies)
        return;
    size_t n = (size_t)g->cols * (size_t)g->rows;

    gz_promote(g);                            /* R12 deferred activation */
    memcpy(g->prev_living, g->living.cur, n); /* death diff baseline */
    memset(g->died_mask, 0, n);               /* only this gen's deaths */
    gz_living_turn(g);                        /* R5 Conway on living only */
    gz_machete(g);                            /* M3/M4 armed humans strike first */
    gz_move(g);                               /* R6/R7 movement pass */
    gz_eat_kill(g);                           /* R6/R8/R9 eat, reinfect */
    gz_spawn(g);                              /* R10/R11 spawn from the dead */
    gz_update_ztrail(g);                      /* R19 zombie fade trail */

    g->generation++;
}

golz_terminal_t golz_terminal(golz_t *g) {
    if (!g)
        return GOLZ_CONTINUE;
    int living = gol_live_count(&g->living);
    /* Record the living-only hash every generation so the cycle ring stays valid
     * regardless of which branch returns below. */
    bool cycled = gol_cycle_record(&g->cycle, &g->living);

    if (living == 0 && golz_zombie_count(g) > 0)
        return GOLZ_ZOMBIE_WIN; /* R13 win, checked before any other outcome */
    if (g->cfg.generations_to_win > 0 && living > 0 &&
        g->generation >= (uint32_t)g->cfg.generations_to_win)
        return GOLZ_HUMAN_WIN; /* M7 humans outlasted the zombies */
    if (cycled)
        return GOLZ_TIE; /* M8 living-only cycle/extinction -> scored tie */
    if (g->generation >= (uint32_t)g->cfg.max_generations)
        return GOLZ_TIE; /* R23 unconditional backstop -> tie */
    return GOLZ_CONTINUE; /* R15 keep running */
}

long golz_parse_wins(const char *s, long fallback) {
    if (!s || !*s)
        return fallback;
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < 0)
        return fallback; /* unparseable, trailing junk, or negative */
    return v;
}
