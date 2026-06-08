/**
 * @file test_dev_telemetry.c
 * Host-only unit tests for the pure dev_telemetry JSON parser (no Redis/LVGL).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dev_telemetry.h"

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static void check_eqf(float got, float want, const char *what) {
    if (fabsf(got - want) > 0.01f) {
        fprintf(stderr, "FAIL %s: got %.3f, want %.3f\n", what, got, want);
        failures++;
    }
}

static void check_equ(uint32_t got, uint32_t want, const char *what) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %u, want %u\n", what, got, want);
        failures++;
    }
}

/* True when every field of the sample is zero/empty. */
static int is_zeroed(const dev_sample_t *s) {
    dev_sample_t z;
    memset(&z, 0, sizeof(z));
    return memcmp(s, &z, sizeof(z)) == 0;
}

/* A full GPU payload parses every field. */
static void test_full_gpu(void) {
    const char *json =
        "{\"host\":\"kai\",\"ts\":1749300000.0,"
        "\"cpu_pct\":45.2,\"top_core_pct\":78.1,"
        "\"ram_used_mb\":6144,\"ram_total_mb\":16384,"
        "\"gpu\":{\"compute_pct\":32.5,\"vram_used_mb\":3072,\"vram_total_mb\":24576}}";
    dev_sample_t s;
    check(dev_telemetry_parse(json, strlen(json), &s), "full gpu parses");
    check(strcmp(s.host, "kai") == 0, "full gpu host");
    check_eqf(s.cpu_pct, 45.2f, "full gpu cpu_pct");
    check_eqf(s.top_core_pct, 78.1f, "full gpu top_core_pct");
    check_equ(s.ram_used_mb, 6144, "full gpu ram_used_mb");
    check_equ(s.ram_total_mb, 16384, "full gpu ram_total_mb");
    check(s.has_gpu, "full gpu has_gpu");
    check_eqf(s.gpu_compute_pct, 32.5f, "full gpu compute_pct");
    check_equ(s.vram_used_mb, 3072, "full gpu vram_used_mb");
    check_equ(s.vram_total_mb, 24576, "full gpu vram_total_mb");
}

/* gpu:null => CPU-only; GPU fields zeroed, CPU/RAM still parsed. */
static void test_gpu_null(void) {
    const char *json =
        "{\"host\":\"pi5\",\"cpu_pct\":10.0,\"top_core_pct\":12.0,"
        "\"ram_used_mb\":2048,\"ram_total_mb\":8192,\"gpu\":null}";
    dev_sample_t s;
    check(dev_telemetry_parse(json, strlen(json), &s), "gpu:null parses");
    check(!s.has_gpu, "gpu:null has_gpu false");
    check_eqf(s.gpu_compute_pct, 0.0f, "gpu:null compute zero");
    check_equ(s.vram_used_mb, 0, "gpu:null vram_used zero");
    check_equ(s.vram_total_mb, 0, "gpu:null vram_total zero");
    check_eqf(s.cpu_pct, 10.0f, "gpu:null cpu still parsed");
    check_equ(s.ram_total_mb, 8192, "gpu:null ram still parsed");
}

/* gpu key omitted entirely => CPU-only, same as null. */
static void test_gpu_omitted(void) {
    const char *json =
        "{\"host\":\"pi5\",\"cpu_pct\":5.0,\"ram_used_mb\":1024,\"ram_total_mb\":4096}";
    dev_sample_t s;
    check(dev_telemetry_parse(json, strlen(json), &s), "gpu omitted parses");
    check(!s.has_gpu, "gpu omitted has_gpu false");
    check_equ(s.vram_total_mb, 0, "gpu omitted vram_total zero");
}

/* Missing host => parse succeeds, host left empty for the caller to fill. */
static void test_host_missing(void) {
    const char *json = "{\"cpu_pct\":1.0,\"ram_used_mb\":1,\"ram_total_mb\":2}";
    dev_sample_t s;
    check(dev_telemetry_parse(json, strlen(json), &s), "missing host parses");
    check(s.host[0] == '\0', "missing host left empty");
}

/* Empty-string host is treated as absent (left empty). */
static void test_host_empty_string(void) {
    const char *json = "{\"host\":\"\",\"cpu_pct\":1.0}";
    dev_sample_t s;
    check(dev_telemetry_parse(json, strlen(json), &s), "empty host parses");
    check(s.host[0] == '\0', "empty host left empty");
}

/* An over-long host is truncated to DEV_HOST_MAX-1 and NUL-terminated. */
static void test_host_overlong(void) {
    char json[256];
    char longhost[200];
    memset(longhost, 'a', sizeof(longhost) - 1);
    longhost[sizeof(longhost) - 1] = '\0';
    snprintf(json, sizeof(json), "{\"host\":\"%s\",\"cpu_pct\":1.0}", longhost);
    dev_sample_t s;
    check(dev_telemetry_parse(json, strlen(json), &s), "overlong host parses");
    check(strlen(s.host) == DEV_HOST_MAX - 1, "overlong host truncated");
    check(s.host[DEV_HOST_MAX - 1] == '\0', "overlong host NUL-terminated");
}

/* NULL input => failure, output zeroed. */
static void test_null_input(void) {
    dev_sample_t s;
    memset(&s, 0xAA, sizeof(s));
    check(!dev_telemetry_parse(NULL, 10, &s), "NULL input fails");
    check(is_zeroed(&s), "NULL input zeroes output");
}

/* Zero length => failure, output zeroed. */
static void test_zero_length(void) {
    dev_sample_t s;
    memset(&s, 0xAA, sizeof(s));
    check(!dev_telemetry_parse("{\"cpu_pct\":1}", 0, &s), "zero len fails");
    check(is_zeroed(&s), "zero len zeroes output");
}

/* len bounds the parse: a length shorter than the JSON truncates it -> fail. */
static void test_len_truncated(void) {
    const char *json = "{\"cpu_pct\":50.0}";
    dev_sample_t s;
    memset(&s, 0xAA, sizeof(s));
    check(!dev_telemetry_parse(json, 5, &s), "truncated by len fails");
    check(is_zeroed(&s), "truncated zeroes output");
}

/* len stops the parse before trailing garbage (embedded-NUL / extra bytes). */
static void test_len_bounds_trailing_garbage(void) {
    /* Valid object, then a NUL, then junk. Pass len covering only the object. */
    const char buf[] = "{\"cpu_pct\":50.0}\0!!!garbage";
    size_t json_len = strlen("{\"cpu_pct\":50.0}");
    dev_sample_t s;
    check(dev_telemetry_parse(buf, json_len, &s), "len-bounded parse ignores trailing bytes");
    check_eqf(s.cpu_pct, 50.0f, "len-bounded cpu_pct");
}

/* Non-finite numbers (1e400 -> inf) are sanitized to 0. */
static void test_non_finite(void) {
    const char *json = "{\"cpu_pct\":1e400,\"ram_total_mb\":1e400}";
    dev_sample_t s;
    check(dev_telemetry_parse(json, strlen(json), &s), "non-finite parses");
    check_eqf(s.cpu_pct, 0.0f, "inf cpu_pct -> 0");
    check_equ(s.ram_total_mb, 0, "inf ram_total -> 0");
}

/* Negative values clamp to 0. */
static void test_negative_clamp(void) {
    const char *json = "{\"cpu_pct\":-5.0,\"ram_used_mb\":-10,\"top_core_pct\":-1}";
    dev_sample_t s;
    check(dev_telemetry_parse(json, strlen(json), &s), "negative parses");
    check_eqf(s.cpu_pct, 0.0f, "negative cpu_pct -> 0");
    check_eqf(s.top_core_pct, 0.0f, "negative top_core -> 0");
    check_equ(s.ram_used_mb, 0, "negative ram_used -> 0");
}

/* Percentages above 100 clamp to 100. */
static void test_over_100(void) {
    const char *json = "{\"cpu_pct\":150.0,\"top_core_pct\":1000.0}";
    dev_sample_t s;
    check(dev_telemetry_parse(json, strlen(json), &s), "over-100 parses");
    check_eqf(s.cpu_pct, 100.0f, "cpu_pct clamped to 100");
    check_eqf(s.top_core_pct, 100.0f, "top_core clamped to 100");
}

/* Absurdly large MB clamps to INT32_MAX so the value stays safe to cast to the
 * int32_t LVGL chart axis (a UINT32_MAX ceiling would wrap negative). */
static void test_mb_overflow(void) {
    const char *json = "{\"ram_total_mb\":1e30,\"gpu\":{\"vram_total_mb\":1e30}}";
    dev_sample_t s;
    check(dev_telemetry_parse(json, strlen(json), &s), "overflow MB parses");
    check_equ(s.ram_total_mb, (uint32_t)INT32_MAX, "ram_total clamped to INT32_MAX");
    check(s.has_gpu, "overflow gpu present");
    check_equ(s.vram_total_mb, (uint32_t)INT32_MAX, "vram_total clamped to INT32_MAX");
    check((int32_t)s.ram_total_mb >= 0, "clamped ram_total stays non-negative as int32");
    check((int32_t)s.vram_total_mb >= 0, "clamped vram_total stays non-negative as int32");
}

/* vram_total_mb:0 is preserved verbatim (division guard is the caller's job). */
static void test_vram_total_zero(void) {
    const char *json =
        "{\"gpu\":{\"compute_pct\":5.0,\"vram_used_mb\":0,\"vram_total_mb\":0}}";
    dev_sample_t s;
    check(dev_telemetry_parse(json, strlen(json), &s), "vram_total 0 parses");
    check(s.has_gpu, "vram_total 0 has_gpu");
    check_equ(s.vram_total_mb, 0, "vram_total stays 0");
}

/* Wrong field types are ignored (treated as 0), object still parses. */
static void test_wrong_types(void) {
    const char *json =
        "{\"cpu_pct\":\"high\",\"ram_used_mb\":true,\"host\":42,"
        "\"gpu\":{\"compute_pct\":[1,2]}}";
    dev_sample_t s;
    check(dev_telemetry_parse(json, strlen(json), &s), "wrong types parses");
    check_eqf(s.cpu_pct, 0.0f, "string cpu_pct -> 0");
    check_equ(s.ram_used_mb, 0, "bool ram_used -> 0");
    check(s.host[0] == '\0', "numeric host ignored");
    check(s.has_gpu, "gpu object present");
    check_eqf(s.gpu_compute_pct, 0.0f, "array compute -> 0");
}

/* Malformed JSON => failure, output zeroed. */
static void test_malformed(void) {
    dev_sample_t s;
    memset(&s, 0xAA, sizeof(s));
    check(!dev_telemetry_parse("{not json", 9, &s), "malformed fails");
    check(is_zeroed(&s), "malformed zeroes output");
}

/* Non-object roots (array, bare number) => failure. */
static void test_non_object_root(void) {
    dev_sample_t s;
    memset(&s, 0xAA, sizeof(s));
    check(!dev_telemetry_parse("[1,2,3]", 7, &s), "array root fails");
    check(is_zeroed(&s), "array root zeroes output");

    memset(&s, 0xAA, sizeof(s));
    check(!dev_telemetry_parse("42", 2, &s), "bare number root fails");
    check(is_zeroed(&s), "bare number zeroes output");
}

int main(void) {
    test_full_gpu();
    test_gpu_null();
    test_gpu_omitted();
    test_host_missing();
    test_host_empty_string();
    test_host_overlong();
    test_null_input();
    test_zero_length();
    test_len_truncated();
    test_len_bounds_trailing_garbage();
    test_non_finite();
    test_negative_clamp();
    test_over_100();
    test_mb_overflow();
    test_vram_total_zero();
    test_wrong_types();
    test_malformed();
    test_non_object_root();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_dev_telemetry: all passed\n");
    return 0;
}
