/**
 * @file test_config.c
 * config_load()'s endpoint fallbacks — the kvscf-vs-claude auth rule above all.
 *
 * The rule is not a nicety. rpidash2 ran one Redis serving both the claude feed
 * and kvscf, so kvscf inheriting the claude endpoint (including its password)
 * was right. Sprint 031 moved the claude feed to the central Redis, which has a
 * password, and kvscf stayed on the local instance, which has none — and a
 * Redis with no password configured answers AUTH with an ERROR, so the
 * inherited password silently broke the handle. Pinning host and port was not
 * enough, because nothing an env file could say meant "this one takes none".
 */
#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every variable config_load reads for the two endpoints under test, cleared so
 * each case states its whole world. */
static void clear_env(void) {
    unsetenv("KDESKDASH_CLAUDE_REDIS_HOST");
    unsetenv("KDESKDASH_CLAUDE_REDIS_PORT");
    unsetenv("KDESKDASH_CLAUDE_REDISCLI_AUTH");
    unsetenv("KDESKDASH_KVSCF_REDIS_HOST");
    unsetenv("KDESKDASH_KVSCF_REDIS_PORT");
    unsetenv("KDESKDASH_KVSCF_REDISCLI_AUTH");
    unsetenv("KDESKDASH_TELEMETRY_REDIS_HOST");
    unsetenv("KDESKDASH_TELEMETRY_REDIS_PORT");
    unsetenv("KDESKDASH_TELEMETRY_REDISCLI_AUTH");
    unsetenv("KDESKDASH_CARD_REDIS_HOST");
    unsetenv("KDESKDASH_CARD_REDIS_PORT");
    unsetenv("KDESKDASH_CARD_REDISCLI_AUTH");
    unsetenv("KDESKDASH_CARD_NAME");
    unsetenv("KDESKDASH_QUICK_PAIRS");
}

static void load(kdeskdash_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    config_load(cfg);
}

/* The historical rpidash2: one instance, both feeds, unset kvscf vars. */
static void test_same_instance_inherits_everything(void) {
    kdeskdash_config_t cfg;
    clear_env();
    setenv("KDESKDASH_CLAUDE_REDIS_HOST", "127.0.0.1", 1);
    setenv("KDESKDASH_CLAUDE_REDIS_PORT", "6380", 1);
    setenv("KDESKDASH_CLAUDE_REDISCLI_AUTH", "secret", 1);
    load(&cfg);
    assert(strcmp(cfg.kvscf_redis_host, "127.0.0.1") == 0);
    assert(cfg.kvscf_redis_port == 6380);
    assert(cfg.kvscf_redis_auth != NULL && strcmp(cfg.kvscf_redis_auth, "secret") == 0);
    printf("ok  same instance: kvscf inherits host, port and auth\n");
}

/* Sprint 031's rpidash2: claude moved to an authenticated central Redis, kvscf
 * pinned to the passwordless local one. The auth must NOT come along. */
static void test_different_endpoint_does_not_inherit_auth(void) {
    kdeskdash_config_t cfg;
    clear_env();
    setenv("KDESKDASH_CLAUDE_REDIS_HOST", "rpi53", 1);
    setenv("KDESKDASH_CLAUDE_REDIS_PORT", "6379", 1);
    setenv("KDESKDASH_CLAUDE_REDISCLI_AUTH", "central-password", 1);
    setenv("KDESKDASH_KVSCF_REDIS_HOST", "127.0.0.1", 1);
    setenv("KDESKDASH_KVSCF_REDIS_PORT", "6380", 1);
    load(&cfg);
    assert(strcmp(cfg.kvscf_redis_host, "127.0.0.1") == 0);
    assert(cfg.kvscf_redis_port == 6380);
    assert(cfg.kvscf_redis_auth == NULL);
    printf("ok  different endpoint: kvscf sends no inherited password\n");
}

/* A differing PORT alone is a different instance — rpidash3's shape before it
 * also moved hosts. */
static void test_same_host_different_port_does_not_inherit_auth(void) {
    kdeskdash_config_t cfg;
    clear_env();
    setenv("KDESKDASH_CLAUDE_REDIS_HOST", "127.0.0.1", 1);
    setenv("KDESKDASH_CLAUDE_REDIS_PORT", "6379", 1);
    setenv("KDESKDASH_CLAUDE_REDISCLI_AUTH", "central-password", 1);
    setenv("KDESKDASH_KVSCF_REDIS_PORT", "6380", 1);
    load(&cfg);
    assert(strcmp(cfg.kvscf_redis_host, "127.0.0.1") == 0);
    assert(cfg.kvscf_redis_port == 6380);
    assert(cfg.kvscf_redis_auth == NULL);
    printf("ok  same host, different port: still a different instance\n");
}

/* An explicit password always wins, whatever the endpoints say — rpidash3 reads
 * the fleet claude feed and drives a kvscf that does require one. */
static void test_explicit_auth_wins(void) {
    kdeskdash_config_t cfg;
    clear_env();
    setenv("KDESKDASH_CLAUDE_REDIS_HOST", "rpi53", 1);
    setenv("KDESKDASH_CLAUDE_REDIS_PORT", "6379", 1);
    setenv("KDESKDASH_CLAUDE_REDISCLI_AUTH", "central-password", 1);
    setenv("KDESKDASH_KVSCF_REDIS_HOST", "127.0.0.1", 1);
    setenv("KDESKDASH_KVSCF_REDIS_PORT", "6380", 1);
    setenv("KDESKDASH_KVSCF_REDISCLI_AUTH", "kvscf-password", 1);
    load(&cfg);
    assert(cfg.kvscf_redis_auth != NULL &&
           strcmp(cfg.kvscf_redis_auth, "kvscf-password") == 0);
    printf("ok  an explicit kvscf password wins over both fallbacks\n");
}

/* Same instance and neither has a password: nothing is invented. */
static void test_no_auth_anywhere_stays_null(void) {
    kdeskdash_config_t cfg;
    clear_env();
    setenv("KDESKDASH_CLAUDE_REDIS_HOST", "127.0.0.1", 1);
    setenv("KDESKDASH_CLAUDE_REDIS_PORT", "6380", 1);
    load(&cfg);
    assert(cfg.claude_redis_auth == NULL);
    assert(cfg.kvscf_redis_auth == NULL);
    printf("ok  no password anywhere: both handles stay unauthenticated\n");
}

/* The service card writes to the same Redis the telemetry feed reads, so with
 * nothing set it must land on kpidash's board (rpi53:6379) unaided. */
static void test_card_defaults_to_the_telemetry_endpoint(void) {
    kdeskdash_config_t cfg;
    clear_env();
    load(&cfg);
    assert(strcmp(cfg.card_redis_host, cfg.telemetry_redis_host) == 0);
    assert(cfg.card_redis_port == cfg.telemetry_redis_port);
    assert(strcmp(cfg.card_redis_host, "rpi53") == 0);
    assert(cfg.card_redis_port == 6379);
    assert(strcmp(cfg.card_name, "deskdash") == 0);
    printf("ok  card defaults to the telemetry endpoint (rpi53:6379, deskdash)\n");
}

/* Same instance: inheriting the telemetry password is right. */
static void test_card_same_instance_inherits_auth(void) {
    kdeskdash_config_t cfg;
    clear_env();
    setenv("KDESKDASH_TELEMETRY_REDIS_HOST", "rpi53", 1);
    setenv("KDESKDASH_TELEMETRY_REDIS_PORT", "6379", 1);
    setenv("KDESKDASH_TELEMETRY_REDISCLI_AUTH", "board-password", 1);
    load(&cfg);
    assert(cfg.card_redis_auth != NULL &&
           strcmp(cfg.card_redis_auth, "board-password") == 0);
    printf("ok  card same instance: inherits the telemetry password\n");
}

/* Different endpoint: the sprint-031 rule again. A card pointed at another
 * Redis must not carry the telemetry password to it — an unauthenticated
 * instance answers AUTH with an ERROR, and the card would simply never appear
 * while every log said "endpoint down". */
static void test_card_different_endpoint_does_not_inherit_auth(void) {
    kdeskdash_config_t cfg;
    clear_env();
    setenv("KDESKDASH_TELEMETRY_REDIS_HOST", "rpi53", 1);
    setenv("KDESKDASH_TELEMETRY_REDIS_PORT", "6379", 1);
    setenv("KDESKDASH_TELEMETRY_REDISCLI_AUTH", "board-password", 1);
    setenv("KDESKDASH_CARD_REDIS_HOST", "127.0.0.1", 1);
    load(&cfg);
    assert(strcmp(cfg.card_redis_host, "127.0.0.1") == 0);
    assert(cfg.card_redis_port == 6379); /* port still falls back independently */
    assert(cfg.card_redis_auth == NULL);
    printf("ok  card different endpoint: password does not travel\n");
}

/* An explicit card password wins over both fallbacks, and a distinct name is
 * how two instances on ONE host avoid clobbering each other's key. */
static void test_card_explicit_overrides(void) {
    kdeskdash_config_t cfg;
    clear_env();
    setenv("KDESKDASH_TELEMETRY_REDISCLI_AUTH", "board-password", 1);
    setenv("KDESKDASH_CARD_REDIS_HOST", "127.0.0.1", 1);
    setenv("KDESKDASH_CARD_REDIS_PORT", "6381", 1);
    setenv("KDESKDASH_CARD_REDISCLI_AUTH", "card-password", 1);
    setenv("KDESKDASH_CARD_NAME", "deskdash-left", 1);
    load(&cfg);
    assert(cfg.card_redis_port == 6381);
    assert(cfg.card_redis_auth != NULL &&
           strcmp(cfg.card_redis_auth, "card-password") == 0);
    assert(strcmp(cfg.card_name, "deskdash-left") == 0);
    printf("ok  explicit card endpoint, password and name all win\n");
}

/* The quick switch has three reachable states and config_load must keep them
 * distinguishable — it passes the spec through verbatim, because quickswitch.c
 * owns the grammar (the same split as modeset.c and KDESKDASH_MODES). The state
 * that matters is "none": without it the gesture would be unconditional, and a
 * default nobody can opt out of is not a default. */
static void test_quick_pairs_three_states(void) {
    kdeskdash_config_t cfg;

    clear_env();
    load(&cfg);
    assert(cfg.quick_pairs == NULL); /* unset -> previously-active fallback */

    clear_env();
    setenv("KDESKDASH_QUICK_PAIRS", "none", 1);
    load(&cfg);
    assert(cfg.quick_pairs != NULL && strcmp(cfg.quick_pairs, "none") == 0);

    clear_env();
    setenv("KDESKDASH_QUICK_PAIRS", "claude:foreground,clock:calc", 1);
    load(&cfg);
    assert(cfg.quick_pairs != NULL &&
           strcmp(cfg.quick_pairs, "claude:foreground,clock:calc") == 0);

    /* Empty is not "none" — env_or treats it as unset, which is the fallback
     * state, not the off state. */
    clear_env();
    setenv("KDESKDASH_QUICK_PAIRS", "", 1);
    load(&cfg);
    assert(cfg.quick_pairs == NULL);

    printf("ok  quick pairs: unset, \"none\" and pairs stay distinguishable\n");
}

int main(void) {
    test_same_instance_inherits_everything();
    test_different_endpoint_does_not_inherit_auth();
    test_same_host_different_port_does_not_inherit_auth();
    test_explicit_auth_wins();
    test_no_auth_anywhere_stays_null();
    test_card_defaults_to_the_telemetry_endpoint();
    test_card_same_instance_inherits_auth();
    test_card_different_endpoint_does_not_inherit_auth();
    test_card_explicit_overrides();
    test_quick_pairs_three_states();
    printf("test_config: all passed\n");
    return 0;
}
