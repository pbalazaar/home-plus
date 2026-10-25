/* Home+ native: small helpers: logging to the service, toasts, strings and JSON, an app's name and colour. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "home.h"

/* ------------------------------------------------------------------ helpers */

void log_remote(const char *fmt, ...)
{
    char msg[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "homeplus: %s\n", msg);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "message", msg);
    char *body = cJSON_PrintUnformatted(o);
    net_post("/api/log", body, 0);
    free(body);
    cJSON_Delete(o);
}

void toast(const char *text, int ms)
{
    tex_free(&S.toast_tex);
    S.toast_tex = text_render(S.f_medium, 26, text, 1300, &S.toast_w, &S.toast_h);
    S.toast_until = SDL_GetTicks() + ms;
    S.toast_t = S.toast_v = 0;
}

int toast_on(void) { return S.toast_tex && SDL_GetTicks() < S.toast_until; }

/* appends src to dst (capacity cap), cut short when full */
void append(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(dst);
    if (n + 1 < cap) snprintf(dst + n, cap - n, "%s", src);
}

void copy_id(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n >= ID_LEN) n = ID_LEN - 1;
    memmove(dst, src, n);
    dst[n] = 0;
}

int parse_hex(const char *s, Uint8 out[3])
{
    unsigned v;
    if (!s || s[0] != '#' || strlen(s) != 7 || sscanf(s + 1, "%6x", &v) != 1) return 0;
    out[0] = (v >> 16) & 255;
    out[1] = (v >> 8) & 255;
    out[2] = v & 255;
    return 1;
}

const char *jstr(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

double jnum(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

App *item_app(int item)
{
    if (item < 0 || item >= S.nitems || S.items[item] < 0) return NULL;
    return &S.apps[S.items[item]];
}

/* case-insensitive substring test (SDL_strcasestr needs SDL 2.24; the TV has 2.0.14) */
int contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

/* Other apps for Jellyfin and Plex, from Homebrew Channel, whose names don't say so (webosbrew/apps-repo) */
static const struct { const char *id; int src; } CLIENTS[] = {
    { "org.litefin.app", SRC_JELLYFIN },   /* Litefin */
    { "org.moonfin.webos", SRC_JELLYFIN }, /* Moonfin */
    { "com.breezyfin.app", SRC_JELLYFIN }, /* Breezyfin */
    { "com.ocenfin.app", SRC_JELLYFIN },   /* OcenFin */
    { "com.beb.plxnative", SRC_PLEX },     /* PlxNative */
};

/* The media source an app's Top Shelf shows, by its id or name (store builds differ). */
int media_source(const App *a)
{
    if (!a) return SRC_NONE;
    for (size_t i = 0; i < sizeof CLIENTS / sizeof *CLIENTS; i++)
        if (!strcmp(a->id, CLIENTS[i].id)) return CLIENTS[i].src;
    if (contains_ci(a->id, "stremio") || contains_ci(a->title, "stremio")) return SRC_STREMIO;
    if (contains_ci(a->id, "jellyfin") || contains_ci(a->title, "jellyfin")) return SRC_JELLYFIN;
    if (contains_ci(a->id, "plex") || contains_ci(a->title, "plex") || !strcmp(a->id, "cdp-30")) return SRC_PLEX;
    return SRC_NONE;
}

/* An app's short name: LG's titles can be long ("Spotify - Music and Podcasts"),
 * so labels stop at the first " - " or ": " (tvOS shows short names too). */
void short_title(const App *a, char *out, size_t cap)
{
    snprintf(out, cap, "%s", a->title);
    static const char *const cuts[] = { " - ", " \xE2\x80\x93 ", " \xE2\x80\x94 ", ": " };
    for (size_t i = 0; i < sizeof cuts / sizeof *cuts; i++) {
        char *c = strstr(out, cuts[i]);
        if (c && c - out >= 3) *c = 0;
    }
}

int is_hidden(const char *id)
{
    for (int i = 0; i < S.nhidden; i++) if (!strcmp(S.hidden[i], id)) return 1;
    return 0;
}

void app_color(const App *a, Uint8 *r, Uint8 *g, Uint8 *b)
{
    if (a && (a->has_col || a->fullbleed)) {
        *r = a->col[0]; *g = a->col[1]; *b = a->col[2];
    } else {
        *r = 70; *g = 70; *b = 82;
    }
}

/* The first of n items to show, `visible` at a time, so that item i stays in view
 * with `margin` items around it: the list only scrolls when i would get closer
 * to an edge than that (or leave it). */
int keep_in_view(int first, int i, int visible, int n, int margin)
{
    if (first > i - margin) first = i - margin;
    if (first < i + margin - (visible - 1)) first = i + margin - (visible - 1);
    if (first > n - visible) first = n - visible;
    return first < 0 ? 0 : first;
}
