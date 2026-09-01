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

int main(void) {
    test_same_instance_inherits_everything();
    test_different_endpoint_does_not_inherit_auth();
    test_same_host_different_port_does_not_inherit_auth();
    test_explicit_auth_wins();
    test_no_auth_anywhere_stays_null();
    printf("test_config: all passed\n");
    return 0;
}
