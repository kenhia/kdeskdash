/**
 * @file test_service_card.c
 * Host-only unit tests for the pure kpidash service-card builder (no Redis).
 *
 * The contract under test is kpidash's, and its failure mode is silence: a key
 * with the wrong segment count, or a payload missing a required field, renders
 * no card and logs nothing we can see. So the tests pin the exact key shape and
 * the exact field set rather than "something plausible".
 */
#include <stdio.h>
#include <string.h>

#include "service_card.h"

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static void eq(const char *got, const char *want, const char *what) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL: %s: got \"%s\", want \"%s\"\n", what, got, want);
        failures++;
    }
}

/* Count ':' — the dashboard's SCAN pattern needs exactly 4 segments. */
static int colons(const char *s) {
    int n = 0;
    for (; *s; s++)
        if (*s == ':')
            n++;
    return n;
}

int main(void) {
    /* --- short host --- */
    {
        char out[SERVICE_CARD_HOST_MAX];
        check(service_card_short_host("rpidash2", out, sizeof(out)), "plain host ok");
        eq(out, "rpidash2", "plain host");

        check(service_card_short_host("kai.local", out, sizeof(out)), "fqdn ok");
        eq(out, "kai", "fqdn truncated at first dot");

        check(service_card_short_host("a.b.c.d", out, sizeof(out)), "multi-dot ok");
        eq(out, "a", "multi-dot takes first label");

        /* WI 2277: rpidash2's own hostname is `rpiDash2`, capital D, and the
         * card key IS the card's identity. Mixed case is valid — it normalises
         * rather than degrading to the sentinel — so that the board reads
         * `deskdash:rpidash2` beside `deskdash:rpidash3` like the rest of the
         * fleet. Pinned because the cards have no TTL: if this ever regresses,
         * the panel grows a second card and the old one never expires. */
        check(service_card_short_host("rpiDash2", out, sizeof(out)), "mixed-case host ok");
        eq(out, "rpidash2", "mixed case lowercased");
        check(service_card_short_host("RPIDASH2", out, sizeof(out)), "upper host ok");
        eq(out, "rpidash2", "all upper lowercased");
        check(service_card_short_host("rpiDash2.local", out, sizeof(out)), "mixed fqdn ok");
        eq(out, "rpidash2", "fqdn truncated then lowercased");
        /* Digits, '-' and '_' are in the charset and must survive untouched. */
        check(service_card_short_host("Pi-Dash_2", out, sizeof(out)), "punct host ok");
        eq(out, "pi-dash_2", "non-alpha characters unchanged");

        /* Unusable hostnames degrade to the sentinel — a card with a wrong host
         * line beats no card at all. */
        check(service_card_short_host(NULL, out, sizeof(out)), "null host ok");
        eq(out, SERVICE_CARD_HOST_NONE, "null -> sentinel");
        check(service_card_short_host("", out, sizeof(out)), "empty host ok");
        eq(out, SERVICE_CARD_HOST_NONE, "empty -> sentinel");
        check(service_card_short_host(".leading", out, sizeof(out)), "leading dot ok");
        eq(out, SERVICE_CARD_HOST_NONE, "leading dot -> sentinel");
        check(service_card_short_host("has:colon", out, sizeof(out)), "colon host ok");
        eq(out, SERVICE_CARD_HOST_NONE, "colon -> sentinel (would break the key)");
        check(service_card_short_host("sp ace", out, sizeof(out)), "space host ok");
        eq(out, SERVICE_CARD_HOST_NONE, "space -> sentinel");

        /* Too small to hold even the sentinel. */
        char tiny[1];
        memcpy(tiny, "X", 1);
        check(!service_card_short_host("kai", tiny, 1), "outsz 1 rejected");
        check(!service_card_short_host("kai", out, 0), "zero outsz rejected");
        check(!service_card_short_host("kai", NULL, 16), "null out rejected");
    }

    /* --- key --- */
    {
        char key[SERVICE_CARD_KEY_MAX];
        check(service_card_key(SERVICE_CARD_NAME, "rpidash2", key, sizeof(key)), "key ok");
        eq(key, "kpidash:services:deskdash:rpidash2", "key shape");
        check(colons(key) == 3, "key has exactly 4 segments (3 colons)");

        /* The sentinel is a legal host segment. */
        check(service_card_key(SERVICE_CARD_NAME, SERVICE_CARD_HOST_NONE, key, sizeof(key)),
              "sentinel host key ok");
        eq(key, "kpidash:services:deskdash:_", "sentinel key shape");

        /* A suffixed name is how two instances on ONE host stay distinct
         * (the handoff's recommended option 1) — it must still be 4 segments. */
        check(service_card_key("deskdash-left", "kai", key, sizeof(key)), "suffixed name ok");
        eq(key, "kpidash:services:deskdash-left:kai", "suffixed name key shape");
        check(colons(key) == 3, "suffixed name still 4 segments");

        /* Anything that would add or drop a segment is refused outright. */
        char sentinel[SERVICE_CARD_KEY_MAX];
        memcpy(sentinel, "SENTINEL", 9);
        memcpy(key, "SENTINEL", 9);
        check(!service_card_key("bad:name", "kai", key, sizeof(key)), "colon in name rejected");
        check(memcmp(key, sentinel, 9) == 0, "rejected name left out untouched");
        check(!service_card_key("deskdash", "bad:host", key, sizeof(key)), "colon in host rejected");
        check(!service_card_key("", "kai", key, sizeof(key)), "empty name rejected");
        check(!service_card_key("deskdash", "", key, sizeof(key)), "empty host rejected");
        check(!service_card_key(NULL, "kai", key, sizeof(key)), "null name rejected");
        check(!service_card_key("deskdash", NULL, key, sizeof(key)), "null host rejected");

        char small[10];
        check(!service_card_key("deskdash", "kai", small, sizeof(small)), "truncation rejected");
    }

    /* --- payload --- */
    {
        char buf[SERVICE_CARD_PAYLOAD_MAX];
        check(service_card_payload(1743292800.789, "ok", "v0.4.2", "kai",
                                   SERVICE_CARD_ICON_DASHBOARD, buf, sizeof(buf)),
              "payload ok");
        /* Every required field present, and the optional ones we asked for. */
        check(strstr(buf, "\"ts\":1743292800.789") != NULL, "ts present, 3dp");
        check(strstr(buf, "\"state\":\"ok\"") != NULL, "state present");
        check(strstr(buf, "\"text\":\"v0.4.2\"") != NULL, "text present");
        check(strstr(buf, "\"host\":\"kai\"") != NULL, "host present");
        check(strstr(buf, "\"icon\":20") != NULL, "icon present");
        check(buf[0] == '{' && buf[strlen(buf) - 1] == '}', "payload is a JSON object");

        /* Optional fields really are optional. */
        check(service_card_payload(1.0, "down", "stopped", NULL, -1, buf, sizeof(buf)),
              "payload without host/icon ok");
        check(strstr(buf, "\"host\"") == NULL, "host omitted when NULL");
        check(strstr(buf, "\"icon\"") == NULL, "icon omitted when negative");
        check(strstr(buf, "\"state\":\"down\"") != NULL, "down state accepted");

        /* Every state kpidash defines, and nothing else. */
        check(service_card_payload(1.0, "unhealthy", "x", NULL, -1, buf, sizeof(buf)), "unhealthy");
        check(service_card_payload(1.0, "maintenance", "x", NULL, -1, buf, sizeof(buf)), "maintenance");
        check(service_card_payload(1.0, "unknown", "x", NULL, -1, buf, sizeof(buf)), "unknown");
        char sentinel[SERVICE_CARD_PAYLOAD_MAX];
        memcpy(sentinel, "SENTINEL", 9);
        memcpy(buf, "SENTINEL", 9);
        check(!service_card_payload(1.0, "green", "x", NULL, -1, buf, sizeof(buf)),
              "unknown state rejected");
        check(memcmp(buf, sentinel, 9) == 0, "rejected state left out untouched");
        check(!service_card_payload(1.0, NULL, "x", NULL, -1, buf, sizeof(buf)), "null state rejected");

        /* text is escaped: a version string is tame, but this is the one field
         * that carries free text, and an unescaped quote makes the payload
         * unparseable — which the dashboard skips silently. */
        check(service_card_payload(1.0, "ok", "a\"b\\c", NULL, -1, buf, sizeof(buf)),
              "quoted text ok");
        check(strstr(buf, "\"text\":\"a\\\"b\\\\c\"") != NULL, "quote and backslash escaped");
        check(service_card_payload(1.0, "ok", "a\nb\tc", NULL, -1, buf, sizeof(buf)),
              "control text ok");
        check(strstr(buf, "\\n") != NULL && strstr(buf, "\\t") != NULL, "newline/tab escaped");
        check(strchr(buf, '\n') == NULL, "no raw newline survives");

        /* NULL text is not the same as absent — kpidash requires the field. */
        check(service_card_payload(1.0, "ok", NULL, NULL, -1, buf, sizeof(buf)), "null text ok");
        check(strstr(buf, "\"text\":\"\"") != NULL, "null text -> empty string, field kept");

        /* No room: refuse rather than emit truncated (unparseable) JSON. */
        char small[16];
        check(!service_card_payload(1.0, "ok", "v0.4.2", "kai", 20, small, sizeof(small)),
              "truncation rejected");
        check(!service_card_payload(1.0, "ok", "x", NULL, -1, buf, 0), "zero outsz rejected");
        check(!service_card_payload(1.0, "ok", "x", NULL, -1, NULL, 16), "null out rejected");
    }

    /* --- throttle --- */
    {
        check(service_card_due(0, 1000, 15), "never published -> due");
        check(!service_card_due(1000, 1000, 15), "same second -> not due");
        check(!service_card_due(1000, 1014, 15), "14s elapsed -> not due");
        check(service_card_due(1000, 1015, 15), "15s elapsed -> due");
        check(service_card_due(1000, 9999, 15), "long gap -> due");
        /* A backwards step (NTP correction on a Pi with no RTC) must not wedge
         * the publisher until wall-clock catches up. */
        check(service_card_due(5000, 1000, 15), "clock went backwards -> due");
        /* The cadence must stay under kpidash's 60 s staleness cutoff. */
        check(SERVICE_CARD_INTERVAL_S < 60, "interval under the staleness cutoff");
    }

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_service_card: all passed\n");
    return 0;
}
