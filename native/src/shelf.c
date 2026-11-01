/* Home+ native: the Top Shelf: an app's pictures, and the carousel of Continue Watching or popular titles. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "home.h"

#define FEATURED_TITLE_Y 210
#define FEATURED_META_Y 415
#define FEATURED_OVERVIEW_Y 465
#define CONTINUE_RESUME_Y 665
#define CONTINUE_PROGRESS_Y 710
#define CONTINUE_DOTS_Y 748
#define BACKDROP_IMAGE_OPACITY 0.8f
#define RAIL_TEXT_MAX_W (W / 2 - LEFT - 5) /* keep the text and its shadow in the left half */
#define SHELF_DETAIL_ALPHA 230 /* readable over bright artwork without a text panel */

static int rail_has_details(const RailItem *it)
{
    return it->genre[0] || it->rating > 0 || it->overview[0];
}

static int rail_enriched(const RailItem *it)
{
    return S.shelf_details != SHELF_DETAILS_OFF &&
           (S.rail_source == SRC_FEATURED || S.rail_source == SRC_STREMIO) && rail_has_details(it);
}

static int pic_eq(Pic a, Pic b) { return a.kind == b.kind && (a.kind == PIC_NONE || a.i == b.i); }

/* a layer lets go of its picture (a held one is freed) */
static void pic_drop(Pic *p)
{
    if (p->kind == PIC_HELD) tex_free(&S.pic_held[p->i]);
    *p = (Pic){ PIC_NONE, 0 };
}

/* The owner of layer p's picture is about to free it (*tex): the layer holds it instead,
 * in the slot the other layer isn't using, until something covers it. */
void pic_hold(Pic *p, Tex **tex)
{
    Pic *o = p == &S.pic_top ? &S.pic_under : &S.pic_top;
    int s = o->kind == PIC_HELD && o->i == 0;
    tex_free(&S.pic_held[s]);
    if (tex && *tex) {
        S.pic_held[s] = *tex;
        *tex = NULL;
        *p = (Pic){ PIC_HELD, s };
    } else {
        *p = (Pic){ PIC_NONE, 0 };
    }
}

/* the carousel title i's picture is on show (or fading in or out) */
int pic_rail(int i)
{
    return (S.pic_under.kind == PIC_RAIL && S.pic_under.i == i) || (S.pic_top.kind == PIC_RAIL && S.pic_top.i == i);
}

/* Wrap the synopsis at the left half of the screen; the fifth line shows an ellipsis if needed. */
static const char *overview_line(const char *from, char *line, size_t cap, int last)
{
    while (*from == ' ') from++;
    const char *best = NULL, *word = from;
    while (*word) {
        const char *end = strchr(word, ' ');
        if (!end) end = word + strlen(word);
        size_t n = (size_t)(end - from);
        if (n > cap - 4) break; /* leave room for an ellipsis */
        memcpy(line, from, n);
        line[n] = 0;
        const char *next = end;
        while (*next == ' ') next++;
        if (last && *next) append(line, cap, "\xE2\x80\xA6");
        if (text_width(S.f_medium, HINT_PX, line) > RAIL_TEXT_MAX_W) break;
        best = end;
        if (!*next) break;
        word = next;
    }
    if (!best) { /* a single word may need text_render's own ellipsis */
        best = strchr(from, ' ');
        if (!best) best = from + strlen(from);
    }
    size_t n = (size_t)(best - from);
    if (n > cap - 4) {
        n = cap - 4;
        while (n && ((unsigned char)from[n] & 0xc0) == 0x80) n--;
    }
    memcpy(line, from, n);
    line[n] = 0;
    const char *next = best;
    while (*next == ' ') next++;
    if (last && *next) append(line, cap, "\xE2\x80\xA6");
    return next;
}

/* the big title and details, made only for the card on show */
static void ensure_rail_texts(RailItem *it)
{
    if (it->texts_made) return; /* (not title_tex: a name that renders to nothing made them every frame) */
    int enriched = rail_enriched(it);
    /* Streaming titles lead with type, genre, year and rating; a paused title
     * keeps its episode and time left on a separate line below the synopsis. */
    char meta[200] = "";
    if (enriched && S.rail_source == SRC_STREMIO && !strcmp(it->type, "series"))
        snprintf(meta, sizeof meta, "Series");
    else if (enriched && S.rail_source == SRC_STREMIO && !strcmp(it->type, "movie"))
        snprintf(meta, sizeof meta, "Movie");
    else if (!strcmp(it->type, "series") && it->season)
        snprintf(meta, sizeof meta, "Season %d  \xC2\xB7  Episode %d", it->season, it->episode);
    else if (it->episode)
        snprintf(meta, sizeof meta, "Episode %d", it->episode);
    else if (!strcmp(it->type, "movie"))
        snprintf(meta, sizeof meta, "Movie");
    else if (!strcmp(it->type, "series"))
        snprintf(meta, sizeof meta, "Series");
    if (enriched && it->genre[0]) {
        if (meta[0]) append(meta, sizeof meta, "  \xC2\xB7  ");
        append(meta, sizeof meta, it->genre);
    }
    if (it->year) { /* popular titles: "Series · 2026" */
        char yr[24];
        snprintf(yr, sizeof yr, "%s%d", meta[0] ? "  \xC2\xB7  " : "", it->year);
        append(meta, sizeof meta, yr);
    }
    if (enriched && it->rating > 0) {
        char rated[48];
        snprintf(rated, sizeof rated, "%sTMDB %.1f/10", meta[0] ? "  \xC2\xB7  " : "", it->rating);
        append(meta, sizeof meta, rated);
    }
    char resume[96] = "";
    if (enriched && S.rail_source == SRC_STREMIO && it->episode) {
        if (it->season)
            snprintf(resume, sizeof resume, "Season %d  \xC2\xB7  Episode %d", it->season, it->episode);
        else snprintf(resume, sizeof resume, "Episode %d", it->episode);
    }
    if (it->remaining) {
        char left[48];
        if (enriched && S.rail_source == SRC_STREMIO) {
            snprintf(left, sizeof left, "%s%d min left", resume[0] ? "  \xC2\xB7  " : "", it->remaining);
            append(resume, sizeof resume, left);
        } else {
            snprintf(left, sizeof left, "%s%d min left", meta[0] ? "  \xC2\xB7  " : "", it->remaining);
            append(meta, sizeof meta, left);
        }
    }
    tex_free(&it->title_tex);
    tex_free(&it->meta_tex);
    tex_free(&it->resume_tex);
    for (int l = 0; l < OVERVIEW_LINES; l++) tex_free(&it->overview_tex[l]);
    it->title_tex = text_render(S.f_bold, 66, it->name, RAIL_TEXT_MAX_W, &it->tw, &it->th);
    it->meta_tex = text_render(S.f_medium, 27, meta, RAIL_TEXT_MAX_W, &it->mw, &it->mh);
    if (resume[0]) it->resume_tex = text_render(S.f_medium, HINT_PX, resume, RAIL_TEXT_MAX_W, &it->rw, &it->rh);
    if (S.shelf_details == SHELF_DETAILS_FULL && it->overview[0]) {
        char line[96];
        const char *next = it->overview;
        for (int l = 0; l < OVERVIEW_LINES && *next; l++) {
            next = overview_line(next, line, sizeof line, l == OVERVIEW_LINES - 1);
            it->overview_tex[l] = text_render(S.f_medium, HINT_PX, line, RAIL_TEXT_MAX_W, &it->ow[l], &it->oh[l]);
        }
    }
    it->texts_made = 1;
}

/* Shows title i in the Top Shelf carousel. */
void rail_focus(int i)
{
    S.rail_index = S.nrail <= 0 || i < 0 ? 0 : i >= S.nrail ? S.nrail - 1 : i;
}

/* The app whose Top Shelf is on show: the focused top-row app, or below the dock
 * the last one focused (its picture stays behind the grid). */
App *shelf_app(void) { return item_app(S.focus < COLS ? S.focus : S.shelf_cur); }

/* Whose carousel the Top Shelf wants: a media app's Continue Watching, or a
 * streaming app's popular titles. */
int want_source(void)
{
    App *a = shelf_app();
    int src = media_source(a);
    return src ? src : a && a->feat == 2 ? SRC_FEATURED : SRC_NONE;
}

/* the carousel holds app a's titles */
int rail_for(const App *a)
{
    if (!a || S.nrail <= 0) return 0;
    int src = media_source(a);
    if (src) return src == S.rail_source;
    return S.rail_source == SRC_FEATURED && !strcmp(S.rail_app, a->id);
}

/* asks the service whether app a is a streaming service, and what's popular on it */
void featured_request(App *a)
{
    char path[ID_LEN + 32];
    snprintf(path, sizeof path, "/api/featured?app=%s", a->id);
    if (!a->feat) a->feat = 1;
    S.feat_req = (int)(a - S.apps);
    net_get_json(path, TAG_FEATURED, S.feat_req, S.generation);
}

void rail_request(int src)
{
    char path[64];
    snprintf(path, sizeof path, "/api/continue?source=%s", SRC_ID[src]);
    S.rail_req = src;
    net_get_json(path, TAG_STREMIO, src, 0);
}

/* Empties the carousel (its source signed out), its pictures and texts with it: a picture
 * on show is held until something covers it, as when its title leaves. */
void rail_clear(void)
{
    for (int l = 0; l < 2; l++) {
        Pic *p = l ? &S.pic_top : &S.pic_under;
        if (p->kind == PIC_RAIL) pic_hold(p, p->i < S.nrail ? &S.rail[p->i].bg_tex : NULL);
    }
    for (int i = 0; i < S.nrail; i++) {
        RailItem *it = &S.rail[i];
        tex_free(&it->bg_tex); tex_free(&it->logo_tex); tex_free(&it->title_tex); tex_free(&it->meta_tex); tex_free(&it->resume_tex);
        for (int l = 0; l < OVERVIEW_LINES; l++) tex_free(&it->overview_tex[l]);
    }
    S.nrail = 0;
    net_drop(TAG_RAIL_BG, -1, S.rail_generation);
    net_drop(TAG_RAIL_LOGO, -1, S.rail_generation);
    S.rail_generation++; /* pictures on the way are for titles gone */
}

/* A setting change redraws only the text. Pictures and the current carousel stay in place. */
void shelf_set_details(int mode)
{
    if (mode == S.shelf_details) return;
    S.shelf_details = mode;
    for (int i = 0; i < S.nrail; i++) {
        RailItem *it = &S.rail[i];
        tex_free(&it->title_tex); tex_free(&it->meta_tex); tex_free(&it->resume_tex);
        for (int l = 0; l < OVERVIEW_LINES; l++) tex_free(&it->overview_tex[l]);
        it->texts_made = 0;
    }
}

/* Puts titles (a JSON array) in the Top Shelf carousel: source src's, and for
 * popular titles app's, under a heading. */
static void rail_fill(cJSON *arr, int src, const char *app, const char *heading, const char *opens)
{
    if (src != S.rail_source || strcmp(app, S.rail_app)) { /* another carousel: its heading, from its first title */
        char hint[160];
        tex_free(&S.heading_tex);
        S.heading_tex = text_render(S.f_semi, HEADING_PX, heading, 0, &S.hw, &S.hh); /* as big as the clock's */
        snprintf(hint, sizeof hint, "OK opens %s", opens);
        tex_free(&S.rail_hint);
        S.rail_hint = text_render(S.f_medium, HINT_PX, hint, 0, &S.rail_hint_w, &S.rail_hint_h);
        /* while you step through them, OK is on the title: Jellyfin's can be played */
        if (src == SRC_JELLYFIN) snprintf(hint, sizeof hint, "Left and right for more  \xC2\xB7  OK plays  \xC2\xB7  Hold OK for options");
        else snprintf(hint, sizeof hint, "Left and right for more  \xC2\xB7  OK opens %s", opens);
        tex_free(&S.rail_browse);
        S.rail_browse = text_render(S.f_medium, HINT_PX, hint, 0, &S.rail_browse_w, &S.rail_browse_h);
        S.rail_source = src;
        snprintf(S.rail_app, sizeof S.rail_app, "%s", app);
        S.rail_sig[0] = 0;
        S.rail_index = 0;
        S.rail_next = 0;
    }
    cJSON *v;
    char sig[2048] = "";
    cJSON_ArrayForEach(v, arr) {
        unsigned hash = 2166136261u;
        for (const unsigned char *p = (const unsigned char *)jstr(v, "name"); *p; p++) hash = (hash ^ *p) * 16777619u;
        hash = (hash ^ 255u) * 16777619u;
        for (const unsigned char *p = (const unsigned char *)jstr(v, "genre"); *p; p++) hash = (hash ^ *p) * 16777619u;
        hash = (hash ^ 255u) * 16777619u;
        for (const unsigned char *p = (const unsigned char *)jstr(v, "overview"); *p; p++) hash = (hash ^ *p) * 16777619u;
        char part[120];
        snprintf(part, sizeof part, "%s:%.3f:%.1f:%d:%d:%d:%d:%08x,", jstr(v, "id"), jnum(v, "progress"), jnum(v, "rating"),
                 (int)jnum(v, "season"), (int)jnum(v, "episode"), (int)jnum(v, "remainingMin"), (int)jnum(v, "year"), hash);
        append(sig, sizeof sig, part);
    }
    if (!strcmp(sig, S.rail_sig) && S.nrail) return;
    int same = S.rail_sig[0] != 0; /* a refresh of the same carousel: stay on the same title */
    snprintf(S.rail_sig, sizeof S.rail_sig, "%s", sig);

    static RailItem old[MAX_RAIL]; /* its pictures and texts move over to the titles that stay */
    int nold = S.nrail;
    char keep[64] = "";
    if (same && S.rail_index < nold) snprintf(keep, sizeof keep, "%s", S.rail[S.rail_index].id);
    memcpy(old, S.rail, sizeof old);
    memset(S.rail, 0, sizeof S.rail);
    S.nrail = 0;
    net_drop(TAG_RAIL_BG, -1, S.rail_generation); /* pictures not yet on the way are for the old order */
    net_drop(TAG_RAIL_LOGO, -1, S.rail_generation);
    S.rail_generation++; /* and the ones on the way are ignored */
    cJSON_ArrayForEach(v, arr) {
        if (S.nrail >= MAX_RAIL) break;
        RailItem *it = &S.rail[S.nrail++];
        snprintf(it->id, sizeof it->id, "%s", jstr(v, "id"));
        snprintf(it->type, sizeof it->type, "%s", jstr(v, "type"));
        snprintf(it->name, sizeof it->name, "%s", jstr(v, "name"));
        snprintf(it->genre, sizeof it->genre, "%s", jstr(v, "genre"));
        snprintf(it->overview, sizeof it->overview, "%s", jstr(v, "overview"));
        const char *bg = jstr(v, "background");
        snprintf(it->bg, sizeof it->bg, "%s", bg[0] ? bg : jstr(v, "thumbnail"));
        snprintf(it->logo, sizeof it->logo, "%s", jstr(v, "logo"));
        it->progress = (float)jnum(v, "progress");
        it->rating = (float)jnum(v, "rating");
        it->season = (int)jnum(v, "season");
        it->episode = (int)jnum(v, "episode");
        it->remaining = (int)jnum(v, "remainingMin");
        it->year = (int)jnum(v, "year");
    }
    int at = S.rail_index, moved[MAX_RAIL], pic_to[MAX_RAIL]; /* where each old title went, and its picture */
    for (int k = 0; k < nold; k++) moved[k] = pic_to[k] = -1;
    for (int i = 0; i < S.nrail; i++) {
        RailItem *it = &S.rail[i];
        for (int k = 0; k < nold; k++) {
            RailItem *o = &old[k];
            if (strcmp(o->id, it->id)) continue;
            moved[k] = i;
            if (strcmp(o->bg, it->bg) || strcmp(o->logo, it->logo)) break;
            if (o->bg_state == 2) it->bg_tex = o->bg_tex, it->bg_state = 2, o->bg_tex = NULL, pic_to[k] = i;
            if (o->logo_state == 2) it->logo_tex = o->logo_tex, it->logo_state = 2, it->lw = o->lw, it->lh = o->lh, o->logo_tex = NULL;
            if (it->progress == o->progress && it->remaining == o->remaining && it->rating == o->rating &&
                it->season == o->season && it->episode == o->episode &&
                it->year == o->year && !strcmp(it->name, o->name) && !strcmp(it->type, o->type) &&
                !strcmp(it->genre, o->genre) && !strcmp(it->overview, o->overview)) {
                it->title_tex = o->title_tex, it->tw = o->tw, it->th = o->th, o->title_tex = NULL;
                it->meta_tex = o->meta_tex, it->mw = o->mw, it->mh = o->mh, o->meta_tex = NULL;
                it->resume_tex = o->resume_tex, it->rw = o->rw, it->rh = o->rh, o->resume_tex = NULL;
                for (int l = 0; l < OVERVIEW_LINES; l++) {
                    it->overview_tex[l] = o->overview_tex[l]; o->overview_tex[l] = NULL;
                    it->ow[l] = o->ow[l]; it->oh[l] = o->oh[l];
                }
                it->texts_made = o->texts_made;
            }
            break;
        }
        if (keep[0] && !strcmp(it->id, keep)) at = i;
    }
    /* what's on show stays: the pages follow their titles (-2: it left), and a picture
     * whose title left is held until something covers it */
    for (int l = 0; l < 2; l++) {
        Pic *p = l ? &S.pic_top : &S.pic_under;
        if (p->kind != PIC_RAIL) continue;
        if (p->i < nold && pic_to[p->i] >= 0) p->i = pic_to[p->i];
        else pic_hold(p, p->i < nold ? &old[p->i].bg_tex : NULL);
    }
    if (S.page_rail >= 0) S.page_rail = S.page_rail < nold && moved[S.page_rail] >= 0 ? moved[S.page_rail] : -2;
    if (S.prev_rail >= 0) S.prev_rail = S.prev_rail < nold && moved[S.prev_rail] >= 0 ? moved[S.prev_rail] : -2;
    for (int k = 0; k < nold; k++) {
        RailItem *o = &old[k];
        tex_free(&o->bg_tex); tex_free(&o->logo_tex); tex_free(&o->title_tex); tex_free(&o->meta_tex); tex_free(&o->resume_tex);
        for (int l = 0; l < OVERVIEW_LINES; l++) tex_free(&o->overview_tex[l]);
    }
    rail_focus(at < S.nrail ? at : S.nrail - 1);
    if (!S.nrail && S.zone == ZONE_RAIL) S.zone = ZONE_GRID;
}

void parse_continue(const char *json, int src)
{
    cJSON *o = cJSON_Parse(json);
    if (src == S.rail_req) S.rail_req = SRC_NONE;
    if (!o) return;
    int connected = cJSON_IsTrue(cJSON_GetObjectItem(o, "connected"));
    S.src_connected[src] = connected;
    if (src == SRC_STREMIO) { /* this answer also carries every account's state, for Settings */
        S.rail_connected = connected;
        S.rail_stremio = cJSON_IsTrue(cJSON_GetObjectItem(o, "stremio"));
        cJSON *tr = cJSON_GetObjectItem(o, "trakt");
        S.trakt_configured = cJSON_IsTrue(cJSON_GetObjectItem(tr, "configured"));
        S.trakt_connected = cJSON_IsTrue(cJSON_GetObjectItem(tr, "connected"));
        S.src_connected[SRC_JELLYFIN] = cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(o, "jellyfin"), "connected"));
        S.src_connected[SRC_PLEX] = cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(o, "plex"), "connected"));
        int tmdb = cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(o, "tmdb"), "configured"));
        if (tmdb && !S.tmdb_configured) /* just set up: ask again about apps that had nothing */
            for (int i = 0; i < S.napps; i++) if (S.apps[i].feat == 3) S.apps[i].feat = 0;
        S.tmdb_configured = tmdb;
    }
    if (src != want_source()) { /* another app's carousel: not on show */
        cJSON_Delete(o);
        return;
    }
    char heading[96];
    if (src == SRC_STREMIO)
        snprintf(heading, sizeof heading, "Continue Watching%s%s", S.rail_stremio ? " \xC2\xB7 Stremio" : "",
                 S.trakt_connected ? " \xC2\xB7 Trakt" : "");
    else
        snprintf(heading, sizeof heading, "Continue Watching \xC2\xB7 %s", SRC_NAME[src]);
    rail_fill(cJSON_GetObjectItem(o, "items"), src, "", heading, SRC_NAME[src]);
    cJSON_Delete(o);
}

/* An app's titles from TMDB, for its Top Shelf: what's popular on the streaming
 * service it is, or what's trending (an app with no picture); none: neither. */
void parse_featured(const char *json, App *a)
{
    cJSON *o = cJSON_Parse(json);
    cJSON *arr = o ? cJSON_GetObjectItem(o, "items") : NULL;
    int n = cJSON_GetArraySize(arr), trending = o && !strcmp(jstr(o, "kind"), "trending");
    a->feat = n > 0 ? 2 : 3;
    if (n > 0 && a == shelf_app()) {
        char heading[ID_LEN + 16], name[ID_LEN];
        short_title(a, name, sizeof name);
        if (trending) snprintf(heading, sizeof heading, "Trending Now");
        else snprintf(heading, sizeof heading, "Popular on %s", name);
        rail_fill(arr, SRC_FEATURED, a->id, heading, name);
        S.rail_trending = trending;
    }
    cJSON_Delete(o);
}

/* ------------------------------------------------------------------ actions */

/* the focused top-row app's carousel is in the Top Shelf */
int rail_active(void)
{
    return S.focus < COLS && rail_for(item_app(S.focus));
}

/* app a's carousel is on the way: its Continue Watching, or what's popular on it */
int carousel_coming(const App *a)
{
    int src = media_source(a);
    if (src) return S.rail_req == src;
    return S.tmdb_configured && S.service_ok && (a->feat <= 1 || (a->feat == 2 && !rail_for(a)));
}

/* Whether the Top Shelf can show the page (item, rail) yet: a title once its picture and
 * logo are in (or failed, or it has none); an app's own page once no carousel is on the
 * way for it and its artwork isn't loading. */
static int page_ready(int item, int rail)
{
    App *a = item_app(item);
    if (!a) return 1;
    if (rail >= 0) return rail_ready(rail);
    return !carousel_coming(a) && !(a->art[0] && a->art_state < 2);
}

/* carousel title i's picture and logo are in (or failed, or it has none): it can be shown */
int rail_ready(int i)
{
    const RailItem *r = &S.rail[i];
    return (r->bg_state >= 2 || !r->bg[0]) && (r->logo_state >= 2 || !r->logo[0]);
}

/* a page's picture: its title's, else its app's artwork, else the app's glow (with its
 * tile large on its own page; the glow alone behind a title). Once another carousel has
 * taken its place, the picture on show stays. */
static Pic page_pic(int item, int rail)
{
    App *a = item_app(item);
    int mine = a && rail_for(a);
    if (rail == -2 || (rail >= 0 && !mine)) return S.pic_top;
    if (rail >= 0 && rail < S.nrail && S.rail[rail].bg_tex) return (Pic){ PIC_RAIL, rail };
    if (!a) return (Pic){ PIC_NONE, 0 };
    return (Pic){ a->art_tex ? PIC_ART : rail == -1 ? PIC_GLOW : PIC_COVER, (int)(a - S.apps) };
}

/* What the Top Shelf needs from the service: the carousel of the app it's for, and its pictures. On screen,
 * the selected title's first, then both of its neighbours, loaded ahead so turning or stepping through never
 * waits; and those still on show. The rest are let go (~8 MB each on this GPU), with their requests, waiting
 * or on their way. While hidden, only the page it comes back to (go_offscreen), so that's in when it's back:
 * its neighbours go too. */
void shelf_load(void)
{
    int want = want_source();
    if (want && want != SRC_FEATURED && want != S.rail_source && want != S.rail_req && S.service_ok) rail_request(want);
    App *sa = shelf_app();
    if (sa && !media_source(sa) && S.tmdb_configured && S.service_ok && S.feat_req < 0 &&
        (sa->feat == 0 || (sa->feat == 2 && !rail_for(sa))))
        featured_request(sa); /* a streaming service? (the service keeps its list) */
    if (rail_active()) {
        int nx = (S.rail_index + 1) % S.nrail, pv = (S.rail_index + S.nrail - 1) % S.nrail;
        int order[3] = { S.rail_index, nx, pv }, n = S.offscreen ? 1 : 3;
        RailItem *sel = &S.rail[S.rail_index];
        if (sel->logo_state == 1) net_bump(TAG_RAIL_LOGO, S.rail_index, S.rail_generation); /* asked for as a neighbour */
        if (sel->bg_state == 1) net_bump(TAG_RAIL_BG, S.rail_index, S.rail_generation);
        for (int k = 0; k < n; k++) {
            RailItem *it = &S.rail[order[k]];
            int prio = k ? PRIO_SOON : PRIO_NOW; /* (the selected title's, then its neighbours') */
            if (it->bg[0] && it->bg_state == 0) {
                it->bg_state = 1;
                net_get_image(it->bg, TAG_RAIL_BG, order[k], S.rail_generation, 0, W, H, prio);
            }
            if (it->logo[0] && it->logo_state == 0) {
                it->logo_state = 1;
                net_get_image(it->logo, TAG_RAIL_LOGO, order[k], S.rail_generation, 0, 640, 240, prio);
            }
        }
        for (int i = 0; i < S.nrail; i++) {
            int near = !S.offscreen && (i == nx || i == pv);
            if (i == S.rail_index || near || i == S.page_rail || i == S.prev_rail || pic_rail(i)) continue;
            RailItem *it = &S.rail[i];
            if (it->bg_state == 1 && net_drop(TAG_RAIL_BG, i, S.rail_generation)) it->bg_state = 0;
            if (it->logo_state == 1 && net_drop(TAG_RAIL_LOGO, i, S.rail_generation)) it->logo_state = 0;
            if (it->bg_tex) tex_free(&it->bg_tex), it->bg_state = 0;
            if (it->logo_tex) tex_free(&it->logo_tex), it->logo_state = 0;
            tex_free(&it->title_tex);
            tex_free(&it->meta_tex);
            tex_free(&it->resume_tex);
            for (int l = 0; l < OVERVIEW_LINES; l++) tex_free(&it->overview_tex[l]);
            it->texts_made = 0;
        }
    } else {
        App *ca = item_app(S.shelf_cur);
        if (ca && ca->art[0] && ca->art_state == 0 && !rail_for(ca) && !carousel_coming(ca)) { /* its own page */
            ca->art_state = 1;
            net_get_image(ca->art, TAG_ART, S.items[S.shelf_cur], S.generation, 0, W, H, PRIO_NOW);
        }
    }
}

/* Back from another app (ready_home()): the Top Shelf shows the page it wants at once, without the wait or
 * the fade, once that page is ready; the page it left lets go of its picture. 1: it's on show. */
int shelf_jump(void)
{
    App *a = item_app(S.shelf_cur);
    int wi = S.shelf_cur, wr = a && rail_for(a) ? S.rail_index : -1;
    if (!page_ready(wi, wr)) return 0;
    Pic want = page_pic(wi, wr);
    if (!pic_eq(want, S.pic_top)) pic_drop(&S.pic_top);
    pic_drop(&S.pic_under);
    S.pic_top = want, S.pic_a = 1;
    S.want_app = S.page_app = wi, S.want_rail = S.page_rail = wr, S.want_at = SDL_GetTicks();
    S.prev_app = -1, S.prev_rail = -2, S.prev_vis = 0;
    S.page_step = 0, S.page_a = 1, S.page_leave = S.skel_a = 0;
    return 1;
}

/* 1: the Top Shelf shows the page it wants, with its own picture: nothing stands in for it; 2: and all of
 * that picture is in (perf.c, which times a return to the front) */
int shelf_complete(void)
{
    if (S.page_app != S.want_app || S.page_rail != S.want_rail || !page_ready(S.want_app, S.want_rail) || S.skel_a > 0.004f) return 0;
    return S.pic_a >= 1 && S.page_a > 0.996f && S.page_leave < 0.004f ? 2 : 1;
}

/* The Top Shelf's next page shows once it's ready, or after PAGE_WAIT_MS; meanwhile the
 * page on show stays and its text leaves. Its picture fades in over the one on show. */
void update_page(float dt, int *anim)
{
    App *a = item_app(S.shelf_cur);
    int wi = S.shelf_cur, wr = a && rail_for(a) ? S.rail_index : -1;
    Uint32 now = SDL_GetTicks();
    /* the wait starts over for another app, or another title (stepping through them),
     * not when the app's carousel comes in */
    if (wi != S.want_app || (wr >= 0 && S.want_rail >= 0 && wr != S.want_rail)) S.want_at = now;
    S.want_app = wi, S.want_rail = wr;
    int on = wi == S.page_app && wr == S.page_rail;
    if (!on && (page_ready(wi, wr) || now - S.want_at >= PAGE_WAIT_MS)) {
        S.page_step = wr >= 0 && S.page_rail >= 0 && wi == S.page_app; /* the next title, same carousel */
        S.prev_app = S.page_app, S.prev_rail = S.page_rail, S.prev_vis = 1 - S.page_leave;
        S.page_app = wi, S.page_rail = wr;
        S.page_a = S.page_leave = 0;
        on = 1;
    }
    approach(&S.page_leave, on ? 0 : 1, 10, dt, anim);
    approach(&S.page_a, 1, 9, dt, anim);
    /* a carousel's page that takes a while: placeholders stand in for its text */
    int slow = !on && now - S.want_at >= SKEL_MS && (wr >= 0 || (a && carousel_coming(a)));
    approach(&S.skel_a, slow ? 1 : 0, 8, dt, anim);
    if (S.skel_a > 0.004f && !S.reduce_motion) *anim = 1; /* their shimmer */
    Pic want = page_pic(S.page_app, S.page_rail);
    if (!pic_eq(want, S.pic_top)) {
        if (S.pic_a >= 0.5f) { /* mostly in: it's the one underneath now */
            pic_drop(&S.pic_under);
            S.pic_under = S.pic_top;
        } else {
            pic_drop(&S.pic_top);
        }
        S.pic_top = want;
        S.pic_a = 0;
    }
    approach(&S.pic_a, 1, 5, dt, anim);
    if (S.pic_a >= 1) pic_drop(&S.pic_under); /* covered */
}

/* The Top Shelf is a picture (artwork, or the app's tile large on a glow, under
 * scrims) and content (titles, the Continue Watching rail). The picture stays
 * behind the grid when you scroll down, blurred; the content scrolls away. */

/* Apps without artwork also get their tile, large, on the right. */
static void draw_app_bigtile(App *a, Uint8 alpha, int backdrop)
{
    if (!a || !alpha || !a->logo_tex) return;
    /* it leaves with the shelf's content, so it never ghosts behind the grid */
    float leave = fmaxf(0, 1 - 2.5f * S.scrolled);
    if (leave <= 0) return;
    /* it steps aside for the Control Center: gone before the panel, coming in from the right, is over it, and back
     * only once the panel is mostly out again (they'd show through each other) */
    float tw = 520, th = tw * TILE_H / TILE_W, cc = 1 - smooth01(0, 0.5f, S.cc.t);
    if (cc <= 0.004f) return;
    draw_big_tile(a, RIGHT - tw, SAFE_Y + 120, tw, th, TILE_R * tw / TILE_W, (Uint8)(alpha * leave * cc), 1, backdrop, 0);
}

/* Lift text from light artwork without putting a panel behind it. */
static void draw_shelf_text(Tex *t, int w, int h, float x, float y, Uint8 alpha)
{
    if (!t || !alpha) return;
    r_image(t, x + 1, y + 2, (float)w, (float)h, 0, 0, 0, (Uint8)(alpha * 0.75f), 0);
    draw_text(t, w, h, x, y, alpha);
}

static void draw_app_text(App *a, Uint8 alpha, float dy)
{
    if (!a || !alpha) return;
    ensure_big(a);
    draw_shelf_text(a->big, a->bw, a->bh, LEFT - 4, 340 + dy, alpha);
    int first = a == item_app(0);
    Tex *sub = first ? S.sub_first_tex : S.sub_tex;
    int sw = first ? S.sfw : S.sw, sh = first ? S.sfh : S.sh, src = media_source(a);
    if (src && !S.src_connected[src]) { /* a media app: sign in from Settings */
        if (!S.connect_tex[src]) {
            char t[120];
            snprintf(t, sizeof t, "Connect %s in Settings \xE2\x86\x92 Accounts to see Continue Watching", SRC_NAME[src]);
            S.connect_tex[src] = text_render(S.f_medium, HINT_PX, t, 0, &S.connect_w[src], &S.connect_h[src]);
        }
        sub = S.connect_tex[src], sw = S.connect_w[src], sh = S.connect_h[src];
    }
    if (S.zone == ZONE_TOP) { /* while the top bar has focus, the hint is about it */
        sub = S.top_hint[S.top_index];
        sw = S.top_hw[S.top_index];
        sh = S.top_hh[S.top_index];
    }
    if (S.moving) return; /* moving it: the capsule at the top says what the keys do */
    draw_shelf_text(sub, sw, sh, LEFT, 340 + a->bh + 10 + dy,
                    (Uint8)(SHELF_DETAIL_ALPHA * alpha / 255 * (1 - fminf(fmaxf(S.cc.t, 0), 1)))); /* the Control Center says what OK does */
}

static void draw_pic(Pic p, Uint8 alpha)
{
    if (!alpha) return;
    App *a = p.kind == PIC_ART || p.kind == PIC_GLOW || p.kind == PIC_COVER ? &S.apps[p.i] : NULL;
    Uint8 image_alpha = (Uint8)(alpha * BACKDROP_IMAGE_OPACITY);
    if (p.kind == PIC_ART) r_cover(a->art_tex, 0, 0, W, H, image_alpha, 0);
    else if (a && a->cover_tex) r_cover(a->cover_tex, 0, 0, W, H, image_alpha, 0);
    else if (a) {
        Uint8 r, g, b;
        app_color(a, &r, &g, &b);
        r_shape(0, 0, W, H, 0, r, g, b, (Uint8)(alpha * 0.25f), 0, 1, 1);
    } else if (p.kind == PIC_RAIL && p.i < S.nrail) r_cover(S.rail[p.i].bg_tex, 0, 0, W, H, image_alpha, 0);
    else if (p.kind == PIC_HELD) r_cover(S.pic_held[p.i], 0, 0, W, H, image_alpha, 0);
}

/* A picture, rather than an app's plain colour glow. */
static int pic_has_image(Pic p)
{
    return p.kind == PIC_ART || p.kind == PIC_RAIL || p.kind == PIC_HELD || ((p.kind == PIC_GLOW || p.kind == PIC_COVER) && S.apps[p.i].cover_tex);
}

/* The carousel's title, like tvOS's Top Shelf: a heading, the title's logo (or its
 * name), what it is, how far you are, where the app opens, and dots for where it
 * is among the others (brighter while you step through them). title: how far the
 * title itself is in (0..1), apart from the rest; skel: placeholders where the next
 * one goes, while it loads. */
static void draw_rail_content(RailItem *cur, Uint8 alpha, float title, float skel, int backdrop)
{
    if (!cur || !alpha) return;
    ensure_rail_texts(cur);
    /* the heading mirrors the clock: same height and top, on the dock's left edge as the
     * clock is on its right; the title sits low on the left, just above the chevron, like
     * the Apple TV app, so the picture's top stays clear */
    panel(backdrop, DOCK_X, TOP_Y, S.hw + 48, BAR, BAR / 2, 1, 0, alpha / 255.0f);
    draw_text(S.heading_tex, S.hw, S.hh, DOCK_X + 24, TOP_Y + (BAR - S.hh) / 2.0f, alpha);
    int logo = cur->logo_tex && cur->lw > 0 && cur->lh > 0, n = S.nrail < MAX_DOTS ? S.nrail : MAX_DOTS;
    float k = logo ? fminf(fminf(560.0f / cur->lw, 190.0f / cur->lh), 1.6f) : 0;
    float title_h = logo ? cur->lh * k + 18 : cur->th + 8;
    int enriched = rail_enriched(cur);
    int full = enriched && S.shelf_details == SHELF_DETAILS_FULL && cur->overview_tex[0];
    float detail_h = S.rail_hint_h + (enriched && S.rail_source == SRC_STREMIO && cur->resume_tex ? cur->rh + 14 : 0);
    float total = title_h + cur->mh + 14 + (cur->progress > 0 ? 21 : 0) + detail_h + (n > 1 ? 32 : 0);
    Uint8 ta = (Uint8)(alpha * title);
    float y = full ? FEATURED_TITLE_Y : DOCK_Y - 64 - total;
    if (skel > 0.004f) { /* placeholders follow the fixed featured anchors */
        float title_skel_y = full ? FEATURED_TITLE_Y : y + title_h - 82;
        float meta_skel_y = full ? FEATURED_META_Y : y + title_h;
        draw_skeleton(LEFT, title_skel_y, 400, 64, 14, alpha / 255.0f * skel);
        draw_skeleton(LEFT, meta_skel_y + (cur->mh - 20) / 2, 250, 20, 10, alpha / 255.0f * skel);
    }
    if (logo) {
        float lw = cur->lw * k, lh = cur->lh * k;
        r_image(cur->logo_tex, LEFT, y, lw, lh, 255, 255, 255, ta, 0);
        y += lh + 18;
    } else {
        draw_shelf_text(cur->title_tex, cur->tw, cur->th, LEFT - 3, y, ta);
        y += cur->th + 8;
    }
    if (full) y = FEATURED_META_Y;
    draw_shelf_text(cur->meta_tex, cur->mw, cur->mh, LEFT, y, (Uint8)(SHELF_DETAIL_ALPHA * ta / 255));
    y += cur->mh + 14;
    if (enriched && !full && S.rail_source == SRC_STREMIO && cur->resume_tex) {
        draw_shelf_text(cur->resume_tex, cur->rw, cur->rh, LEFT, y, (Uint8)(SHELF_DETAIL_ALPHA * ta / 255));
        y += cur->rh + 14;
    }
    if (cur->progress > 0 && !full) {
        const float bw = 240, bh = 5;
        r_shape(LEFT, y, bw, bh, bh / 2, 255, 255, 255, (Uint8)(ta * 0.28f), 0, 1, 1);
        r_shape(LEFT, y, bw * cur->progress, bh, bh / 2, 255, 255, 255, ta, 0, 1, 1);
        y += bh + 16;
    }
    float cc = 1 - fminf(fmaxf(S.cc.t, 0), 1); /* the Control Center, open, says what OK does */
    if (full) y = FEATURED_OVERVIEW_Y;
    float resume_y = CONTINUE_RESUME_Y;
    if (full) {
        float line_y = y;
        for (int l = 0; l < OVERVIEW_LINES; l++) if (cur->overview_tex[l]) {
            draw_shelf_text(cur->overview_tex[l], cur->ow[l], cur->oh[l], LEFT, line_y, (Uint8)(SHELF_DETAIL_ALPHA * ta / 255));
            line_y += cur->oh[l] + 3;
        }
        resume_y = fmaxf(resume_y, line_y + 5);
        if (S.zone == ZONE_RAIL && S.rail_browse) {
            float limit = S.rail_source == SRC_STREMIO ? resume_y : DOCK_Y - 64 - (n > 1 ? 32 : 0) + 22;
            float hint_y = line_y + 7;
            if (hint_y + S.rail_browse_h + 8 > limit) hint_y = TOP_Y + BAR + 22;
            draw_shelf_text(S.rail_browse, S.rail_browse_w, S.rail_browse_h, LEFT, hint_y,
                            (Uint8)(SHELF_DETAIL_ALPHA * alpha / 255));
        }
    } else {
        if (S.zone == ZONE_RAIL && S.rail_browse)
            draw_shelf_text(S.rail_browse, S.rail_browse_w, S.rail_browse_h, LEFT, y, (Uint8)(SHELF_DETAIL_ALPHA * alpha / 255));
        else draw_shelf_text(S.rail_hint, S.rail_hint_w, S.rail_hint_h, LEFT, y, (Uint8)(SHELF_DETAIL_ALPHA * alpha / 255 * cc)); /* (from the tile: OK opens the app) */
    }
    if (full && S.rail_source == SRC_STREMIO && cur->resume_tex)
        draw_shelf_text(cur->resume_tex, cur->rw, cur->rh, LEFT, resume_y, (Uint8)(SHELF_DETAIL_ALPHA * ta / 255));
    if (full && S.rail_source == SRC_STREMIO && cur->progress > 0) {
        const float bw = 240, bh = 5;
        r_shape(LEFT, CONTINUE_PROGRESS_Y, bw, bh, bh / 2, 255, 255, 255, (Uint8)(ta * 0.28f), 0, 1, 1);
        r_shape(LEFT, CONTINUE_PROGRESS_Y, bw * cur->progress, bh, bh / 2, 255, 255, 255, ta, 0, 1, 1);
    }
    y = full && S.rail_source == SRC_STREMIO ? CONTINUE_DOTS_Y :
        full ? DOCK_Y - 64 - (n > 1 ? 32 : 0) + 22 : y + S.rail_hint_h + 22;
    int browsing = S.zone == ZONE_RAIL;
    for (int i = 0; n > 1 && i < n; i++) {
        int on = i == (S.rail_index < n ? S.rail_index : n - 1);
        float d = on ? 10 : 8, dx = LEFT + i * 20 + (10 - d) / 2;
        r_shape(dx, y + (10 - d) / 2, d, d, d / 2, 255, 255, 255, (Uint8)(alpha * (on ? 0.95f : browsing ? 0.5f : 0.3f)), 0, 1, 1);
    }
}

/* Pictures sit at 80% opacity on black. During a crossfade, their combined opacity
 * stays at 80% too, so the old picture disappears without a dark dip or a ghost. */
void draw_shelf_art(Uint8 alpha, int backdrop)
{
    if (!alpha) return;
    float t = S.pic_a;
    r_fill(0, 0, 0, alpha);
    if (t < 1) {
        float under = 1 - t;
        if (pic_has_image(S.pic_under) && pic_has_image(S.pic_top))
            under /= 1 - BACKDROP_IMAGE_OPACITY * t;
        draw_pic(S.pic_under, (Uint8)(alpha * under));
    }
    draw_pic(S.pic_top, (Uint8)(alpha * t));
    draw_scrims(alpha);
    if (t < 1 && S.pic_under.kind == PIC_GLOW) draw_app_bigtile(&S.apps[S.pic_under.i], (Uint8)(alpha * (1 - smooth01(0, 0.5f, t))), backdrop);
    if (S.pic_top.kind == PIC_GLOW) draw_app_bigtile(&S.apps[S.pic_top.i], (Uint8)(alpha * smooth01(0.3f, 1, t)), backdrop);
}

/* a page's text: its app's name, or its carousel's title (-2: none) */
static void draw_page(int item, int rail, Uint8 alpha, float title, float skel, float dy, int backdrop)
{
    App *a = item_app(item);
    if (!a || !alpha) return;
    if (rail == -1) draw_app_text(a, alpha, dy);
    else if (rail >= 0 && rail < S.nrail && rail_for(a)) draw_rail_content(&S.rail[rail], alpha, title, skel, backdrop);
}

/* The Top Shelf while a carousel loads: placeholders where its heading and title go. */
static void draw_shelf_skeleton(float a, int backdrop)
{
    panel(backdrop, DOCK_X, TOP_Y, 300, BAR, BAR / 2, 1, 0, a);
    draw_skeleton(DOCK_X + 24, TOP_Y + (BAR - 14) / 2.0f, 220, 14, 7, a);
    float y = DOCK_Y - 64 - 170;
    draw_skeleton(LEFT, y, 400, 64, 14, a);
    draw_skeleton(LEFT, y + 84, 250, 20, 10, a);
    draw_skeleton(LEFT, y + 122, 330, 18, 9, a);
}

/* Stepping through a carousel, only the title changes, coming in with its picture.
 * Otherwise one page at a time: the old one leaves quickly, then the new one rises in.
 * While the next page loads, the text on show leaves (stepping through: the title). */
void draw_shelf_content(Uint8 alpha, int backdrop)
{
    if (!alpha) return;
    int stepping = S.want_app == S.page_app && S.want_rail >= 0 && S.page_rail >= 0;
    float stay = 1 - S.page_leave;
    float skel = S.skel_a; /* the next page is taking a while */
    if (skel > 0.004f && !stepping) draw_shelf_skeleton(alpha / 255.0f * skel, backdrop);
    if (S.page_step) {
        draw_page(S.page_app, S.page_rail, stepping ? alpha : (Uint8)(alpha * stay),
                  fminf(1, 0.25f + S.page_a) * (stepping ? stay : 1), stepping ? skel : 0, 0, backdrop);
        return;
    }
    float t = S.page_a, in = smooth01(0.35f, 1, t);
    if (t < 0.4f && S.prev_vis > 0)
        draw_page(S.prev_app, S.prev_rail, (Uint8)(alpha * S.prev_vis * (1 - smooth01(0, 0.4f, t))), 1, 0, 0, backdrop);
    draw_page(S.page_app, S.page_rail, (Uint8)(alpha * in * (stepping ? 1 : stay)), stepping ? stay : 1, stepping ? skel : 0,
              S.reduce_motion ? 0 : 14 * (1 - in), backdrop);
}
