/**
 * @file kvscf_feed.c
 * Pure core for the `foreground` mode — see kvscf_feed.h. No Redis, no LVGL.
 */
#include "kvscf_feed.h"

#include <stdio.h>
#include <stdlib.h> /* qsort */
#include <string.h>
#include <strings.h> /* strcasecmp */

#include "cJSON.h"
#include "palette.h"        /* named colours a launcher button may be authored in */
#include "telemetry_host.h" /* telemetry_host_token_ok — the host charset contract */

static void copy_field(char *dst, size_t dstsz, const char *src) {
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= dstsz)
        n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

size_t kvscf_trim_trailing(char *s) {
    if (!s)
        return 0;
    size_t n = strlen(s);
    while (n > 0) {
        unsigned char c = (unsigned char)s[n - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
            c == '\v')
            s[--n] = '\0';
        else
            break;
    }
    return n;
}

kv_app_t kvscf_app_from_str(const char *s) {
    if (!s)
        return KV_APP_UNKNOWN;
    if (strcmp(s, "stable") == 0)
        return KV_APP_STABLE;
    if (strcmp(s, "insiders") == 0)
        return KV_APP_INSIDERS;
    if (strcmp(s, "exploration") == 0)
        return KV_APP_EXPLORATION;
    return KV_APP_UNKNOWN;
}

static kv_remote_t remote_from_str(const char *s) {
    if (!s)
        return KV_REMOTE_UNKNOWN;
    if (strcmp(s, "local") == 0)
        return KV_REMOTE_LOCAL;
    if (strcmp(s, "ssh") == 0)
        return KV_REMOTE_SSH;
    if (strcmp(s, "wsl") == 0)
        return KV_REMOTE_WSL;
    if (strcmp(s, "devcontainer") == 0)
        return KV_REMOTE_DEVCONTAINER;
    if (strcmp(s, "codespaces") == 0)
        return KV_REMOTE_CODESPACES;
    return KV_REMOTE_UNKNOWN;
}

uint32_t kvscf_app_color(kv_app_t app) {
    switch (app) {
    case KV_APP_STABLE: return 0x60A5EB;      /* VS Code blue ("code")   */
    case KV_APP_INSIDERS: return 0x38BE84;    /* Insiders green          */
    default: return 0xE9EDF6;                 /* exploration/unknown: ink */
    }
}

/* Copy a JSON string field into dst; leaves dst "" for a null/absent/non-string
 * value (so `null` active_file/remote_host is handled). */
static void get_str(const cJSON *obj, const char *key, char *dst, size_t dstsz) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring)
        copy_field(dst, dstsz, it->valuestring);
    else
        dst[0] = '\0';
}

int kvscf_parse_append(const char *json, size_t len, kvscf_instance_t *arr,
                       int count, int max) {
    if (!json || len == 0 || !arr || count >= max)
        return count;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root)
        return count;

    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return count;
    }

    /* Publisher host — validated, and the focus-channel target. */
    const cJSON *hostj = cJSON_GetObjectItemCaseSensitive(root, "host");
    if (!cJSON_IsString(hostj) || !hostj->valuestring ||
        !telemetry_host_token_ok(hostj->valuestring, strlen(hostj->valuestring))) {
        cJSON_Delete(root);
        return count;
    }
    const char *host = hostj->valuestring;

    const cJSON *insts = cJSON_GetObjectItemCaseSensitive(root, "instances");
    if (cJSON_IsArray(insts)) {
        const cJSON *el = NULL;
        cJSON_ArrayForEach(el, insts) {
            if (count >= max)
                break;
            if (!cJSON_IsObject(el))
                continue;

            const cJSON *idj = cJSON_GetObjectItemCaseSensitive(el, "id");
            const cJSON *labelj = cJSON_GetObjectItemCaseSensitive(el, "label");
            /* id + label are mandatory: no id -> cannot focus; no label ->
             * nothing to show. */
            if (!cJSON_IsString(idj) || !idj->valuestring || !idj->valuestring[0] ||
                !cJSON_IsString(labelj) || !labelj->valuestring ||
                !labelj->valuestring[0])
                continue;

            kvscf_instance_t *r = &arr[count];
            memset(r, 0, sizeof(*r));
            copy_field(r->id, sizeof(r->id), idj->valuestring);
            copy_field(r->label, sizeof(r->label), labelj->valuestring);
            copy_field(r->host, sizeof(r->host), host);
            get_str(el, "remote_host", r->remote_host, sizeof(r->remote_host));
            get_str(el, "workspace", r->workspace, sizeof(r->workspace));
            get_str(el, "active_file", r->active_file, sizeof(r->active_file));

            char appbuf[16];
            get_str(el, "app", appbuf, sizeof(appbuf));
            r->app = kvscf_app_from_str(appbuf[0] ? appbuf : NULL);
            char rembuf[16];
            get_str(el, "remote", rembuf, sizeof(rembuf));
            r->remote = remote_from_str(rembuf[0] ? rembuf : NULL);

            const cJSON *zj = cJSON_GetObjectItemCaseSensitive(el, "z_index");
            if (cJSON_IsNumber(zj))
                r->z_index = (int)zj->valuedouble;

            /* Favorites (sprint 008). An absent `running` means an older
             * publisher that only ever listed open windows — treat as running. */
            const cJSON *runj = cJSON_GetObjectItemCaseSensitive(el, "running");
            r->running = cJSON_IsBool(runj) ? cJSON_IsTrue(runj) : true;
            r->favorite =
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(el, "favorite"));

            count++;
        }
    }

    cJSON_Delete(root);
    return count;
}

static int cmp_label(const void *a, const void *b) {
    const kvscf_instance_t *x = a, *y = b;
    if (x->running != y->running)
        return x->running ? -1 : 1; /* running block first, favorites after */
    int c = strcasecmp(x->label, y->label);
    if (c)
        return c;
    c = strcasecmp(x->host, y->host);
    if (c)
        return c;
    return strcmp(x->id, y->id);
}

void kvscf_sort_by_label(kvscf_instance_t *arr, int n) {
    if (arr && n > 1)
        qsort(arr, (size_t)n, sizeof(arr[0]), cmp_label);
}

const char *kvscf_display_host(const kvscf_instance_t *in) {
    if (!in)
        return "";
    return in->remote_host[0] ? in->remote_host : in->host;
}

void kvscf_display_label(const kvscf_instance_t *in, char *buf, size_t bufsz) {
    if (!buf || bufsz == 0)
        return;
    buf[0] = '\0';
    if (!in)
        return;
    copy_field(buf, bufsz, in->label);

    const char *host = kvscf_display_host(in);
    if (!host[0])
        return;
    /* Strip a trailing " (<host>)" only when it exactly matches. */
    char suffix[KV_HOST_MAX + 4];
    int sn = snprintf(suffix, sizeof(suffix), " (%s)", host);
    if (sn <= 0)
        return;
    size_t ln = strlen(buf), sl = (size_t)sn;
    if (ln >= sl && strcmp(buf + (ln - sl), suffix) == 0)
        buf[ln - sl] = '\0';
}

/* ---- Edge windows ----------------------------------------------------- */

int kvscf_parse_edge_append(const char *json, size_t len, kvscf_edge_t *arr,
                            int count, int max) {
    if (!json || len == 0 || !arr || count >= max)
        return count;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root)
        return count;
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return count;
    }

    const cJSON *hostj = cJSON_GetObjectItemCaseSensitive(root, "host");
    if (!cJSON_IsString(hostj) || !hostj->valuestring ||
        !telemetry_host_token_ok(hostj->valuestring, strlen(hostj->valuestring))) {
        cJSON_Delete(root);
        return count;
    }
    const char *host = hostj->valuestring;

    const cJSON *wins = cJSON_GetObjectItemCaseSensitive(root, "windows");
    if (cJSON_IsArray(wins)) {
        const cJSON *el = NULL;
        cJSON_ArrayForEach(el, wins) {
            if (count >= max)
                break;
            if (!cJSON_IsObject(el))
                continue;
            const cJSON *idj = cJSON_GetObjectItemCaseSensitive(el, "id");
            const cJSON *labelj = cJSON_GetObjectItemCaseSensitive(el, "label");
            if (!cJSON_IsString(idj) || !idj->valuestring || !idj->valuestring[0] ||
                !cJSON_IsString(labelj) || !labelj->valuestring ||
                !labelj->valuestring[0])
                continue;

            kvscf_edge_t *r = &arr[count];
            memset(r, 0, sizeof(*r));
            copy_field(r->id, sizeof(r->id), idj->valuestring);
            copy_field(r->label, sizeof(r->label), labelj->valuestring);
            copy_field(r->host, sizeof(r->host), host);

            const cJSON *nm = cJSON_GetObjectItemCaseSensitive(el, "named");
            r->named = cJSON_IsTrue(nm);
            const cJSON *tc = cJSON_GetObjectItemCaseSensitive(el, "tab_count");
            r->tab_count = cJSON_IsNumber(tc) ? (int)tc->valuedouble : -1;
            const cJSON *zj = cJSON_GetObjectItemCaseSensitive(el, "z_index");
            if (cJSON_IsNumber(zj))
                r->z_index = (int)zj->valuedouble;

            count++;
        }
    }

    cJSON_Delete(root);
    return count;
}

static int cmp_edge(const void *a, const void *b) {
    const kvscf_edge_t *x = a, *y = b;
    if (x->named != y->named)
        return x->named ? -1 : 1; /* named block first */
    int c = strcasecmp(x->label, y->label);
    if (c)
        return c;
    return strcmp(x->id, y->id);
}

void kvscf_sort_edge(kvscf_edge_t *arr, int n) {
    if (arr && n > 1)
        qsort(arr, (size_t)n, sizeof(arr[0]), cmp_edge);
}

/* ---- Configured apps -------------------------------------------------- */

int kvscf_parse_apps_append(const char *json, size_t len, kvscf_appitem_t *arr,
                            int count, int max) {
    if (!json || len == 0 || !arr || count >= max)
        return count;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root)
        return count;
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return count;
    }

    const cJSON *hostj = cJSON_GetObjectItemCaseSensitive(root, "host");
    if (!cJSON_IsString(hostj) || !hostj->valuestring ||
        !telemetry_host_token_ok(hostj->valuestring, strlen(hostj->valuestring))) {
        cJSON_Delete(root);
        return count;
    }
    const char *host = hostj->valuestring;

    const cJSON *apps = cJSON_GetObjectItemCaseSensitive(root, "apps");
    if (cJSON_IsArray(apps)) {
        const cJSON *el = NULL;
        cJSON_ArrayForEach(el, apps) {
            if (count >= max)
                break;
            if (!cJSON_IsObject(el))
                continue;
            const cJSON *keyj = cJSON_GetObjectItemCaseSensitive(el, "key");
            const cJSON *labelj = cJSON_GetObjectItemCaseSensitive(el, "label");
            if (!cJSON_IsString(keyj) || !keyj->valuestring || !keyj->valuestring[0] ||
                !cJSON_IsString(labelj) || !labelj->valuestring ||
                !labelj->valuestring[0])
                continue;

            kvscf_appitem_t *r = &arr[count];
            memset(r, 0, sizeof(*r));
            copy_field(r->key, sizeof(r->key), keyj->valuestring);
            copy_field(r->label, sizeof(r->label), labelj->valuestring);
            copy_field(r->host, sizeof(r->host), host);
            r->running =
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(el, "running"));
            const cJSON *oj = cJSON_GetObjectItemCaseSensitive(el, "order");
            if (cJSON_IsNumber(oj))
                r->order = (int)oj->valuedouble;

            count++;
        }
    }

    cJSON_Delete(root);
    return count;
}

static int cmp_apps(const void *a, const void *b) {
    const kvscf_appitem_t *x = a, *y = b;
    if (x->order != y->order)
        return x->order < y->order ? -1 : 1;
    int c = strcasecmp(x->label, y->label);
    if (c)
        return c;
    return strcmp(x->key, y->key);
}

void kvscf_sort_apps(kvscf_appitem_t *arr, int n) {
    if (arr && n > 1)
        qsort(arr, (size_t)n, sizeof(arr[0]), cmp_apps);
}

size_t kvscf_launch_payload(const char *token, const char *app_key, char *buf,
                            size_t bufsz) {
    if (!buf || bufsz == 0)
        return 0;
    buf[0] = '\0';
    if (!token || !token[0] || !app_key || !app_key[0])
        return 0;
    int n = snprintf(buf, bufsz, "{\"token\":\"%s\",\"app\":\"%s\"}", token,
                     app_key);
    if (n < 0 || (size_t)n >= bufsz) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

/* ---- Launcher buttons ------------------------------------------------- */

/* An optional integer field. Absent/non-numeric yields `dflt`, which is how
 * w/h default to 1 while row/col stay mandatory (their default is out of
 * range, so an absent one is skipped rather than silently placed at 0,0). */
static int get_int(const cJSON *obj, const char *key, int dflt) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(it) ? (int)it->valuedouble : dflt;
}

bool kvscf_parse_launcher(const char *json, size_t len, kvscf_launcher_t *out) {
    if (!json || len == 0 || !out)
        return false;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root)
        return false;
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *hostj = cJSON_GetObjectItemCaseSensitive(root, "host");
    if (!cJSON_IsString(hostj) || !hostj->valuestring ||
        !telemetry_host_token_ok(hostj->valuestring, strlen(hostj->valuestring))) {
        cJSON_Delete(root);
        return false;
    }

    /* The grid is published, never assumed — no grid means no way to place a
     * button, so the whole payload is unusable and the caller keeps its cache. */
    const cJSON *gridj = cJSON_GetObjectItemCaseSensitive(root, "grid");
    int rows = cJSON_IsObject(gridj) ? get_int(gridj, "rows", 0) : 0;
    int cols = cJSON_IsObject(gridj) ? get_int(gridj, "cols", 0) : 0;
    if (rows < 1 || rows > KV_GRID_ROWS_MAX || cols < 1 || cols > KV_GRID_COLS_MAX) {
        cJSON_Delete(root);
        return false;
    }

    /* Past this point the payload is committed: write `out` and never fail. */
    memset(out, 0, sizeof(*out));
    copy_field(out->host, sizeof(out->host), hostj->valuestring);
    out->rows = rows;
    out->cols = cols;
    const cJSON *tsj = cJSON_GetObjectItemCaseSensitive(root, "ts");
    if (cJSON_IsNumber(tsj))
        out->ts = (long long)tsj->valuedouble;

    /* Cell occupancy, so an overlap can be refused earlier-wins the same way
     * the publisher already does. Sized off the ceilings, indexed by the
     * *published* grid. */
    bool occ[KV_GRID_ROWS_MAX * KV_GRID_COLS_MAX];
    memset(occ, 0, sizeof(occ));

    const cJSON *btns = cJSON_GetObjectItemCaseSensitive(root, "buttons");
    if (cJSON_IsArray(btns)) {
        const cJSON *el = NULL;
        cJSON_ArrayForEach(el, btns) {
            if (out->count >= KV_BUTTONS_MAX) {
                out->skipped++;
                continue;
            }
            if (!cJSON_IsObject(el)) {
                out->skipped++;
                continue;
            }

            const cJSON *keyj = cJSON_GetObjectItemCaseSensitive(el, "key");
            const cJSON *labelj = cJSON_GetObjectItemCaseSensitive(el, "label");
            if (!cJSON_IsString(keyj) || !keyj->valuestring || !keyj->valuestring[0] ||
                !cJSON_IsString(labelj) || !labelj->valuestring ||
                !labelj->valuestring[0]) {
                out->skipped++;
                continue;
            }
            /* Reject rather than truncate: a clipped key presses the wrong
             * button. The label may clip — that is only cosmetic. */
            if (strlen(keyj->valuestring) >= KV_BTNKEY_MAX) {
                out->skipped++;
                continue;
            }

            int row = get_int(el, "row", -1);
            int col = get_int(el, "col", -1);
            int w = get_int(el, "w", 1);
            int h = get_int(el, "h", 1);
            if (row < 0 || col < 0 || w < 1 || h < 1 || w > KV_SPAN_MAX ||
                h > KV_SPAN_MAX || row + h > rows || col + w > cols) {
                out->skipped++;
                continue;
            }

            bool clash = false;
            for (int r = row; r < row + h && !clash; r++)
                for (int c = col; c < col + w; c++)
                    if (occ[r * cols + c]) {
                        clash = true;
                        break;
                    }
            if (clash) {
                out->skipped++;
                continue;
            }
            for (int r = row; r < row + h; r++)
                for (int c = col; c < col + w; c++)
                    occ[r * cols + c] = true;

            kvscf_button_t *b = &out->buttons[out->count++];
            memset(b, 0, sizeof(*b));
            copy_field(b->key, sizeof(b->key), keyj->valuestring);
            copy_field(b->label, sizeof(b->label), labelj->valuestring);
            get_str(el, "color", b->color, sizeof(b->color));
            b->row = row;
            b->col = col;
            b->w = w;
            b->h = h;
        }
    }

    cJSON_Delete(root);
    return true;
}

size_t kvscf_press_payload(const char *token, const char *button_key, char *buf,
                           size_t bufsz) {
    if (!buf || bufsz == 0)
        return 0;
    buf[0] = '\0';
    if (!token || !token[0] || !button_key || !button_key[0])
        return 0; /* R8: never a payload without a token */
    int n = snprintf(buf, bufsz, "{\"token\":\"%s\",\"button\":\"%s\"}", token,
                     button_key);
    if (n < 0 || (size_t)n >= bufsz) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool kvscf_button_rgb(const char *color, uint32_t *out_rgb) {
    if (!color || !out_rgb)
        return false;
    const char *s = color[0] == '#' ? color + 1 : color;
    if (strlen(s) == 6) {
        uint32_t v = 0;
        int i = 0;
        for (; i < 6; i++) {
            int nib = hex_nibble(s[i]);
            if (nib < 0)
                break;
            v = (v << 4) | (uint32_t)nib;
        }
        if (i == 6) {
            *out_rgb = v;
            return true;
        }
    }
    /* Not hex — try the repo's named palette (case-insensitive). */
    int idx = kd_pal_find(color);
    if (idx < 0)
        return false;
    *out_rgb = kd_pal_rgb(idx);
    return true;
}

/* ---- Label filtering (no tofu) ---------------------------------------- */

/* Decode one UTF-8 codepoint at `p` (bounded by `end`). Returns the byte length
 * consumed and writes the codepoint, or 0 for an invalid/truncated sequence —
 * strict enough to reject overlongs and surrogates, since a label arrives over
 * the wire from another machine. */
static size_t utf8_decode(const unsigned char *p, const unsigned char *end,
                          uint32_t *cp) {
    if (p >= end)
        return 0;
    unsigned char c = p[0];
    size_t n;
    uint32_t v;
    if (c < 0x80) {
        *cp = c;
        return 1;
    } else if ((c & 0xE0) == 0xC0) {
        n = 2;
        v = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
        n = 3;
        v = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
        n = 4;
        v = c & 0x07u;
    } else {
        return 0;
    }
    if ((size_t)(end - p) < n)
        return 0;
    for (size_t i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80)
            return 0;
        v = (v << 6) | (uint32_t)(p[i] & 0x3F);
    }
    if ((n == 2 && v < 0x80) || (n == 3 && v < 0x800) || (n == 4 && v < 0x10000))
        return 0; /* overlong */
    if (v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))
        return 0;
    *cp = v;
    return n;
}

/* Codepoints no monochrome bitmap/outline renderer here can ever draw usefully,
 * dropped before the predicate even sees them. */
static bool always_drop(uint32_t cp) {
    return cp == 0x200D ||                    /* zero-width joiner        */
           cp == 0xFE0E || cp == 0xFE0F ||    /* variation selectors 15/16 */
           (cp >= 0x1F3FB && cp <= 0x1F3FF);  /* skin tone modifiers      */
}

size_t kvscf_label_filter(const char *in, char *out, size_t outsz,
                          bool (*renderable)(uint32_t cp, void *ctx), void *ctx) {
    if (!out || outsz == 0)
        return 0;
    out[0] = '\0';
    if (!in)
        return 0;

    const unsigned char *p = (const unsigned char *)in;
    const unsigned char *end = p + strlen(in);
    size_t w = 0;
    bool pending_space = false; /* emit at most one space, and none leading */

    while (p < end) {
        uint32_t cp = 0;
        size_t n = utf8_decode(p, end, &cp);
        if (n == 0) {
            p++; /* invalid byte: drop it, keep scanning */
            continue;
        }
        const unsigned char *seq = p;
        p += n;

        if (always_drop(cp))
            continue;
        if (renderable && !renderable(cp, ctx))
            continue;

        /* Collapse the gaps a dropped codepoint leaves behind. */
        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') {
            pending_space = (w > 0);
            continue;
        }
        if (pending_space) {
            if (w + 1 >= outsz)
                break;
            out[w++] = ' ';
            pending_space = false;
        }
        if (w + n >= outsz)
            break; /* never split a multi-byte sequence */
        memcpy(out + w, seq, n);
        w += n;
    }

    out[w] = '\0';
    return w;
}

int kvscf_page_count(int n, int per_page) {
    if (per_page <= 0 || n <= 0)
        return 1;
    return (n + per_page - 1) / per_page;
}

int kvscf_clamp_page(int page, int n, int per_page) {
    int pages = kvscf_page_count(n, per_page);
    if (page < 0)
        return 0;
    if (page >= pages)
        return pages - 1;
    return page;
}

size_t kvscf_focus_payload(const char *token, const char *id, bool maximize,
                           char *buf, size_t bufsz) {
    if (!buf || bufsz == 0)
        return 0;
    buf[0] = '\0';
    if (!token || !token[0] || !id || !id[0])
        return 0; /* R8: never a payload without a token/id */
    int n = snprintf(buf, bufsz, "{\"token\":\"%s\",\"id\":\"%s\",\"maximize\":%s}",
                     token, id, maximize ? "true" : "false");
    if (n < 0 || (size_t)n >= bufsz) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n;
}
