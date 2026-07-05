/**
 * @file test_dev_view.c
 * Host-only unit tests for the pure dev-mode view logic: the liveness
 * precedence ladder and the CPU-only layout debounce.
 */
#include <stdio.h>

#include "modes/dev_view.h"

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

#define STALE_MS 10000

/* Convenience: resolve with all-default "live" inputs overridden per field. */
static dev_side_view_t resolve(bool tok, bool assigned, bool seen, bool live,
                               uint32_t ms_since_ok) {
    dev_side_inputs_t in = {
        .telemetry_ok = tok,
        .assigned = assigned,
        .seen_in_discovery = seen,
        .ever_live = live,
        .ms_since_ok = ms_since_ok,
    };
    return dev_side_resolve(&in, STALE_MS);
}

static void test_ladder_precedence(void) {
    /* (1) unavailable supersedes everything, even a fully live side. */
    check(resolve(false, true, true, true, 0) == DEV_SIDE_UNAVAIL,
          "endpoint down -> UNAVAIL even when otherwise live");
    check(resolve(false, false, false, false, 0) == DEV_SIDE_UNAVAIL,
          "endpoint down + empty -> UNAVAIL (global wins over empty)");

    /* (2) empty when reachable but no assignment. */
    check(resolve(true, false, false, false, 0) == DEV_SIDE_EMPTY,
          "reachable + unassigned -> EMPTY");

    /* (3) assigned but not in discovery -> offline (R18), regardless of
     * staleness or prior liveness. */
    check(resolve(true, true, false, false, 0) == DEV_SIDE_OFFLINE,
          "assigned + never seen -> OFFLINE");
    check(resolve(true, true, false, true, 999999) == DEV_SIDE_OFFLINE,
          "assigned + vanished from discovery -> OFFLINE wins over STALE");

    /* (4) stale: was live, in discovery, samples stopped. */
    check(resolve(true, true, true, true, STALE_MS) == DEV_SIDE_STALE,
          "live then quiet >= stale_ms -> STALE");
    check(resolve(true, true, true, true, STALE_MS + 5000) == DEV_SIDE_STALE,
          "well past threshold -> STALE");

    /* (5) live: fresh samples. */
    check(resolve(true, true, true, true, 0) == DEV_SIDE_LIVE,
          "fresh sample -> LIVE");
    check(resolve(true, true, true, true, STALE_MS - 1) == DEV_SIDE_LIVE,
          "just under threshold -> still LIVE");
}

static void test_assigned_not_yet_live(void) {
    /* Assigned + discovered + reachable, but no valid sample has ever arrived:
     * WAITING, not a flat-zero LIVE chart that would look like a healthy 0% host
     * (#52). Never STALE either — ms_since_ok is meaningless before first live. */
    check(resolve(true, true, true, false, 0) == DEV_SIDE_WAITING,
          "assigned + seen + no sample yet -> WAITING");
    check(resolve(true, true, true, false, 999999) == DEV_SIDE_WAITING,
          "never-live host is WAITING, never STALE");

    /* UNAVAIL/OFFLINE still supersede WAITING (endpoint/discovery come first). */
    check(resolve(false, true, true, false, 0) == DEV_SIDE_UNAVAIL,
          "endpoint down -> UNAVAIL wins over WAITING");
    check(resolve(true, true, false, false, 0) == DEV_SIDE_OFFLINE,
          "not in discovery -> OFFLINE wins over WAITING");
}

static void test_waiting_to_live_transition(void) {
    /* The first OK sample flips ever_live true; the same side then resolves LIVE
     * on the next poll (the render path drops a gap marker on that transition). */
    check(resolve(true, true, true, false, 0) == DEV_SIDE_WAITING,
          "before first sample -> WAITING");
    check(resolve(true, true, true, true, 0) == DEV_SIDE_LIVE,
          "after first OK sample -> LIVE");
}

static void test_gpu_gate_loss_debounce(void) {
    dev_gpu_gate_t g;
    dev_gpu_gate_init(&g);
    check(g.cpu_only == false, "gate starts in two-chart layout");

    /* Need N=3 consecutive absent samples before collapsing. */
    check(dev_gpu_gate_update(&g, false, 3) == false, "absent 1/3 -> not yet");
    check(dev_gpu_gate_update(&g, false, 3) == false, "absent 2/3 -> not yet");
    check(dev_gpu_gate_update(&g, false, 3) == true, "absent 3/3 -> cpu-only");
    check(dev_gpu_gate_update(&g, false, 3) == true, "still absent -> stays cpu-only");
}

static void test_gpu_gate_blip_does_not_flap(void) {
    dev_gpu_gate_t g;
    dev_gpu_gate_init(&g);
    check(dev_gpu_gate_update(&g, false, 3) == false, "absent 1");
    check(dev_gpu_gate_update(&g, false, 3) == false, "absent 2");
    /* One GPU sample resets the run before the threshold: no collapse. */
    check(dev_gpu_gate_update(&g, true, 3) == false, "gpu blip resets run");
    check(dev_gpu_gate_update(&g, false, 3) == false, "absent 1 again");
    check(dev_gpu_gate_update(&g, false, 3) == false, "absent 2 again");
    check(dev_gpu_gate_update(&g, false, 3) == true, "absent 3 again -> cpu-only");
}

static void test_gpu_gate_gain_path(void) {
    dev_gpu_gate_t g;
    dev_gpu_gate_init(&g);
    /* Collapse to cpu-only... */
    dev_gpu_gate_update(&g, false, 2);
    check(dev_gpu_gate_update(&g, false, 2) == true, "collapsed to cpu-only");
    /* ...then a single GPU sample restores two charts immediately. */
    check(dev_gpu_gate_update(&g, true, 2) == false, "gpu returns -> two charts at once");
}

int main(void) {
    test_ladder_precedence();
    test_assigned_not_yet_live();
    test_waiting_to_live_transition();
    test_gpu_gate_loss_debounce();
    test_gpu_gate_blip_does_not_flap();
    test_gpu_gate_gain_path();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("all dev_view tests passed\n");
    return 0;
}
