/**
 * @file golz_mc.c
 * Headless monte-carlo balance sweep for GoLZ machetes. Links the pure core
 * (src/golz.c + src/gol.c) only — no LVGL/Redis. For each (machete_percentage,
 * human_kill_zombie) point, runs many games that each sample the real per-round
 * randomized settings distribution (mirroring roll_settings() in src/modes/golz.c)
 * against a representative panel resolution, then reports Human/Zombie/Tie rates
 * and the decisive Human share H/(H+Z). Used to pick the mode's starting ranges.
 *
 * Build:  cmake --build build --target golz_mc   (or see CMakeLists.txt)
 * Run:    ./build/golz_mc [--res WxH] [--games N] [--gens G] [--seed S]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "golz.h"

typedef struct {
    int human;
    int zombie;
    int tie;
} tally_t;

/* One game: sample the real randomized settings, override the swept machete
 * knobs and the human-win threshold, then step to a terminal outcome. trail and
 * speed do not affect the win outcome, so they are minimised for speed. */
static golz_terminal_t run_game(uint32_t *rng, int disp_w, int disp_h,
                                int machete_pct, int human_kill, int gens) {
    gol_settings_t living;
    memset(&living, 0, sizeof(living));
    int cell_size = 2 + (int)(gol_rand_u32(rng) % 29); /* 2..30, like the mode */
    int padding = (int)(gol_rand_u32(rng) % 3);        /* 0..2 */
    if (padding >= cell_size)
        padding = cell_size - 1;
    living.cell_size = cell_size;
    living.padding = padding;
    living.density = 0.15 + (gol_rand_u32(rng) / 4294967295.0) * 0.35; /* 0.15..0.5 */
    living.trail = false;
    living.trail_turns = 1;
    living.speed_ms = 100;
    living.rgb = false;

    golz_settings_t z;
    memset(&z, 0, sizeof(z));
    z.initial_count = (int)(gol_rand_u32(rng) % 3);        /* 0..2 */
    z.zombie_reinfect = (int)(gol_rand_u32(rng) % 101);    /* 0..100 */
    z.zombie_spawn_chance = (int)(gol_rand_u32(rng) % 41); /* 0..40 */
    z.max_generations = gens + 5000; /* backstop well above the win threshold */
    z.machete_percentage = machete_pct;
    z.human_kill_zombie = human_kill;
    z.generations_to_win = gens;

    int block = cell_size + padding;
    if (block < 1)
        block = 1;
    int cols = disp_w / block;
    int rows = disp_h / block;
    if (cols < 1)
        cols = 1;
    if (rows < 1)
        rows = 1;

    golz_t g;
    if (!golz_init(&g, cols, rows, &living, &z, rng))
        return GOLZ_TIE;
    golz_seed(&g);
    golz_terminal_t t;
    do {
        golz_step(&g);
        t = golz_terminal(&g);
    } while (t == GOLZ_CONTINUE);
    golz_free(&g);
    return t;
}

static tally_t run_combo(uint32_t *rng, int disp_w, int disp_h, int machete_pct,
                         int human_kill, int gens, int games) {
    tally_t t = {0, 0, 0};
    for (int i = 0; i < games; i++) {
        switch (run_game(rng, disp_w, disp_h, machete_pct, human_kill, gens)) {
        case GOLZ_HUMAN_WIN:
            t.human++;
            break;
        case GOLZ_ZOMBIE_WIN:
            t.zombie++;
            break;
        default:
            t.tie++;
            break;
        }
    }
    return t;
}

/* End-to-end validation of the shipping game: draw machete_percentage and
 * human_kill_zombie uniformly from the mode's ranges each game, start the
 * adaptive threshold at `start_gens`, and apply the live +win/-loss steps
 * (floored) after every decisive game. Reports the converged threshold and the
 * steady-state outcome split over the back half of the run. */
static void run_adaptive(uint32_t *rng, int disp_w, int disp_h, int mp_lo,
                         int mp_hi, int hk_lo, int hk_hi, int start_gens,
                         int win_step, int loss_step, int floor, int games) {
    long gens = start_gens;
    long gens_min = start_gens, gens_max = start_gens, gens_sum = 0;
    tally_t all = {0, 0, 0};
    tally_t back = {0, 0, 0}; /* back half = steady state */
    for (int i = 0; i < games; i++) {
        int mp = mp_lo + (int)(gol_rand_u32(rng) % (uint32_t)(mp_hi - mp_lo + 1));
        int hk = hk_lo + (int)(gol_rand_u32(rng) % (uint32_t)(hk_hi - hk_lo + 1));
        golz_terminal_t r =
            run_game(rng, disp_w, disp_h, mp, hk, (int)gens);
        bool back_half = i >= games / 2;
        if (r == GOLZ_HUMAN_WIN) {
            all.human++;
            if (back_half)
                back.human++;
            gens += win_step;
        } else if (r == GOLZ_ZOMBIE_WIN) {
            all.zombie++;
            if (back_half)
                back.zombie++;
            gens -= loss_step;
            if (gens < floor)
                gens = floor;
        } else {
            all.tie++;
            if (back_half)
                back.tie++;
        }
        gens_sum += gens;
        if (gens < gens_min)
            gens_min = gens;
        if (gens > gens_max)
            gens_max = gens;
    }
    int bd = back.human + back.zombie;
    printf("\n== Adaptive loop  machete[%d..%d] kill[%d..%d] start_gens=%d "
           "(+%d/-%d, floor %d) ==\n",
           mp_lo, mp_hi, hk_lo, hk_hi, start_gens, win_step, loss_step, floor);
    printf("  games=%d  threshold: final=%ld  mean=%ld  range=[%ld..%ld]\n", games,
           gens, gens_sum / games, gens_min, gens_max);
    printf("  overall:   H=%.1f%%  Z=%.1f%%  T=%.1f%%\n", 100.0 * all.human / games,
           100.0 * all.zombie / games, 100.0 * all.tie / games);
    printf("  back-half: H=%.1f%%  Z=%.1f%%  T=%.1f%%   decisive H/(H+Z)=%.1f%%\n",
           100.0 * back.human / (games - games / 2),
           100.0 * back.zombie / (games - games / 2),
           100.0 * back.tie / (games - games / 2),
           bd ? 100.0 * back.human / bd : 0.0);
}

int main(int argc, char **argv) {
    int disp_w = 1920, disp_h = 440; /* rpidash2 panel */
    int games = 200;
    int gens = 250;
    uint32_t seed = 0xC0FFEEu;
    int adaptive = 0;
    int win_step = 3, loss_step = 3; /* mode default: symmetric -> 50/50 decisive */
    int mp_lo = 10, mp_hi = 25, hk_lo = 22, hk_hi = 38; /* shipped ranges */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--res") && i + 1 < argc) {
            sscanf(argv[++i], "%dx%d", &disp_w, &disp_h);
        } else if (!strcmp(argv[i], "--games") && i + 1 < argc) {
            games = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--gens") && i + 1 < argc) {
            gens = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            seed = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--adaptive")) {
            adaptive = 1;
        } else if (!strcmp(argv[i], "--machete") && i + 1 < argc) {
            sscanf(argv[++i], "%d-%d", &mp_lo, &mp_hi);
        } else if (!strcmp(argv[i], "--kill") && i + 1 < argc) {
            sscanf(argv[++i], "%d-%d", &hk_lo, &hk_hi);
        } else if (!strcmp(argv[i], "--steps") && i + 1 < argc) {
            sscanf(argv[++i], "%d/%d", &win_step, &loss_step);
        } else {
            fprintf(stderr,
                    "usage: %s [--res WxH] [--games N] [--gens G] [--seed S]\n"
                    "          [--adaptive --machete LO-HI --kill LO-HI "
                    "--steps WIN/LOSS]\n",
                    argv[0]);
            return 2;
        }
    }
    if (seed == 0)
        seed = 1;

    if (adaptive) {
        uint32_t rng = seed;
        printf("GoLZ adaptive validation  res=%dx%d  seed=0x%X\n", disp_w, disp_h,
               seed);
        run_adaptive(&rng, disp_w, disp_h, mp_lo, mp_hi, hk_lo, hk_hi, gens,
                     win_step, loss_step, 100, games);
        return 0;
    }

    const int mp[] = {0, 10, 20, 30, 40, 50, 60};
    const int hk[] = {0, 20, 30, 40, 50, 60, 70, 80};
    const int n_mp = (int)(sizeof(mp) / sizeof(mp[0]));
    const int n_hk = (int)(sizeof(hk) / sizeof(hk[0]));

    uint32_t rng = seed;

    printf("GoLZ machete monte-carlo  res=%dx%d  games/cell=%d  gens_to_win=%d  "
           "seed=0x%X\n",
           disp_w, disp_h, games, gens, seed);
    printf("Each cell samples the real randomized per-round settings "
           "(cell_size/density/zombies/reinfect/spawn).\n");
    fflush(stdout);

    /* Compute every cell once, store the tally, then print the three views. */
    tally_t grid[16][16];
    for (int i = 0; i < n_mp; i++) {
        for (int j = 0; j < n_hk; j++)
            grid[i][j] = run_combo(&rng, disp_w, disp_h, mp[i], hk[j], gens, games);
        printf("  ...row machete_%%=%d done\n", mp[i]);
        fflush(stdout);
    }

    printf("\n== Human win %%  (rows = machete_%%, cols = human_kill_%%) ==\n        ");
    for (int j = 0; j < n_hk; j++)
        printf("hk%-4d", hk[j]);
    printf("\n");
    for (int i = 0; i < n_mp; i++) {
        printf("mp%-4d  ", mp[i]);
        for (int j = 0; j < n_hk; j++)
            printf("%-6.1f", 100.0 * grid[i][j].human / games);
        printf("\n");
    }

    printf("\n== Decisive Human share H/(H+Z) %%  (50 = even match) ==\n        ");
    for (int j = 0; j < n_hk; j++)
        printf("hk%-4d", hk[j]);
    printf("\n");
    for (int i = 0; i < n_mp; i++) {
        printf("mp%-4d  ", mp[i]);
        for (int j = 0; j < n_hk; j++) {
            int dec = grid[i][j].human + grid[i][j].zombie;
            if (dec == 0)
                printf("%-6s", "-");
            else
                printf("%-6.1f", 100.0 * grid[i][j].human / dec);
        }
        printf("\n");
    }

    printf("\n== Tie %%  (board stabilised; neither side resolved) ==\n        ");
    for (int j = 0; j < n_hk; j++)
        printf("hk%-4d", hk[j]);
    printf("\n");
    for (int i = 0; i < n_mp; i++) {
        printf("mp%-4d  ", mp[i]);
        for (int j = 0; j < n_hk; j++)
            printf("%-6.1f", 100.0 * grid[i][j].tie / games);
        printf("\n");
    }

    return 0;
}
