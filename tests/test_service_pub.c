/**
 * @file test_service_pub.c
 * The service card's failure path, through the publisher's I/O seam (WI 2281).
 *
 * The card publishes from every panel to a Redis it only WRITES to, and the
 * claim is that losing that Redis never stalls the panel and the card comes
 * back by itself. This drives service_pub.c's real tick loop against a fake
 * that fails the Nth write — both ways a write fails: no reply at all (the
 * socket went away) and an error reply (the socket is fine, the server said
 * no) — and asserts the publisher backs off rather than hammering, never
 * counts a failure as a publish, and recovers without waiting out the card's
 * 15 s interval.
 *
 * No Redis, no socket, no sleeping: the fake owns the clock too.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "service_card.h"
#include "service_pub.h"
#include "service_pub_internal.h"

static int failures;

static void check(long got, long want, const char *what) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %ld, want %ld\n", what, got, want);
        failures++;
    }
}

/* ---- the fake -------------------------------------------------------- */

typedef enum { W_OK = 0, W_NO_REPLY, W_ERROR_REPLY } write_outcome_t;

#define BACKOFF_S 5 /* the fake's handle backoff; redis.c's is the same */

static struct {
    time_t now;
    bool   down;          /* endpoint unreachable: ensure() says no        */
    bool   ctx_err;       /* last command left the context errored         */
    time_t backoff_until; /* the handle is backing off until this second   */
    int    set_calls;
    int    drop_calls;
    int    allocs, frees;
    write_outcome_t fail_on[32]; /* outcome of the Nth set (1-based); 0 = OK */
    char   last_payload[SERVICE_CARD_PAYLOAD_MAX];
    char   last_key[SERVICE_CARD_KEY_MAX];
} F;

/* Models redis_client_ensure: an errored context is freed and the handle
 * backs off; while backing off or unreachable, no connection. */
static bool fake_ensure(redis_client_t *c) {
    (void)c;
    if (F.ctx_err) {
        F.ctx_err = false;
        F.backoff_until = F.now + BACKOFF_S;
        return false;
    }
    if (F.now < F.backoff_until)
        return false;
    if (F.down) {
        F.backoff_until = F.now + BACKOFF_S;
        return false;
    }
    return true;
}

static redisReply *fake_set(redis_client_t *c, const char *key, const char *payload) {
    (void)c;
    F.set_calls++;
    snprintf(F.last_key, sizeof(F.last_key), "%s", key);
    snprintf(F.last_payload, sizeof(F.last_payload), "%s", payload);
    write_outcome_t o = F.set_calls < 32 ? F.fail_on[F.set_calls] : W_OK;
    if (o == W_NO_REPLY) {
        F.ctx_err = true; /* what hiredis does when a command cannot complete */
        return NULL;
    }
    redisReply *r = calloc(1, sizeof(*r));
    F.allocs++;
    r->type = (o == W_ERROR_REPLY) ? REDIS_REPLY_ERROR : REDIS_REPLY_STATUS;
    return r;
}

static void fake_free(void *reply) {
    if (!reply)
        return;
    F.frees++;
    free(reply);
}

static void fake_drop(redis_client_t *c) {
    (void)c;
    F.drop_calls++;
    F.backoff_until = F.now + BACKOFF_S;
}

static time_t fake_now(void) { return F.now; }

static const service_pub_io_t FAKE = {
    .ensure = fake_ensure,
    .set = fake_set,
    .free_reply = fake_free,
    .drop = fake_drop,
    .now = fake_now,
};

/* A main loop runs this many ticks a second; the exact number does not matter,
 * only that it is many — the failure mode being guarded is "once per tick". */
#define TICKS_PER_S 50

static void run_seconds(time_t from, time_t to) {
    for (time_t t = from; t < to; t++) {
        F.now = t;
        for (int i = 0; i < TICKS_PER_S; i++)
            service_pub_tick();
    }
}

static void reset(void) {
    memset(&F, 0, sizeof(F));
    F.now = 1000;
    service_pub_set_io(&FAKE);
    service_pub_init("127.0.0.1", 6379, NULL, NULL, "0.0.0-test");
}

/* ---- tests ------------------------------------------------------------ */

static void test_seam_defaults_to_real(void) {
    service_pub_set_io(NULL);
    check(service_pub_io() == service_pub_io_real(), 1, "NULL restores the real seam");
    service_pub_set_io(&FAKE);
    check(service_pub_io() == &FAKE, 1, "a fake installs");
    service_pub_set_io(NULL);
}

static void test_steady_state(void) {
    reset();
    run_seconds(1000, 1060);
    /* 1000, 1015, 1030, 1045 — once per interval, not once per tick. */
    check(F.set_calls, 4, "steady state: one write per 15 s interval");
    check(service_pub_published_count(), 4, "steady state: every write counted");
    check(service_pub_failing(), 0, "steady state: not failing");
    check(strstr(F.last_payload, "\"state\":\"ok\"") != NULL, 1, "payload says ok");
}

static void test_no_reply_backs_off_then_recovers(void) {
    reset();
    F.fail_on[2] = W_NO_REPLY; /* the write at 1015 */
    run_seconds(1000, 1015);
    check(F.set_calls, 1, "no-reply: first write at 1000");

    run_seconds(1015, 1020);
    check(F.set_calls, 2, "no-reply: ONE failed write, then quiet through the backoff");
    check(service_pub_published_count(), 1, "no-reply: failure not counted as a publish");
    check(service_pub_failing(), 1, "no-reply: failing");

    /* Backoff over at 1020: publishes straight away rather than waiting until
     * 1030 — the reconnect lands and the card goes out on it. */
    run_seconds(1020, 1021);
    check(F.set_calls, 3, "no-reply: recovers the moment the backoff ends");
    check(service_pub_published_count(), 2, "no-reply: recovery counted");
    check(service_pub_failing(), 0, "no-reply: no longer failing");
}

static void test_error_reply_backs_off_then_recovers(void) {
    reset();
    F.fail_on[2] = W_ERROR_REPLY; /* -NOAUTH / -MISCONF / -READONLY at 1015 */
    run_seconds(1000, 1015);

    run_seconds(1015, 1020);
    check(F.set_calls, 2, "error-reply: ONE failed write, not one per tick");
    check(service_pub_published_count(), 1, "error-reply: NOT counted as a publish");
    check(service_pub_failing(), 1, "error-reply: failing");
    check(F.drop_calls, 1, "error-reply: the handle is dropped into backoff");

    run_seconds(1020, 1021);
    check(F.set_calls, 3, "error-reply: retried once the backoff ends");
    check(service_pub_published_count(), 2, "error-reply: recovery counted");
}

static void test_long_outage(void) {
    reset();
    run_seconds(1000, 1001);
    check(F.set_calls, 1, "outage: published before it");

    /* A minute and a half down. Every tick asks; the handle says no; nothing
     * is written, because there is nothing to write to. */
    F.down = true;
    run_seconds(1001, 1090);
    check(F.set_calls, 1, "outage: no write attempted while unreachable");
    check(service_pub_published_count(), 1, "outage: nothing counted");

    F.down = false;
    run_seconds(1090, 1096);
    check(F.set_calls, 2, "outage: exactly one write once reachable");
    check(service_pub_published_count(), 2, "outage: card back");
}

static void test_repeated_failures_stay_bounded(void) {
    reset();
    /* Every write from the 2nd to the 20th fails, alternating the two ways. */
    for (int n = 2; n <= 20; n++)
        F.fail_on[n] = (n % 2) ? W_ERROR_REPLY : W_NO_REPLY;
    run_seconds(1000, 1100);
    /* At most one attempt per backoff window: 100 s / 5 s = 20, plus the
     * first. Anything near TICKS_PER_S * 100 is the hammering bug. */
    check(F.set_calls <= 21, 1, "persistent failure: at most one write per backoff");
    check(service_pub_published_count(), 1, "persistent failure: only the first counted");
    check(F.allocs, F.frees, "every reply the seam made, the seam freed");
}

static void test_shutdown(void) {
    reset();
    run_seconds(1000, 1001);
    int before = F.set_calls;
    service_pub_shutdown();
    check(F.set_calls, before + 1, "shutdown: one best-effort write");
    check(strstr(F.last_payload, "\"state\":\"down\"") != NULL, 1, "shutdown: says down");

    reset();
    F.down = true;
    service_pub_shutdown();
    check(F.set_calls, 0, "shutdown while unreachable: no write, no wait");
    check(F.allocs, F.frees, "shutdown: replies freed");
}

int main(void) {
    test_seam_defaults_to_real();
    test_steady_state();
    test_no_reply_backs_off_then_recovers();
    test_error_reply_backs_off_then_recovers();
    test_long_outage();
    test_repeated_failures_stay_bounded();
    test_shutdown();
    service_pub_set_io(NULL);
    if (failures) {
        fprintf(stderr, "test_service_pub: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_service_pub: all passed\n");
    return 0;
}
