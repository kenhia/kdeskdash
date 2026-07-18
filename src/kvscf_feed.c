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

            count++;
        }
    }

    cJSON_Delete(root);
    return count;
}

static int cmp_label(const void *a, const void *b) {
    const kvscf_instance_t *x = a, *y = b;
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
