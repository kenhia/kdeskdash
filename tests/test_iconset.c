/**
 * @file test_iconset.c
 * Host-only unit tests for the pure `icons` model: set table + splits, paging
 * math, favourites set, and the bake-ready favourites file round-trip.
 */
#include <stdio.h>
#include <string.h>

#include "iconset.h"

static int failures;

static void check(long got, long want, const char *what) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %ld, want %ld\n", what, got, want);
        failures++;
    }
}

static void check_str(const char *got, const char *want, const char *what) {
    if ((got == NULL) != (want == NULL) || (got && strcmp(got, want) != 0)) {
        fprintf(stderr, "FAIL %s: got \"%s\", want \"%s\"\n", what,
                got ? got : "(null)", want ? want : "(null)");
        failures++;
    }
}

static void test_set_table(void) {
    int n = iconset_count();
    /* 9 single sets + Font Awesome(2) + Material Design(14) = 25. */
    check(n, 25, "set count");

    iconset_set_t s;
    check(iconset_at(0, &s), 0, "at(0) ok");
    check_str(s.name, "Font Logos", "first set name");
    check((long)s.start, 0xF300, "first set start");

    /* Out-of-range indices fail cleanly. */
    check(iconset_at(-1, &s), -1, "at(-1) fails");
    check(iconset_at(n, &s), -1, "at(count) fails");

    /* Every set resolves and has a non-empty, ordered range. */
    for (int i = 0; i < n; i++) {
        iconset_set_t t;
        check(iconset_at(i, &t), 0, "each set resolves");
        if (t.start > t.end) {
            fprintf(stderr, "FAIL set %d range inverted: %x..%x\n", i, t.start,
                    t.end);
            failures++;
        }
    }
}

static void test_splits(void) {
    /* Font Awesome splits into "Font Awesome 1".."Font Awesome 2" contiguous
     * over 0xF000..0xF2E0 with no gap or overlap at the seam. */
    iconset_set_t a, b;
    int fa = -1;
    for (int i = 0; i < iconset_count(); i++) {
        iconset_set_t t;
        iconset_at(i, &t);
        if (strcmp(t.name, "Font Awesome 1") == 0) {
            fa = i;
            break;
        }
    }
    check(fa >= 0, 1, "found Font Awesome 1");
    iconset_at(fa, &a);
    iconset_at(fa + 1, &b);
    check_str(a.name, "Font Awesome 1", "FA part 1 name");
    check_str(b.name, "Font Awesome 2", "FA part 2 name");
    check((long)a.start, 0xF000, "FA part 1 starts at group start");
    check((long)b.end, 0xF2E0, "FA part 2 ends at group end");
    check((long)b.start, (long)a.end + 1, "FA split contiguous (no gap/overlap)");

    /* Material Design's last sub-set ends exactly at the group end. */
    iconset_set_t last;
    iconset_at(iconset_count() - 1, &last);
    check_str(last.name, "Material Design 14", "last set is MD 14");
    check((long)last.end, 0xF1AF0, "MD last ends at group end");
}

static void test_wrap(void) {
    int n = iconset_count();
    check(iconset_wrap(0, +1), 1, "wrap +1 from 0");
    check(iconset_wrap(n - 1, +1), 0, "wrap +1 past end");
    check(iconset_wrap(0, -1), n - 1, "wrap -1 from 0");
    check(iconset_wrap(3, +n), 3, "wrap full turn is identity");
}

static void test_name_for_cp(void) {
    check_str(iconset_name_for_cp(0xF301), "Font Logos", "F301 -> Font Logos");
    check_str(iconset_name_for_cp(0xE706), "Devicons", "E706 -> Devicons");
    check_str(iconset_name_for_cp(0xF0100), "Material Design",
              "F0100 -> Material Design (sub-set collapses to base name)");
    check_str(iconset_name_for_cp(0x00041), NULL, "'A' belongs to no set");
}

static void test_paging(void) {
    check(iconset_page_count(0, 56), 1, "empty set still one page");
    check(iconset_page_count(56, 56), 1, "exact fill is one page");
    check(iconset_page_count(57, 56), 2, "one over spills to two");
    check(iconset_page_count(112, 56), 2, "two exact pages");
    check(iconset_page_count(113, 56), 3, "remainder rounds up");
    check(iconset_page_count(10, 0), 1, "per_page<=0 guarded");

    check(iconset_clamp_page(-3, 100, 56), 0, "clamp below to 0");
    check(iconset_clamp_page(99, 100, 56), 1, "clamp above to last");
    check(iconset_clamp_page(1, 100, 56), 1, "in-range unchanged");
    check(iconset_clamp_page(5, 0, 56), 0, "clamp into single empty page");
}

static void test_favs_set(void) {
    iconset_favs_t f;
    iconset_favs_clear(&f);
    check(iconset_favs_count(&f), 0, "starts empty");

    check(iconset_favs_add(&f, 0xF31B), 1, "add new");
    check(iconset_favs_add(&f, 0xF31B), 0, "add dup is no-op");
    check(iconset_favs_contains(&f, 0xF31B), 1, "contains after add");
    check(iconset_favs_count(&f), 1, "count after dup add");

    /* Insertion keeps ascending order regardless of add order. */
    iconset_favs_add(&f, 0xE700);
    iconset_favs_add(&f, 0xF400);
    check((long)iconset_favs_at(&f, 0), 0xE700, "sorted[0]");
    check((long)iconset_favs_at(&f, 1), 0xF31B, "sorted[1]");
    check((long)iconset_favs_at(&f, 2), 0xF400, "sorted[2]");
    check((long)iconset_favs_at(&f, 9), 0, "at out-of-range -> 0");

    check(iconset_favs_toggle(&f, 0xF31B), 0, "toggle off returns 0");
    check(iconset_favs_contains(&f, 0xF31B), 0, "gone after toggle off");
    check(iconset_favs_toggle(&f, 0xEA60), 1, "toggle on returns 1");
    check(iconset_favs_remove(&f, 0x1234), 0, "remove absent is 0");
}

static void test_favs_roundtrip(void) {
    iconset_favs_t f;
    iconset_favs_clear(&f);
    iconset_favs_add(&f, 0xF31B);
    iconset_favs_add(&f, 0xE706);
    iconset_favs_add(&f, 0xEA60);

    char buf[512];
    size_t n = iconset_favs_format(&f, buf, sizeof(buf));
    check(n > 0, 1, "format wrote bytes");

    /* Format -> parse is identity in membership and order. */
    iconset_favs_t g;
    int loaded = iconset_favs_parse(&g, buf);
    check(loaded, 3, "parsed all three back");
    check((long)iconset_favs_at(&g, 0), 0xE706, "rt sorted[0]");
    check((long)iconset_favs_at(&g, 1), 0xEA60, "rt sorted[1]");
    check((long)iconset_favs_at(&g, 2), 0xF31B, "rt sorted[2]");

    /* The file annotates each codepoint with its set. */
    if (!strstr(buf, "# Font Logos") || !strstr(buf, "# Devicons")) {
        fprintf(stderr, "FAIL format missing set comments:\n%s\n", buf);
        failures++;
    }
}

static void test_favs_parse_tolerant(void) {
    const char *text = "# a comment line\n"
                       "\n"
                       "   \n"
                       "0xF31B  # with 0x prefix and trailing comment\n"
                       "U+E706\n"
                       "  ea60  \n" /* leading space, bare hex */
                       "garbage line no hex\n"
                       "f31b again duplicate\n" /* dup collapses */
                       "zzzz\n";                /* non-hex -> skipped */
    iconset_favs_t f;
    int loaded = iconset_favs_parse(&f, text);
    check(loaded, 3, "tolerant parse keeps 3 unique valid");
    check(iconset_favs_contains(&f, 0xF31B), 1, "0x-prefixed parsed");
    check(iconset_favs_contains(&f, 0xE706), 1, "U+ parsed");
    check(iconset_favs_contains(&f, 0xEA60), 1, "bare hex parsed");

    /* NULL and empty inputs are safe. */
    check(iconset_favs_parse(&f, NULL), 0, "NULL text -> 0");
    check(iconset_favs_parse(&f, ""), 0, "empty text -> 0");
}

static void test_format_truncation(void) {
    iconset_favs_t f;
    iconset_favs_clear(&f);
    for (uint32_t cp = 0xF300; cp < 0xF320; cp++)
        iconset_favs_add(&f, cp);
    /* A tiny buffer must never overflow and must stay NUL-terminated. */
    char small[16];
    size_t n = iconset_favs_format(&f, small, sizeof(small));
    check(n < sizeof(small), 1, "truncated within buffer");
    check(small[n] == '\0', 1, "NUL-terminated after truncation");
}

int main(void) {
    test_set_table();
    test_splits();
    test_wrap();
    test_name_for_cp();
    test_paging();
    test_favs_set();
    test_favs_roundtrip();
    test_favs_parse_tolerant();
    test_format_truncation();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_iconset: all passed\n");
    return 0;
}
