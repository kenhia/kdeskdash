/**
 * @file kvscf_feed.h
 * Pure core for the `foreground` (R4gnd) mode: parse the kvscf window feed,
 * order it, page it, colour it, and build the focus-command payload. No Redis,
 * no LVGL, host-testable.
 *
 * Feed contract (published by kvscf on `cleo`, frozen 2026-07-18 — see
 * sprints/011-remote-foreground-mode/requirements.md):
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
/* Focus token. Usually an HWND decimal string, but a non-running Code favorite
 * carries a *folder URI* instead (e.g.
 * "vscode-remote://ssh-remote%2Bkai/home/ken/src/ai-agents/harness-eval"), so this
 * must be generous — a truncated id would publish a broken relaunch command. */
#define KV_ID_MAX        256
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
    bool        running;  /* false = a favorite with no open window (relaunch) */
    bool        favorite; /* one of Ken's favorites, open or not               */
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

/* Display order: the `running` block first, then the non-running (favorite)
 * block; each block case-insensitive alphabetical by `label` (tie-break host,
 * then id). Our own order — not `z_index`. */
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

/* ---- Launcher buttons (kvscf:launcher:<host>) -------------------------- */

/* Grid bounds. The *published* grid is authoritative — never hard-code 3×9;
 * these are only the ceilings a malformed feed cannot push a buffer past. */
#define KV_GRID_ROWS_MAX 6
#define KV_GRID_COLS_MAX 12
#define KV_BUTTONS_MAX   (KV_GRID_ROWS_MAX * KV_GRID_COLS_MAX)
#define KV_SPAN_MAX      3 /* w,h are 1..3 per the contract */

/* Sprint 008 shipped a silent truncation bug because a field sized for one kind
 * of value met another. So: a `key` that does not fit is **rejected**, never
 * truncated — a truncated key presses the wrong button (or none). A `label`
 * that does not fit is truncated, which is merely cosmetic. */
#define KV_BTNKEY_MAX   48
#define KV_BTNLABEL_MAX 48
#define KV_COLOR_MAX    24 /* "#rrggbb" or a palette name */

typedef struct {
    char key[KV_BTNKEY_MAX];     /* stable id, echoed back in the press command */
    char label[KV_BTNLABEL_MAX]; /* ready-to-render text; may carry emoji       */
    char color[KV_COLOR_MAX];    /* "#rrggbb" or palette name; "" = default     */
    int  row, col;               /* 0-based top-left cell                       */
    int  w, h;                   /* span in cells, 1..KV_SPAN_MAX               */
} kvscf_button_t;

typedef struct {
    char           host[KV_HOST_MAX]; /* publisher host — the press channel */
    long long      ts;                /* publisher timestamp (0 when absent) */
    int            rows, cols;        /* the published grid, as published */
    kvscf_button_t buttons[KV_BUTTONS_MAX];
    int            count;
    int            skipped; /* buttons validation dropped on this parse */
} kvscf_launcher_t;

/* Parse one host's `{host,ts,grid,buttons:[…]}` launcher JSON into `out`, which
 * is fully overwritten on success and left untouched on failure — so a caller
 * holding a last-good config keeps it (cache-and-dim; a work machine sleeps).
 *
 * Rejects the whole payload when the envelope is unusable: bad JSON, invalid
 * `host`, or a missing/out-of-range `grid` (the grid is on the wire precisely so
 * neither side assumes it). Individual buttons are validated again even though
 * kvscf already did — a feed is not a promise — and a bad one is skipped and
 * counted in `skipped` rather than failing the mode. Overlaps resolve
 * earlier-wins, matching the publisher. Returns true if `out` was written. */
bool kvscf_parse_launcher(const char *json, size_t len, kvscf_launcher_t *out);

/* Build the press payload `{"token":"…","button":"…"}` into buf. Returns the
 * length written, or 0 (buf "") if token/key is empty or buf is too small.
 * Same R8 guard as the other payload builders: no payload without a token. */
size_t kvscf_press_payload(const char *token, const char *button_key, char *buf,
                           size_t bufsz);

/* Resolve a button's authored `color` to 0xRRGGBB: "#rrggbb"/"rrggbb" hex, or a
 * case-insensitive name from the kdeskdash palette (palette.h). Returns false
 * for empty/unparseable/unknown — the caller uses its default rather than
 * dropping the button. */
bool kvscf_button_rgb(const char *color, uint32_t *out_rgb);

/* Copy `in` to `out`, dropping every codepoint that would render as tofu.
 *
 * The vendored Symbols Nerd Font carries **no** emoji and no Latin, and
 * Montserrat carries no emoji, so a label like "🦀 Rust Docs" would draw a
 * replacement box on a launcher button. Rather than ship that, drop what cannot
 * be drawn: always the zero-width joiner, the variation selectors and the skin
 * tone modifiers (pure noise for a monochrome renderer), plus any codepoint
 * `renderable` rejects. `renderable` may be NULL to apply only the always-drop
 * set. Runs of whitespace left behind collapse, and the result is trimmed, so a
 * dropped leading emoji does not leave the text indented.
 *
 * The predicate is a seam: the mode backs it with a cache-less TinyTTF probe
 * plus the built-in font, and the tests back it with a stub. Returns the length
 * written (0 and out "" on bad arguments). */
size_t kvscf_label_filter(const char *in, char *out, size_t outsz,
                          bool (*renderable)(uint32_t cp, void *ctx), void *ctx);

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
