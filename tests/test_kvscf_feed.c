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

    if (failures) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("test_kvscf_feed: all passed\n");
    return 0;
}
