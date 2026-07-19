/**
 * @file kvscf_feed.h
 * Pure core for the `foreground` (R4gnd) mode: parse the kvscf window feed,
 * order it, page it, colour it, and build the focus-command payload. No Redis,
 * no LVGL, host-testable.
 *
 * Feed contract (published by kvscf on `cleo`, frozen 2026-07-18 — see
 * docs/brainstorms/2026-07-18-remote-foreground-mode-requirements.md):
 *   kvscf:instances:<host>   String = JSON { "host", "instances":[ {…} ] }, TTL 10s
 *     instance: id(HWND string, opaque focus token), label, remote(local|ssh|…),
 *               remote_host(nullable), app(stable|insiders|exploration|unknown),
 *               workspace, active_file(nullable), z_index(int)
 *   kvscf:focus:<host>       pub/sub channel; payload {token,id,maximize}
 *
 * Two distinct uses of "host": the **focus channel** targets the *publisher*
 * host (where the window physically is), i.e. the record's `host`; the **display
 * line** shows where the code runs, i.e. `remote_host` falling back to `host`.
 */
#ifndef KDESKDASH_KVSCF_FEED_H
#define KDESKDASH_KVSCF_FEED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KV_HOST_MAX      64
#define KV_ID_MAX        24 /* HWND decimal string */
#define KV_LABEL_MAX     64 /* kvscf already ellipsis-truncates long labels */
#define KV_WORKSPACE_MAX 48
#define KV_FILE_MAX      64

/* Total windows tracked across all hosts (paged KV_PER_PAGE at a time). */
#define KV_INSTANCES_MAX 64

/* Window grid: 4×7 = 28 cells this sprint (5×7 reserved for the Edge view). */
#define KV_COLS     4
#define KV_ROWS     7
#define KV_PER_PAGE (KV_COLS * KV_ROWS)

#define KV_TOKEN_MAX 128

typedef enum {
    KV_APP_STABLE = 0,
    KV_APP_INSIDERS,
    KV_APP_EXPLORATION,
    KV_APP_UNKNOWN,
} kv_app_t;

typedef enum {
    KV_REMOTE_LOCAL = 0,
    KV_REMOTE_SSH,
    KV_REMOTE_WSL,
    KV_REMOTE_DEVCONTAINER,
    KV_REMOTE_CODESPACES,
    KV_REMOTE_UNKNOWN,
} kv_remote_t;

typedef struct {
    char        id[KV_ID_MAX];             /* opaque focus token (HWND)         */
    char        label[KV_LABEL_MAX];       /* window title (rendered)           */
    char        host[KV_HOST_MAX];         /* publisher host — focus channel    */
    char        remote_host[KV_HOST_MAX];  /* "" when null (local)              */
    char        workspace[KV_WORKSPACE_MAX];
    char        active_file[KV_FILE_MAX];  /* "" when null                      */
    kv_app_t    app;
    kv_remote_t remote;
    int         z_index; /* parsed but unused (we sort by label) */
} kvscf_instance_t;

/* Trim trailing whitespace/CR/LF in place (byte-exact token matching depends on
 * this — the secret can originate CRLF on Windows). Returns the new length. */
size_t kvscf_trim_trailing(char *s);

/* Parse one host's `{host,instances:[…]}` JSON String, appending each valid
 * instance to `arr` (already holding `count`, capacity `max`). Tolerates null
 * `active_file`/`remote_host`, missing optional fields, and skips malformed
 * entries and any whose `host`/`id`/`label` is absent or invalid. Returns the
 * new count. */
int kvscf_parse_append(const char *json, size_t len, kvscf_instance_t *arr,
                       int count, int max);

/* Stable case-insensitive alphabetical sort by `label` (tie-break host, then id
 * for determinism). This is the display order — our own, not `z_index`. */
void kvscf_sort_by_label(kvscf_instance_t *arr, int n);

/* ---- Edge windows (kvscf:edge:<host>) --------------------------------- */

/* One Microsoft Edge window. `id` is the same kind of opaque HWND focus token as
 * a VS Code instance — the focus command is identical. */
typedef struct {
    char id[KV_ID_MAX];
    char label[KV_LABEL_MAX]; /* user window name (named) or active tab title */
    char host[KV_HOST_MAX];   /* publisher host — the focus channel            */
    bool named;               /* true = user-named window; false = tab-derived  */
    int  tab_count;           /* unnamed best-effort count; -1 when null/named  */
    int  z_index;
} kvscf_edge_t;

/* Parse one host's `{host,windows:[…]}` Edge JSON String, appending each valid
 * window to `arr` (holding `count`, capacity `max`). Same discipline as
 * kvscf_parse_append: host validated, id/label required, null `tab_count` -> -1,
 * malformed skipped. Returns the new count. */
int kvscf_parse_edge_append(const char *json, size_t len, kvscf_edge_t *arr,
                            int count, int max);

/* Sort Edge windows: named block first, then unnamed block; each block
 * case-insensitive alphabetical by `label` (tie-break id). */
void kvscf_sort_edge(kvscf_edge_t *arr, int n);

/* ---- Configured apps (kvscf:apps:<host>) ------------------------------ */

#define KV_APPKEY_MAX 32

/* One configured app — focus-if-running-else-launch. Unlike Code/Edge the focus
 * command carries the stable `key` (not an HWND), since a non-running app has no
 * window. */
typedef struct {
    char key[KV_APPKEY_MAX]; /* stable app id — echoed back in the launch command */
    char label[KV_LABEL_MAX];
    char host[KV_HOST_MAX];  /* publisher host — the launch channel               */
    bool running;            /* false -> render greyed                            */
    int  order;              /* configured sort index                             */
} kvscf_appitem_t;

/* Parse one host's `{host,apps:[…]}` JSON String, appending each valid app.
 * key/label required; `running` bool; `id` ignored (we command by key). Returns
 * the new count. */
int kvscf_parse_apps_append(const char *json, size_t len, kvscf_appitem_t *arr,
                            int count, int max);

/* Sort apps by configured `order`, then case-insensitive `label`, then `key`. */
void kvscf_sort_apps(kvscf_appitem_t *arr, int n);

/* Build the launch-or-focus payload `{"token":"…","app":"…"}` into buf. Returns
 * the length written, or 0 if token/key is empty or buf too small (buf set to
 * ""). The Apps command is keyed by `app`, not `id`. */
size_t kvscf_launch_payload(const char *token, const char *app_key, char *buf,
                            size_t bufsz);

/* The host to *display* for an instance: `remote_host` if set, else `host`. */
const char *kvscf_display_host(const kvscf_instance_t *in);

/* The label to *display*, with a redundant trailing " (<display-host>)" suffix
 * stripped (kvscf bakes e.g. "klams (kubs0)" into the label, but the host is
 * shown separately). Only strips when the parenthesised token exactly matches
 * the display host. Writes a NUL-terminated result into buf. */
void kvscf_display_label(const kvscf_instance_t *in, char *buf, size_t bufsz);

/* Label colour (0xRRGGBB) for an app variant — mirrors the cleo side. */
uint32_t kvscf_app_color(kv_app_t app);

/* app enum from its string (unknown on NULL/unrecognised). */
kv_app_t kvscf_app_from_str(const char *s);

/* Grid paging (always >= 1 page). */
int kvscf_page_count(int n, int per_page);
int kvscf_clamp_page(int page, int n, int per_page);

/* Build the focus-command payload `{"token":"…","id":"…","maximize":…}` into buf.
 * Returns the length written, or 0 if token/id is empty or buf too small (in
 * which case buf is set to ""). Guards R8: no payload without a token. */
size_t kvscf_focus_payload(const char *token, const char *id, bool maximize,
                           char *buf, size_t bufsz);

#endif /* KDESKDASH_KVSCF_FEED_H */
