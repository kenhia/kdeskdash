/**
 * @file quickswitch.c
 * Pure core for the double-tap quick switch. See quickswitch.h.
 */
#include "quickswitch.h"

#include <stdio.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

static void copy_id(char *dst, const char *src) {
    snprintf(dst, QUICKSWITCH_ID_MAX, "%s", src);
}

/* Copy one `a:b` entry, rejecting anything that would not round-trip: an empty
 * side, a missing or extra ':', or an id too long for the buffer. */
static bool parse_entry(const char *tok, size_t len, char *a, char *b) {
    const char *colon = memchr(tok, ':', len);
    if (!colon)
        return false;
    size_t alen = (size_t)(colon - tok);
    size_t blen = len - alen - 1;
    if (alen == 0 || blen == 0)
        return false;
    if (alen >= QUICKSWITCH_ID_MAX || blen >= QUICKSWITCH_ID_MAX)
        return false;
    if (memchr(colon + 1, ':', blen))
        return false; /* "a:b:c" is a typo, not a triple */
    memcpy(a, tok, alen);
    a[alen] = '\0';
    memcpy(b, colon + 1, blen);
    b[blen] = '\0';
    return true;
}

/* Whole-spec off switch. Checked before the pair grammar so "none" can never
 * also be read as a malformed pair and warned about. */
static bool spec_is_off(const char *spec) {
    const char *b = spec;
    while (*b == ' ' || *b == '\t')
        b++;
    const char *e = b + strlen(b);
    while (e > b && (e[-1] == ' ' || e[-1] == '\t'))
        e--;
    size_t n = (size_t)(e - b);
    char buf[8];
    if (n == 0 || n >= sizeof(buf))
        return false;
    memcpy(buf, b, n);
    buf[n] = '\0';
    return strcasecmp(buf, "none") == 0 || strcasecmp(buf, "off") == 0;
}

void quickswitch_init(quickswitch_t *q, const char *spec) {
    if (!q)
        return;
    memset(q, 0, sizeof(*q));
    if (!spec || spec[0] == '\0')
        return;
    if (spec_is_off(spec)) {
        q->disabled = true;
        return;
    }

    const char *p = spec;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        /* Trim surrounding spaces so "a:b, c:d" behaves as it looks. */
        while (len > 0 && (*p == ' ' || *p == '\t')) {
            p++;
            len--;
        }
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t'))
            len--;

        if (len > 0) {
            if (q->pair_count >= QUICKSWITCH_MAX_PAIRS) {
                fprintf(stderr,
                        "kdeskdash: quick-switch pairs full (%d) — ignoring the rest\n",
                        QUICKSWITCH_MAX_PAIRS);
                return;
            }
            char a[QUICKSWITCH_ID_MAX], b[QUICKSWITCH_ID_MAX];
            if (parse_entry(p, len, a, b)) {
                copy_id(q->a[q->pair_count], a);
                copy_id(q->b[q->pair_count], b);
                q->pair_count++;
            } else {
                /* Warn and skip: a blank panel is only recoverable over SSH,
                 * and so is a panel whose navigation half-works. */
                fprintf(stderr,
                        "kdeskdash: ignoring malformed quick-switch pair \"%.*s\" "
                        "(want \"<id>:<id>\")\n",
                        (int)len, p);
            }
        }
        if (!comma)
            break;
        p = comma + 1;
    }
}

void quickswitch_note_active(quickswitch_t *q, const char *id) {
    if (!q || !id || id[0] == '\0')
        return;
    if (strcmp(q->current, id) == 0)
        return; /* re-activating the same mode must not make it its own partner */
    if (q->current[0] != '\0')
        copy_id(q->previous, q->current);
    copy_id(q->current, id);
}

bool quickswitch_tap(quickswitch_t *q, uint32_t now_ms) {
    if (!q || q->disabled)
        return false;
    /* Unsigned arithmetic wraps, which is exactly right for an LVGL tick that
     * rolls over every ~49 days. */
    if (q->tap_pending && (uint32_t)(now_ms - q->last_tap_ms) <= QUICKSWITCH_WINDOW_MS) {
        q->tap_pending = false; /* consumed: a third fast tap starts over */
        return true;
    }
    q->tap_pending = true;
    q->last_tap_ms = now_ms;
    return false;
}

const char *quickswitch_target(const quickswitch_t *q) {
    if (!q || q->disabled || q->current[0] == '\0')
        return NULL;
    for (int i = 0; i < q->pair_count; i++) {
        if (strcmp(q->current, q->a[i]) == 0)
            return q->b[i];
        if (strcmp(q->current, q->b[i]) == 0)
            return q->a[i];
    }
    if (q->previous[0] != '\0' && strcmp(q->previous, q->current) != 0)
        return q->previous;
    return NULL;
}
