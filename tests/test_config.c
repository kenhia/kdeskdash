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
    unsetenv("REDISCLI_AUTH");
    unsetenv("KDESKDASH_CONTROL_REDISCLI_AUTH");
    unsetenv("KDESKDASH_KVSCF_REDIS_HOST");
    unsetenv("KDESKDASH_KVSCF_REDIS_PORT");
    unsetenv("KVSCF_REDISCLI_AUTH");
    unsetenv("CLAUDE_REDISCLI_AUTH");
    unsetenv("KDESKDASH_TELEMETRY_REDIS_HOST");
    unsetenv("KDESKDASH_TELEMETRY_REDIS_PORT");
    unsetenv("KDESKDASH_CARD_REDIS_HOST");
    unsetenv("KDESKDASH_CARD_REDIS_PORT");
    unsetenv("KDESKDASH_CARD_REDISCLI_AUTH");
    unsetenv("KDESKDASH_CARD_NAME");
    unsetenv("KDESKDASH_QUICK_PAIRS");
    unsetenv("KDESKDASH_CMD_REDIS_HOST");
    unsetenv("KDESKDASH_CMD_REDIS_PORT");
    unsetenv("KDESKDASH_CMD_REDISCLI_AUTH");
    unsetenv("KDESKDASH_PANEL_HOST");
    unsetenv("KDESKDASH_STATE_FILE");
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
    setenv("REDISCLI_AUTH", "secret", 1);
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
    setenv("REDISCLI_AUTH", "central-password", 1);
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
    setenv("REDISCLI_AUTH", "central-password", 1);
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
    setenv("REDISCLI_AUTH", "central-password", 1);
    setenv("KDESKDASH_KVSCF_REDIS_HOST", "127.0.0.1", 1);
    setenv("KDESKDASH_KVSCF_REDIS_PORT", "6380", 1);
    setenv("KVSCF_REDISCLI_AUTH", "kvscf-password", 1);
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
    setenv("REDISCLI_AUTH", "board-password", 1);
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
    setenv("REDISCLI_AUTH", "board-password", 1);
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
    setenv("REDISCLI_AUTH", "board-password", 1);
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

/* Sprint 037's collision, and the reason the control handle has its own name.
 *
 * REDISCLI_AUTH is the fleet's name for the CENTRAL rpi53 password, and the
 * unit now reads /etc/khomelab/secrets.env, so on both panels that variable is
 * always set. The control Redis is this board's own passwordless instance on
 * 6379 — a Redis with no password configured answers AUTH with an ERROR, so if
 * this handle read the bare name it would stop connecting the moment the fleet
 * file arrived, taking remote control, last-mode persistence, GoL injection and
 * the screenshot trigger with it. Silently: the panel keeps drawing.
 *
 * This is the sprint-031 failure one variable to the left. That one cost a
 * sprint to diagnose; this test is why this one did not. */
static void test_control_ignores_the_fleet_password(void) {
    kdeskdash_config_t cfg;
    clear_env();
    setenv("REDISCLI_AUTH", "central-password", 1);
    load(&cfg);
    assert(cfg.redis_auth == NULL);
    printf("ok  control Redis sends no AUTH just because the fleet file is read\n");
}

/* ...and it still authenticates when its OWN name says to, so the escape hatch
 * for a board that one day password-protects its local instance is real. */
static void test_control_reads_its_own_name(void) {
    kdeskdash_config_t cfg;
    clear_env();
    setenv("REDISCLI_AUTH", "central-password", 1);
    setenv("KDESKDASH_CONTROL_REDISCLI_AUTH", "board-local", 1);
    load(&cfg);
    assert(cfg.redis_auth != NULL && strcmp(cfg.redis_auth, "board-local") == 0);
    printf("ok  control Redis reads KDESKDASH_CONTROL_REDISCLI_AUTH\n");
}

/* The collapse this slice exists for: two keys that were one secret become one
 * key. Telemetry and the claude feed are two connections to rpi53:6379, and the
 * service card rides the telemetry endpoint, so one fleet variable must serve
 * all three — with no per-panel line anywhere. */
static void test_one_fleet_password_serves_all_three_central_handles(void) {
    kdeskdash_config_t cfg;
    clear_env();
    setenv("REDISCLI_AUTH", "central-password", 1);
    load(&cfg);
    assert(cfg.telemetry_redis_auth != NULL &&
           strcmp(cfg.telemetry_redis_auth, "central-password") == 0);
    assert(cfg.claude_redis_auth != NULL &&
           strcmp(cfg.claude_redis_auth, "central-password") == 0);
    /* The card inherits it only because it resolves to the same endpoint — the
     * same-instance rule still does the work, it is just fed a fleet name. */
    assert(cfg.card_redis_auth != NULL &&
           strcmp(cfg.card_redis_auth, "central-password") == 0);
    printf("ok  one REDISCLI_AUTH serves telemetry, claude and the card\n");
}

/* The kvscf slot is the OTHER secret, and must not pick up the central one. On
 * both panels kvscf is the board's own 6380 instance, which has a different
 * password from rpi53 — measured different on the two boards, too. */
static void test_kvscf_reads_the_fleet_slot_name(void) {
    kdeskdash_config_t cfg;
    clear_env();
    setenv("REDISCLI_AUTH", "central-password", 1);
    setenv("KDESKDASH_CLAUDE_REDIS_HOST", "rpi53", 1);
    setenv("KDESKDASH_CLAUDE_REDIS_PORT", "6379", 1);
    setenv("KDESKDASH_KVSCF_REDIS_HOST", "127.0.0.1", 1);
    setenv("KDESKDASH_KVSCF_REDIS_PORT", "6380", 1);
    setenv("KVSCF_REDISCLI_AUTH", "board-6380-password", 1);
    load(&cfg);
    assert(cfg.kvscf_redis_auth != NULL &&
           strcmp(cfg.kvscf_redis_auth, "board-6380-password") == 0);
    assert(strcmp(cfg.claude_redis_auth, "central-password") == 0);
    printf("ok  kvscf reads KVSCF_REDISCLI_AUTH, distinct from the central one\n");
}

/* The two panels' 6380 instances are different services with different
 * passwords, and k-homelab's bin/check-secrets refuses one key name pointing at
 * two store entries — so the fleet publishes two names and each panel declares
 * one. Both shapes must resolve, from the same binary, with no per-host build.
 *
 * rpidash3's shape: the slot name is the one its manifest declares. */
static void test_kvscf_rpidash3_shape(void) {
    kdeskdash_config_t cfg;
    clear_env();
    setenv("REDISCLI_AUTH", "central-password", 1);
    setenv("KDESKDASH_CLAUDE_REDIS_HOST", "rpi53", 1);
    setenv("KDESKDASH_CLAUDE_REDIS_PORT", "6379", 1);
    setenv("KDESKDASH_KVSCF_REDIS_HOST", "127.0.0.1", 1);
    setenv("KDESKDASH_KVSCF_REDIS_PORT", "6380", 1);
    setenv("KVSCF_REDISCLI_AUTH", "redis-kvscf-password", 1);
    load(&cfg);
    assert(cfg.kvscf_redis_auth != NULL &&
           strcmp(cfg.kvscf_redis_auth, "redis-kvscf-password") == 0);
    printf("ok  rpidash3 shape: kvscf reads KVSCF_REDISCLI_AUTH\n");
}

/* rpidash2's shape: its instance is called redis-claude for historical reasons,
 * so its published key name is CLAUDE_REDISCLI_AUTH — and it is emphatically
 * NOT the claude feed's password, which is central's REDISCLI_AUTH. Getting
 * these two confused is the whole reason the fallback is commented as heavily
 * as it is. */
static void test_kvscf_rpidash2_shape(void) {
    kdeskdash_config_t cfg;
    clear_env();
    setenv("REDISCLI_AUTH", "central-password", 1);
    setenv("KDESKDASH_CLAUDE_REDIS_HOST", "rpi53", 1);
    setenv("KDESKDASH_CLAUDE_REDIS_PORT", "6379", 1);
    setenv("KDESKDASH_KVSCF_REDIS_HOST", "127.0.0.1", 1);
    setenv("KDESKDASH_KVSCF_REDIS_PORT", "6380", 1);
    setenv("CLAUDE_REDISCLI_AUTH", "redis-claude-password", 1);
    load(&cfg);
    assert(cfg.kvscf_redis_auth != NULL &&
           strcmp(cfg.kvscf_redis_auth, "redis-claude-password") == 0);
    /* ...and the claude FEED still authenticates against central, not this. */
    assert(strcmp(cfg.claude_redis_auth, "central-password") == 0);
    printf("ok  rpidash2 shape: kvscf reads CLAUDE_REDISCLI_AUTH, feed unaffected\n");
}

/* The slot name wins when a host somehow declares both — a defined tie-break
 * rather than whichever getenv happens to run first. */
static void test_kvscf_slot_name_wins_over_the_historical_one(void) {
    kdeskdash_config_t cfg;
    clear_env();
    setenv("KDESKDASH_CLAUDE_REDIS_HOST", "rpi53", 1);
    setenv("KDESKDASH_KVSCF_REDIS_HOST", "127.0.0.1", 1);
    setenv("KDESKDASH_KVSCF_REDIS_PORT", "6380", 1);
    setenv("KVSCF_REDISCLI_AUTH", "slot-name", 1);
    setenv("CLAUDE_REDISCLI_AUTH", "historical-name", 1);
    load(&cfg);
    assert(cfg.kvscf_redis_auth != NULL &&
           strcmp(cfg.kvscf_redis_auth, "slot-name") == 0);
    printf("ok  both declared: KVSCF_REDISCLI_AUTH wins\n");
}

/* --- commands from central (sprint 039) ---------------------------------- */

/* Both panels point telemetry at rpi53, which is where the control families
 * live, so neither device env file needs a command endpoint line. */
static void test_cmd_defaults_to_the_telemetry_endpoint(void) {
    kdeskdash_config_t cfg;
    clear_env();
    setenv("KDESKDASH_TELEMETRY_REDIS_HOST", "rpi53", 1);
    setenv("KDESKDASH_TELEMETRY_REDIS_PORT", "6379", 1);
    setenv("REDISCLI_AUTH", "fleet", 1);
    load(&cfg);
    assert(strcmp(cfg.cmd_redis_host, "rpi53") == 0);
    assert(cfg.cmd_redis_port == 6379);
    assert(cfg.cmd_redis_auth != NULL && strcmp(cfg.cmd_redis_auth, "fleet") == 0);
    printf("ok  cmd: defaults to the telemetry endpoint, auth included\n");
}

/* The sprint-031 rule, third instance of it: auth follows the ENDPOINT. */
static void test_cmd_different_endpoint_does_not_inherit_auth(void) {
    kdeskdash_config_t cfg;
    clear_env();
    setenv("KDESKDASH_TELEMETRY_REDIS_HOST", "rpi53", 1);
    setenv("REDISCLI_AUTH", "fleet", 1);
    setenv("KDESKDASH_CMD_REDIS_HOST", "127.0.0.1", 1);
    load(&cfg);
    assert(strcmp(cfg.cmd_redis_host, "127.0.0.1") == 0);
    assert(cfg.cmd_redis_auth == NULL);
    printf("ok  cmd: a different host does not inherit the telemetry password\n");
}

static void test_cmd_same_host_different_port_does_not_inherit_auth(void) {
    kdeskdash_config_t cfg;
    clear_env();
    setenv("KDESKDASH_TELEMETRY_REDIS_HOST", "rpi53", 1);
    setenv("KDESKDASH_TELEMETRY_REDIS_PORT", "6379", 1);
    setenv("REDISCLI_AUTH", "fleet", 1);
    setenv("KDESKDASH_CMD_REDIS_PORT", "6380", 1);
    load(&cfg);
    assert(cfg.cmd_redis_port == 6380);
    assert(cfg.cmd_redis_auth == NULL);
    printf("ok  cmd: a different port does not inherit the telemetry password\n");
}

static void test_cmd_explicit_auth_wins(void) {
    kdeskdash_config_t cfg;
    clear_env();
    setenv("KDESKDASH_TELEMETRY_REDIS_HOST", "rpi53", 1);
    setenv("REDISCLI_AUTH", "fleet", 1);
    setenv("KDESKDASH_CMD_REDISCLI_AUTH", "its-own", 1);
    load(&cfg);
    assert(cfg.cmd_redis_auth != NULL &&
           strcmp(cfg.cmd_redis_auth, "its-own") == 0);
    printf("ok  cmd: an explicit password wins over the inherited one\n");
}

/* The `{host}` segment. The measured case is rpidash2's kernel hostname
 * `rpiDash2` (korg WI 2277) against the fleet's `rpidash2`; config_load cannot
 * fake gethostname(), so the override path is what is asserted here and the
 * derivation itself is test_panel_cmd's. */
static void test_panel_host_override_is_normalized(void) {
    kdeskdash_config_t cfg;
    clear_env();
    setenv("KDESKDASH_PANEL_HOST", "rpiDash2.local", 1);
    load(&cfg);
    assert(strcmp(cfg.panel_host, "rpidash2") == 0);
    printf("ok  panel host: an override is lowercased and shortened too\n");
}

/* An unusable override disables the feed rather than guessing — panel_feed
 * refuses to open a handle on an empty host. */
static void test_panel_host_junk_disables_the_feed(void) {
    kdeskdash_config_t cfg;
    clear_env();
    setenv("KDESKDASH_PANEL_HOST", "not a host", 1);
    load(&cfg);
    assert(cfg.panel_host != NULL && cfg.panel_host[0] == '\0');
    printf("ok  panel host: junk yields \"\", which disables the feed\n");
}

/* Derived when unset: whatever this build host is called, it must at least be
 * a legal token and carry no upper case. */
static void test_panel_host_is_derived_when_unset(void) {
    kdeskdash_config_t cfg;
    clear_env();
    load(&cfg);
    for (const char *p = cfg.panel_host; *p; p++)
        assert(!(*p >= 'A' && *p <= 'Z'));
    printf("ok  panel host: derived from the hostname, never upper case\n");
}

static void test_state_path_default_and_override(void) {
    kdeskdash_config_t cfg;
    clear_env();
    load(&cfg);
    assert(strcmp(cfg.state_path, "/var/lib/kdeskdash/state") == 0);
    setenv("KDESKDASH_STATE_FILE", "/var/tmp/other-state", 1);
    load(&cfg);
    assert(strcmp(cfg.state_path, "/var/tmp/other-state") == 0);
    printf("ok  state file: default and override\n");
}

int main(void) {
    test_control_ignores_the_fleet_password();
    test_control_reads_its_own_name();
    test_one_fleet_password_serves_all_three_central_handles();
    test_kvscf_reads_the_fleet_slot_name();
    test_kvscf_rpidash3_shape();
    test_kvscf_rpidash2_shape();
    test_kvscf_slot_name_wins_over_the_historical_one();
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
    test_cmd_defaults_to_the_telemetry_endpoint();
    test_cmd_different_endpoint_does_not_inherit_auth();
    test_cmd_same_host_different_port_does_not_inherit_auth();
    test_cmd_explicit_auth_wins();
    test_panel_host_override_is_normalized();
    test_panel_host_junk_disables_the_feed();
    test_panel_host_is_derived_when_unset();
    test_state_path_default_and_override();
    printf("test_config: all passed\n");
    return 0;
}
