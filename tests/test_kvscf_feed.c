/**
 * @file test_kvscf_feed.c
 * Host-only unit tests for the pure `foreground`-mode core: JSON parse (incl.
 * null/missing/malformed), ci-label sort, display-host fallback, paging math,
 * focus-payload building, and token trimming. Uses the captured live sample.
 */
#include <stdio.h>
#include <string.h>

#include "kvscf_feed.h"

static int failures;

static void check(long got, long want, const char *what) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %ld, want %ld\n", what, got, want);
        failures++;
    }
}

static void check_str(const char *got, const char *want, const char *what) {
    if (!got || strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s: got \"%s\", want \"%s\"\n", what,
                got ? got : "(null)", want);
        failures++;
    }
}

/* Trimmed excerpt of a real kvscf:instances:cleo value (2026-07-18): a local
 * stable window (null active_file/remote_host) and two ssh Insiders windows. */
static const char *SAMPLE =
    "{\"host\":\"cleo\",\"instances\":["
    "{\"active_file\":null,\"app\":\"stable\",\"id\":\"432410618\","
    "\"label\":\"ClaudeWorks\",\"remote\":\"local\",\"remote_host\":null,"
    "\"workspace\":\"ClaudeWorks\",\"z_index\":25},"
    "{\"active_file\":\"ch3.ipynb\",\"app\":\"insiders\",\"id\":\"566170092\","
    "\"label\":\"gen-ai-langchain (kai)\",\"remote\":\"ssh\",\"remote_host\":\"kai\","
    "\"workspace\":\"gen-ai-langchain\",\"z_index\":16},"
    "{\"active_file\":\"plan.md\",\"app\":\"insiders\",\"id\":\"2034420\","
    "\"label\":\"homelab-ai-plan (kai)\",\"remote\":\"ssh\",\"remote_host\":\"kai\","
    "\"workspace\":\"homelab-ai-plan\",\"z_index\":20}]}";

static void test_parse(void) {
    kvscf_instance_t arr[KV_INSTANCES_MAX];
    int n = kvscf_parse_append(SAMPLE, strlen(SAMPLE), arr, 0, KV_INSTANCES_MAX);
    check(n, 3, "parsed three instances");

    /* First instance in array order (pre-sort) is the local stable one. */
    check_str(arr[0].id, "432410618", "id copied");
    check_str(arr[0].label, "ClaudeWorks", "label copied");
    check_str(arr[0].host, "cleo", "host from root");
    check_str(arr[0].remote_host, "", "null remote_host -> empty");
    check_str(arr[0].active_file, "", "null active_file -> empty");
    check(arr[0].app, KV_APP_STABLE, "app stable");
    check(arr[0].remote, KV_REMOTE_LOCAL, "remote local");
    check(arr[0].z_index, 25, "z_index parsed");

    check(arr[1].app, KV_APP_INSIDERS, "app insiders");
    check_str(arr[1].remote_host, "kai", "remote_host copied");
    check_str(arr[1].active_file, "ch3.ipynb", "active_file copied");
}

static void test_display_host(void) {
    kvscf_instance_t arr[KV_INSTANCES_MAX];
    int n = kvscf_parse_append(SAMPLE, strlen(SAMPLE), arr, 0, KV_INSTANCES_MAX);
    check(n, 3, "have three");
    /* local -> publisher host; ssh -> remote_host. */
    check_str(kvscf_display_host(&arr[0]), "cleo", "local displays publisher host");
    check_str(kvscf_display_host(&arr[1]), "kai", "ssh displays remote_host");
}

static void test_display_label(void) {
    kvscf_instance_t arr[KV_INSTANCES_MAX];
    int n = kvscf_parse_append(SAMPLE, strlen(SAMPLE), arr, 0, KV_INSTANCES_MAX);
    check(n, 3, "have three");
    char buf[KV_LABEL_MAX];
    /* ssh window: " (kai)" suffix matches remote_host -> stripped. */
    kvscf_display_label(&arr[1], buf, sizeof(buf));
    check_str(buf, "gen-ai-langchain", "strips matching (kai) suffix");
    /* local window: no suffix -> unchanged. */
    kvscf_display_label(&arr[0], buf, sizeof(buf));
    check_str(buf, "ClaudeWorks", "no suffix left intact");

    /* Non-matching parenthesised token is preserved. */
    kvscf_instance_t x;
    memset(&x, 0, sizeof(x));
    snprintf(x.label, sizeof(x.label), "foo (bar)");
    snprintf(x.remote_host, sizeof(x.remote_host), "kai");
    kvscf_display_label(&x, buf, sizeof(buf));
    check_str(buf, "foo (bar)", "non-matching suffix kept");
}

static void test_sort(void) {
    kvscf_instance_t arr[KV_INSTANCES_MAX];
    int n = kvscf_parse_append(SAMPLE, strlen(SAMPLE), arr, 0, KV_INSTANCES_MAX);
    kvscf_sort_by_label(arr, n);
    /* Case-insensitive alphabetical: ClaudeWorks, gen-ai-langchain, homelab-ai. */
    check_str(arr[0].label, "ClaudeWorks", "sorted[0]");
    check_str(arr[1].label, "gen-ai-langchain (kai)", "sorted[1]");
    check_str(arr[2].label, "homelab-ai-plan (kai)", "sorted[2]");
}

/* Sprint 008: favorites. A non-running row carries a folder URI as its id (not an
 * HWND) — it must survive parsing untruncated or the relaunch command breaks. */
static const char *FAV_SAMPLE =
    "{\"host\":\"cleo\",\"instances\":["
    "{\"id\":\"777\",\"label\":\"zeta-open\",\"app\":\"insiders\",\"running\":true,\"favorite\":true},"
    "{\"id\":\"888\",\"label\":\"alpha-open\",\"app\":\"stable\",\"running\":true,\"favorite\":false},"
    "{\"id\":\"vscode-remote://ssh-remote%2Bkai/home/ken/src/ai-agents/harness-eval\","
    "\"label\":\"harness-eval (kai)\",\"remote_host\":\"kai\",\"app\":\"insiders\","
    "\"active_file\":null,\"z_index\":null,\"running\":false,\"favorite\":true}]}";

static void test_favorites_parse(void) {
    kvscf_instance_t arr[KV_INSTANCES_MAX];
    int n = kvscf_parse_append(FAV_SAMPLE, strlen(FAV_SAMPLE), arr, 0,
                               KV_INSTANCES_MAX);
    check(n, 3, "parsed three (incl. a non-running favorite)");

    check(arr[0].running, 1, "running true parsed");
    check(arr[0].favorite, 1, "favorite true parsed");
    check(arr[1].favorite, 0, "favorite false parsed");
    check(arr[2].running, 0, "running false parsed");

    /* The landmine: the folder-URI id must round-trip in full. */
    const char *uri =
        "vscode-remote://ssh-remote%2Bkai/home/ken/src/ai-agents/harness-eval";
    check_str(arr[2].id, uri, "folder-URI id not truncated");
    check((long)strlen(arr[2].id), (long)strlen(uri), "URI id full length");
}

static void test_favorites_sort(void) {
    kvscf_instance_t arr[KV_INSTANCES_MAX];
    int n = kvscf_parse_append(FAV_SAMPLE, strlen(FAV_SAMPLE), arr, 0,
                               KV_INSTANCES_MAX);
    kvscf_sort_by_label(arr, n);
    /* Running block first (alpha-open, zeta-open), then the non-running one —
     * even though "harness-eval" sorts before "zeta-open" alphabetically. */
    check_str(arr[0].label, "alpha-open", "running sorted[0]");
    check_str(arr[1].label, "zeta-open", "running sorted[1]");
    check_str(arr[2].label, "harness-eval (kai)", "non-running last");
    check(arr[2].running, 0, "last row is the favorite");
}

static void test_running_defaults_true(void) {
    /* Older publisher with no `running` field -> treated as running. */
    kvscf_instance_t arr[KV_INSTANCES_MAX];
    int n = kvscf_parse_append(SAMPLE, strlen(SAMPLE), arr, 0, KV_INSTANCES_MAX);
    check(n, 3, "legacy sample parsed");
    check(arr[0].running, 1, "absent running -> true (back-compat)");
    check(arr[0].favorite, 0, "absent favorite -> false");
}

static void test_merge_across_hosts(void) {
    const char *a = "{\"host\":\"cleo\",\"instances\":["
                    "{\"id\":\"1\",\"label\":\"Zeta\",\"app\":\"stable\"}]}";
    const char *b = "{\"host\":\"kubs0\",\"instances\":["
                    "{\"id\":\"2\",\"label\":\"Alpha\",\"app\":\"insiders\"}]}";
    kvscf_instance_t arr[KV_INSTANCES_MAX];
    int n = 0;
    n = kvscf_parse_append(a, strlen(a), arr, n, KV_INSTANCES_MAX);
    n = kvscf_parse_append(b, strlen(b), arr, n, KV_INSTANCES_MAX);
    check(n, 2, "merged two hosts");
    kvscf_sort_by_label(arr, n);
    check_str(arr[0].label, "Alpha", "merged+sorted[0]");
    check_str(arr[0].host, "kubs0", "alpha from kubs0");
    check_str(arr[1].label, "Zeta", "merged+sorted[1]");
}

static void test_parse_tolerant(void) {
    kvscf_instance_t arr[KV_INSTANCES_MAX];
    /* Bad root host -> whole blob rejected. */
    check(kvscf_parse_append("{\"host\":\"bad host!\",\"instances\":[]}", 34, arr,
                             0, KV_INSTANCES_MAX),
          0, "invalid host token rejects blob");
    /* Non-JSON, empty, NULL -> unchanged count. */
    check(kvscf_parse_append("not json", 8, arr, 0, KV_INSTANCES_MAX), 0,
          "garbage -> 0");
    check(kvscf_parse_append(NULL, 0, arr, 5, KV_INSTANCES_MAX), 5,
          "NULL leaves count");
    /* Missing id or label -> that instance skipped, others kept. */
    const char *mixed =
        "{\"host\":\"cleo\",\"instances\":["
        "{\"label\":\"no-id\"},"                         /* skip: no id      */
        "{\"id\":\"9\"},"                                 /* skip: no label   */
        "{\"id\":\"7\",\"label\":\"keep\",\"app\":\"x\"}]}"; /* unknown app ok */
    int n = kvscf_parse_append(mixed, strlen(mixed), arr, 0, KV_INSTANCES_MAX);
    check(n, 1, "only the complete instance kept");
    check_str(arr[0].label, "keep", "kept the valid one");
    check(arr[0].app, KV_APP_UNKNOWN, "unrecognised app -> unknown");
}

static void test_cap(void) {
    /* Never overflow the array cap. */
    kvscf_instance_t arr[KV_INSTANCES_MAX];
    char big[8192];
    size_t off = 0;
    off += (size_t)snprintf(big + off, sizeof(big) - off, "{\"host\":\"cleo\",\"instances\":[");
    for (int i = 0; i < KV_INSTANCES_MAX + 20; i++)
        off += (size_t)snprintf(big + off, sizeof(big) - off,
                                "%s{\"id\":\"%d\",\"label\":\"w%d\"}", i ? "," : "",
                                i, i);
    off += (size_t)snprintf(big + off, sizeof(big) - off, "]}");
    int n = kvscf_parse_append(big, off, arr, 0, KV_INSTANCES_MAX);
    check(n, KV_INSTANCES_MAX, "clamped to cap");
}

/* Trimmed excerpt of a real kvscf:edge:cleo value: named windows (tab_count
 * null) and unnamed windows (integer tab_count). */
static const char *EDGE_SAMPLE =
    "{\"host\":\"cleo\",\"ts\":1784417437,\"windows\":["
    "{\"id\":\"591750\",\"label\":\"Claude\",\"named\":true,\"tab_count\":null,\"z_index\":7},"
    "{\"id\":\"67994\",\"label\":\"AI-Models\",\"named\":true,\"tab_count\":null,\"z_index\":64},"
    "{\"id\":\"657812\",\"label\":\"Dashboard | Claude Platform\",\"named\":false,\"tab_count\":9,\"z_index\":40},"
    "{\"id\":\"526744\",\"label\":\"ch2-chat-models\",\"named\":false,\"tab_count\":3,\"z_index\":53}]}";

static void test_edge_parse(void) {
    kvscf_edge_t arr[KV_INSTANCES_MAX];
    int n = kvscf_parse_edge_append(EDGE_SAMPLE, strlen(EDGE_SAMPLE), arr, 0,
                                    KV_INSTANCES_MAX);
    check(n, 4, "parsed four edge windows");
    check_str(arr[0].id, "591750", "edge id");
    check_str(arr[0].label, "Claude", "edge label");
    check_str(arr[0].host, "cleo", "edge host from root");
    check(arr[0].named, 1, "named true");
    check(arr[0].tab_count, -1, "named tab_count null -> -1");
    check(arr[2].named, 0, "unnamed false");
    check(arr[2].tab_count, 9, "unnamed tab_count parsed");
}

static void test_edge_sort(void) {
    kvscf_edge_t arr[KV_INSTANCES_MAX];
    int n = kvscf_parse_edge_append(EDGE_SAMPLE, strlen(EDGE_SAMPLE), arr, 0,
                                    KV_INSTANCES_MAX);
    kvscf_sort_edge(arr, n);
    /* Named block first (AI-Models, Claude), then unnamed (ch2..., Dashboard). */
    check_str(arr[0].label, "AI-Models", "named sorted[0]");
    check(arr[0].named, 1, "sorted[0] named");
    check_str(arr[1].label, "Claude", "named sorted[1]");
    check(arr[1].named, 1, "sorted[1] named");
    check_str(arr[2].label, "ch2-chat-models", "unnamed sorted[0]");
    check(arr[2].named, 0, "sorted[2] unnamed");
    check_str(arr[3].label, "Dashboard | Claude Platform", "unnamed sorted[1]");
}

/* Trimmed excerpt of a real kvscf:apps:cleo value: running + not-running apps. */
static const char *APPS_SAMPLE =
    "{\"apps\":["
    "{\"id\":\"9176544\",\"key\":\"claude\",\"label\":\"Claude\",\"order\":0,\"running\":true},"
    "{\"id\":null,\"key\":\"kindle\",\"label\":\"Kindle\",\"order\":5,\"running\":false},"
    "{\"id\":\"198890\",\"key\":\"copilot\",\"label\":\"Copilot\",\"order\":2,\"running\":true}],"
    "\"host\":\"cleo\",\"ts\":1784423885}";

static void test_apps_parse_sort(void) {
    kvscf_appitem_t arr[KV_INSTANCES_MAX];
    int n = kvscf_parse_apps_append(APPS_SAMPLE, strlen(APPS_SAMPLE), arr, 0,
                                    KV_INSTANCES_MAX);
    check(n, 3, "parsed three apps");
    kvscf_sort_apps(arr, n);
    /* Sorted by order: claude(0), copilot(2), kindle(5). */
    check_str(arr[0].key, "claude", "order sort[0]");
    check_str(arr[0].host, "cleo", "app host from root");
    check(arr[0].running, 1, "claude running");
    check_str(arr[1].key, "copilot", "order sort[1]");
    check_str(arr[2].key, "kindle", "order sort[2]");
    check(arr[2].running, 0, "kindle not running (null id ok)");
}

static void test_launch_payload(void) {
    char buf[128];
    size_t n = kvscf_launch_payload("kvscf-abc", "kindle", buf, sizeof(buf));
    check(n > 0, 1, "launch payload built");
    check_str(buf, "{\"token\":\"kvscf-abc\",\"app\":\"kindle\"}", "launch shape");
    /* Guards: no token or no key -> empty. */
    check(kvscf_launch_payload("", "kindle", buf, sizeof(buf)), 0, "no token -> 0");
    check(kvscf_launch_payload("t", "", buf, sizeof(buf)), 0, "no key -> 0");
    char small[8];
    check(kvscf_launch_payload("kvscf-abc", "kindle", small, sizeof(small)), 0,
          "too small -> 0");
    check_str(small, "", "empty on overflow");
}

static void test_app_color(void) {
    check((long)kvscf_app_color(KV_APP_STABLE), 0x60A5EB, "stable colour");
    check((long)kvscf_app_color(KV_APP_INSIDERS), 0x38BE84, "insiders colour");
    check((long)kvscf_app_color(KV_APP_UNKNOWN), 0xE9EDF6, "unknown -> ink");
}

static void test_paging(void) {
    check(kvscf_page_count(0, KV_PER_PAGE), 1, "empty -> one page");
    check(kvscf_page_count(28, 28), 1, "exact fill one page");
    check(kvscf_page_count(29, 28), 2, "one over -> two pages");
    check(kvscf_clamp_page(5, 29, 28), 1, "clamp above to last");
    check(kvscf_clamp_page(-2, 29, 28), 0, "clamp below to 0");
}

static void test_focus_payload(void) {
    char buf[256];
    size_t n = kvscf_focus_payload("kvscf-abc", "749802118", false, buf, sizeof(buf));
    check(n > 0, 1, "payload built");
    check_str(buf, "{\"token\":\"kvscf-abc\",\"id\":\"749802118\",\"maximize\":false}",
              "payload shape");
    kvscf_focus_payload("t", "1", true, buf, sizeof(buf));
    check_str(buf, "{\"token\":\"t\",\"id\":\"1\",\"maximize\":true}", "maximize true");
    /* R8: no token or no id -> empty, length 0. */
    check(kvscf_focus_payload("", "1", false, buf, sizeof(buf)), 0, "no token -> 0");
    check_str(buf, "", "empty buf on no token");
    check(kvscf_focus_payload("t", "", false, buf, sizeof(buf)), 0, "no id -> 0");
    /* Tiny buffer -> 0, not overflow. */
    char small[8];
    check(kvscf_focus_payload("kvscf-abc", "1", false, small, sizeof(small)), 0,
          "too small -> 0");
    check_str(small, "", "empty on overflow");
}

static void test_trim(void) {
    char a[] = "kvscf-abc\r\n";
    check((long)kvscf_trim_trailing(a), 9, "trim CRLF length");
    check_str(a, "kvscf-abc", "trimmed value");
    char b[] = "  spaced \t ";
    kvscf_trim_trailing(b);
    check_str(b, "  spaced", "trailing ws trimmed, leading kept");
    char c[] = "clean";
    check((long)kvscf_trim_trailing(c), 5, "no-op clean");
}

/* ---- Launcher buttons (sprint 025) ------------------------------------ */

/* Shape taken verbatim from the frozen contract (kvscf docs
 * kdeskdash-vscode-mode.md §6). */
static const char *LAUNCHER =
    "{\"host\":\"kwork\",\"ts\":1784416199,"
    "\"grid\":{\"rows\":3,\"cols\":9},"
    "\"buttons\":["
    "{\"key\":\"ado-pipelines\",\"label\":\"Pipelines\",\"color\":\"#2ec4c4\","
    "\"row\":0,\"col\":0,\"w\":2,\"h\":1},"
    "{\"key\":\"ado-wits\",\"label\":\"Work Items\",\"color\":\"#2ec4c4\","
    "\"row\":0,\"col\":2,\"w\":1,\"h\":1}]}";

static void test_launcher_parse(void) {
    kvscf_launcher_t L;
    check(kvscf_parse_launcher(LAUNCHER, strlen(LAUNCHER), &L), 1, "parsed");
    check_str(L.host, "kwork", "host from root");
    check(L.ts, 1784416199, "ts parsed");
    check(L.rows, 3, "grid rows as published");
    check(L.cols, 9, "grid cols as published");
    check(L.count, 2, "two buttons");
    check(L.skipped, 0, "nothing skipped");
    check_str(L.buttons[0].key, "ado-pipelines", "key copied");
    check_str(L.buttons[0].label, "Pipelines", "label copied");
    check_str(L.buttons[0].color, "#2ec4c4", "color copied verbatim");
    check(L.buttons[0].w, 2, "w span");
    check(L.buttons[1].col, 2, "col");
    check(L.buttons[1].h, 1, "h span");
}

/* The grid is on the wire so neither side assumes it: a payload without a
 * usable one is rejected whole, leaving the caller's last-good config intact. */
static void test_launcher_grid_required(void) {
    kvscf_launcher_t L;
    memset(&L, 0, sizeof(L));
    L.cols = 9; /* sentinel: must survive a rejected parse */

    const char *no_grid = "{\"host\":\"kwork\",\"buttons\":[]}";
    check(kvscf_parse_launcher(no_grid, strlen(no_grid), &L), 0, "no grid -> reject");
    check(L.cols, 9, "rejected parse leaves out untouched");

    const char *zero = "{\"host\":\"kwork\",\"grid\":{\"rows\":0,\"cols\":9}}";
    check(kvscf_parse_launcher(zero, strlen(zero), &L), 0, "zero rows -> reject");

    const char *huge = "{\"host\":\"kwork\",\"grid\":{\"rows\":3,\"cols\":99}}";
    check(kvscf_parse_launcher(huge, strlen(huge), &L), 0, "cols past ceiling -> reject");

    const char *bad_host = "{\"host\":\"kwork;rm\",\"grid\":{\"rows\":3,\"cols\":9}}";
    check(kvscf_parse_launcher(bad_host, strlen(bad_host), &L), 0, "bad host -> reject");

    check(kvscf_parse_launcher("not json", 8, &L), 0, "garbage -> reject");

    /* An empty but well-formed grid IS usable — it means "no buttons yet". */
    const char *empty = "{\"host\":\"kwork\",\"grid\":{\"rows\":3,\"cols\":9},\"buttons\":[]}";
    check(kvscf_parse_launcher(empty, strlen(empty), &L), 1, "empty button list ok");
    check(L.count, 0, "no buttons");
    check(L.cols, 9, "grid overwritten on success");
}

/* A typo can never blank a panel: bad placements are skipped and counted, the
 * good ones still render. */
static void test_launcher_skips_bad_buttons(void) {
    const char *json =
        "{\"host\":\"kwork\",\"grid\":{\"rows\":3,\"cols\":4},\"buttons\":["
        "{\"key\":\"ok\",\"label\":\"Ok\",\"row\":0,\"col\":0},"        /* keep      */
        "{\"key\":\"oob\",\"label\":\"X\",\"row\":3,\"col\":0},"         /* off grid  */
        "{\"key\":\"wide\",\"label\":\"X\",\"row\":1,\"col\":3,\"w\":2},"/* overhangs */
        "{\"key\":\"big\",\"label\":\"X\",\"row\":1,\"col\":0,\"w\":4}," /* w > span  */
        "{\"key\":\"neg\",\"label\":\"X\",\"row\":-1,\"col\":0},"        /* negative  */
        "{\"label\":\"X\",\"row\":2,\"col\":0},"                         /* no key    */
        "{\"key\":\"nolabel\",\"row\":2,\"col\":1},"                     /* no label  */
        "{\"key\":\"clash\",\"label\":\"X\",\"row\":0,\"col\":0},"       /* overlap   */
        "\"not an object\","
        "{\"key\":\"last\",\"label\":\"Last\",\"row\":2,\"col\":3}]}";
    kvscf_launcher_t L;
    check(kvscf_parse_launcher(json, strlen(json), &L), 1, "parsed despite junk");
    check(L.count, 2, "two survivors");
    check(L.skipped, 8, "eight skipped and counted");
    check_str(L.buttons[0].key, "ok", "first survivor");
    check_str(L.buttons[1].key, "last", "second survivor");
    /* Earlier wins the contested cell, matching the publisher. */
    check_str(L.buttons[0].label, "Ok", "earlier button kept the cell");
}

/* Absent w/h default to 1; a multi-cell button claims every cell it covers. */
static void test_launcher_span_occupancy(void) {
    const char *json =
        "{\"host\":\"kwork\",\"grid\":{\"rows\":3,\"cols\":4},\"buttons\":["
        "{\"key\":\"big\",\"label\":\"Big\",\"row\":0,\"col\":0,\"w\":2,\"h\":2},"
        "{\"key\":\"inside\",\"label\":\"X\",\"row\":1,\"col\":1},"  /* inside big  */
        "{\"key\":\"free\",\"label\":\"Free\",\"row\":1,\"col\":2}]}";
    kvscf_launcher_t L;
    check(kvscf_parse_launcher(json, strlen(json), &L), 1, "parsed");
    check(L.count, 2, "the covered cell was refused");
    check(L.skipped, 1, "one skipped");
    check(L.buttons[1].w, 1, "absent w defaults to 1");
    check(L.buttons[1].h, 1, "absent h defaults to 1");
}

/* Risk from sprint 008: a key that does not fit must be REJECTED, never
 * truncated — a truncated key presses the wrong button. Labels may truncate. */
static void test_launcher_oversize_fields(void) {
    char json[512];
    char key[KV_BTNKEY_MAX + 8];
    memset(key, 'k', sizeof(key) - 1);
    key[sizeof(key) - 1] = '\0';
    snprintf(json, sizeof(json),
             "{\"host\":\"kwork\",\"grid\":{\"rows\":1,\"cols\":2},\"buttons\":["
             "{\"key\":\"%s\",\"label\":\"X\",\"row\":0,\"col\":0}]}", key);
    kvscf_launcher_t L;
    check(kvscf_parse_launcher(json, strlen(json), &L), 1, "envelope still ok");
    check(L.count, 0, "over-long key rejected, not truncated");
    check(L.skipped, 1, "counted as skipped");

    /* The longest key that DOES fit must round-trip byte-exact. */
    memset(key, 'k', KV_BTNKEY_MAX - 1);
    key[KV_BTNKEY_MAX - 1] = '\0';
    snprintf(json, sizeof(json),
             "{\"host\":\"kwork\",\"grid\":{\"rows\":1,\"cols\":2},\"buttons\":["
             "{\"key\":\"%s\",\"label\":\"X\",\"row\":0,\"col\":0}]}", key);
    check(kvscf_parse_launcher(json, strlen(json), &L), 1, "parsed");
    check(L.count, 1, "max-length key accepted");
    check_str(L.buttons[0].key, key, "max-length key round-trips exactly");
}

static void test_launcher_cap(void) {
    /* A 6x12 grid is the ceiling; every cell filled is exactly KV_BUTTONS_MAX. */
    char json[8192];
    int n = snprintf(json, sizeof(json),
                     "{\"host\":\"kwork\",\"grid\":{\"rows\":%d,\"cols\":%d},"
                     "\"buttons\":[", KV_GRID_ROWS_MAX, KV_GRID_COLS_MAX);
    for (int r = 0; r < KV_GRID_ROWS_MAX; r++)
        for (int c = 0; c < KV_GRID_COLS_MAX; c++)
            n += snprintf(json + n, sizeof(json) - (size_t)n,
                          "%s{\"key\":\"b%d_%d\",\"label\":\"B\",\"row\":%d,"
                          "\"col\":%d}", (r || c) ? "," : "", r, c, r, c);
    snprintf(json + n, sizeof(json) - (size_t)n, "]}");

    kvscf_launcher_t L;
    check(kvscf_parse_launcher(json, strlen(json), &L), 1, "full grid parses");
    check(L.count, KV_BUTTONS_MAX, "every cell filled");
    check(L.skipped, 0, "nothing skipped at the ceiling");
}

static void test_press_payload(void) {
    char buf[256];
    size_t n = kvscf_press_payload("kvscf-abc", "ado-pipelines", buf, sizeof(buf));
    check((long)n, (long)strlen(buf), "press length matches");
    check_str(buf, "{\"token\":\"kvscf-abc\",\"button\":\"ado-pipelines\"}",
              "press payload shape");
    /* R8 again: never a command without a token. */
    check(kvscf_press_payload("", "k", buf, sizeof(buf)), 0, "no token -> 0");
    check_str(buf, "", "empty buf on no token");
    check(kvscf_press_payload("t", "", buf, sizeof(buf)), 0, "no key -> 0");
    char small[8];
    check(kvscf_press_payload("kvscf-abc", "ado-pipelines", small, sizeof(small)),
          0, "too small -> 0");
    check_str(small, "", "empty on overflow");
}

static void test_button_rgb(void) {
    uint32_t rgb = 0;
    check(kvscf_button_rgb("#2ec4c4", &rgb), 1, "hex with hash");
    check((long)rgb, 0x2ec4c4, "hex value");
    check(kvscf_button_rgb("2EC4C4", &rgb), 1, "hex without hash, upper");
    check((long)rgb, 0x2ec4c4, "case-insensitive hex");
    /* Palette names are the repo's colour vocabulary — accept them by name. */
    check(kvscf_button_rgb("edge_teal", &rgb), 1, "palette name");
    check((long)rgb, 0x2ec4c4, "palette value");
    check(kvscf_button_rgb("CLAUDE_CORAL", &rgb), 1, "palette name upper");
    check((long)rgb, 0xcf6b4a, "palette value upper");
    /* Unknown/empty is "use the default", never a reason to drop the button. */
    check(kvscf_button_rgb("", &rgb), 0, "empty -> default");
    check(kvscf_button_rgb("#12345", &rgb), 0, "short hex -> default");
    check(kvscf_button_rgb("#gggggg", &rgb), 0, "non-hex -> default");
    check(kvscf_button_rgb("chartreuse", &rgb), 0, "unknown name -> default");
    check(kvscf_button_rgb(NULL, &rgb), 0, "NULL -> default");
}

/* Stub predicate: ASCII renders, everything else does not — which is exactly
 * the posture of Montserrat + a symbols-only Nerd Font. */
static bool ascii_only(uint32_t cp, void *ctx) {
    (void)ctx;
    return cp < 0x80;
}

static void test_label_filter(void) {
    char out[64];

    /* U+1F980 CRAB, then a space, then text. */
    kvscf_label_filter("\xF0\x9F\xA6\x80 Rust Docs", out, sizeof(out), ascii_only, NULL);
    check_str(out, "Rust Docs", "unrenderable leader dropped and trimmed");

    /* U+1F6E0 U+FE0F — base emoji plus a variation selector. */
    kvscf_label_filter("Build \xF0\x9F\x9B\xA0\xEF\xB8\x8F now", out, sizeof(out),
                       ascii_only, NULL);
    check_str(out, "Build now", "emoji + VS16 dropped, spaces collapsed");

    /* Always-drop set applies even with no predicate: ZWJ, VS15/16, skin tone. */
    kvscf_label_filter("a\xE2\x80\x8D\xEF\xB8\x8E\xF0\x9F\x8F\xBD" "b", out,
                       sizeof(out), NULL, NULL);
    check_str(out, "ab", "ZWJ/VS15/skin-tone always dropped");

    /* A renderable non-ASCII codepoint survives when the predicate allows it. */
    kvscf_label_filter("caf\xC3\xA9", out, sizeof(out), NULL, NULL);
    check_str(out, "caf\xC3\xA9", "no predicate -> non-ASCII kept");
    kvscf_label_filter("caf\xC3\xA9", out, sizeof(out), ascii_only, NULL);
    check_str(out, "caf", "predicate drops the accented e");

    /* Nothing renderable at all leaves an empty string, not a box. */
    kvscf_label_filter("\xF0\x9F\xA6\x80", out, sizeof(out), ascii_only, NULL);
    check_str(out, "", "all-emoji label empties out");

    /* Plain text is untouched. */
    kvscf_label_filter("Work Items", out, sizeof(out), ascii_only, NULL);
    check_str(out, "Work Items", "plain ASCII passes through");

    /* Invalid UTF-8 is dropped rather than emitted. */
    kvscf_label_filter("a\xFF\xFE" "b", out, sizeof(out), NULL, NULL);
    check_str(out, "ab", "invalid bytes dropped");

    /* Truncation never splits a multi-byte sequence. */
    char tiny[6];
    kvscf_label_filter("caf\xC3\xA9\xC3\xA9", tiny, sizeof(tiny), NULL, NULL);
    check_str(tiny, "caf\xC3\xA9", "stops on a codepoint boundary");

    check(kvscf_label_filter(NULL, out, sizeof(out), NULL, NULL), 0, "NULL in -> 0");
    check_str(out, "", "NULL in empties out");
}

int main(void) {
    test_parse();
    test_display_host();
    test_display_label();
    test_sort();
    test_favorites_parse();
    test_favorites_sort();
    test_running_defaults_true();
    test_merge_across_hosts();
    test_edge_parse();
    test_edge_sort();
    test_apps_parse_sort();
    test_launch_payload();
    test_parse_tolerant();
    test_cap();
    test_app_color();
    test_paging();
    test_focus_payload();
    test_trim();
    test_launcher_parse();
    test_launcher_grid_required();
    test_launcher_skips_bad_buttons();
    test_launcher_span_occupancy();
    test_launcher_oversize_fields();
    test_launcher_cap();
    test_press_payload();
    test_button_rgb();
    test_label_filter();

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_kvscf_feed: all passed\n");
    return 0;
}
