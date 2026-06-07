/**
 * @file test_telemetry_host.c
 * Host-only unit tests for the pure telemetry host-token contract (no Redis).
 */
#include <stdio.h>
#include <string.h>

#include "telemetry_host.h"

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

/* Expect a key to be accepted and yield exactly `want` as the host. */
static void ok(const char *key, const char *want) {
    char out[128];
    bool got = telemetry_host_from_key(key, strlen(key), out, sizeof(out));
    if (!got) {
        fprintf(stderr, "FAIL: expected accept for \"%s\"\n", key);
        failures++;
        return;
    }
    if (strcmp(out, want) != 0) {
        fprintf(stderr, "FAIL: \"%s\" -> host \"%s\", want \"%s\"\n", key, out, want);
        failures++;
    }
}

/* Expect a key to be rejected (out must be left untouched). */
static void reject(const char *key) {
    char out[128];
    memcpy(out, "SENTINEL", 9);
    bool got = telemetry_host_from_key(key, strlen(key), out, sizeof(out));
    if (got) {
        fprintf(stderr, "FAIL: expected reject for \"%s\"\n", key);
        failures++;
    }
    if (memcmp(out, "SENTINEL", 9) != 0) {
        fprintf(stderr, "FAIL: \"%s\" rejected but clobbered out buffer\n", key);
        failures++;
    }
}

int main(void) {
    /* Happy paths. */
    ok("kpidash:client:kai:dev_telemetry", "kai");
    ok("kpidash:client:a:dev_telemetry", "a"); /* 1-char host */
    ok("kpidash:client:rpi-5_3.local:dev_telemetry", "rpi-5_3.local");

    /* Anchoring violations. */
    reject("kpidash:client::dev_telemetry");          /* empty host segment */
    reject("kpidash:client:kai:dev_telemetry:evil");  /* trailing garbage, no suffix anchor */
    reject("kpidash:client:kai");                      /* missing suffix */
    reject(":dev_telemetry");                          /* missing prefix + host */
    reject("kpidashXclient:kai:dev_telemetry");        /* corrupted prefix */
    reject("other:client:kai:dev_telemetry");          /* wrong prefix */
    reject("kpidash:client:dev_telemetry");            /* no host, suffix eats prefix tail */
    reject("");                                         /* empty */
    reject("foo");                                      /* unrelated */

    /* Charset violations (embedded ':' or illegal bytes in the host). */
    reject("kpidash:client:a:b:dev_telemetry"); /* embedded colon */
    reject("kpidash:client:ho st:dev_telemetry"); /* space */
    reject("kpidash:client:ho/st:dev_telemetry"); /* slash */
    reject("kpidash:client:ho*st:dev_telemetry"); /* glob char */

    /* Oversized host (64 chars > DEV_HOST_MAX-1 == 63). */
    {
        char key[256];
        char host64[65];
        memset(host64, 'x', 64);
        host64[64] = '\0';
        snprintf(key, sizeof(key), "kpidash:client:%s:dev_telemetry", host64);
        reject(key);

        /* Exactly 63 chars must be accepted. */
        char host63[64];
        memset(host63, 'x', 63);
        host63[63] = '\0';
        snprintf(key, sizeof(key), "kpidash:client:%s:dev_telemetry", host63);
        ok(key, host63);
    }

    /* Output buffer too small must reject, not truncate. */
    {
        char small[3]; /* room for 2 chars + NUL */
        bool got = telemetry_host_from_key(
            "kpidash:client:kai:dev_telemetry",
            strlen("kpidash:client:kai:dev_telemetry"), small, sizeof(small));
        check(!got, "outsz too small -> reject");
    }

    /* Defensive null/zero handling. */
    {
        char out[16];
        check(!telemetry_host_from_key(NULL, 5, out, sizeof(out)), "null key");
        check(!telemetry_host_from_key("k", 1, NULL, 16), "null out");
        check(!telemetry_host_from_key("k", 1, out, 0), "zero outsz");
    }

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_telemetry_host: all passed\n");
    return 0;
}
