/* Home+ native: the apps: the list from the service, your layout (order, hidden), launching and closing them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "home.h"

/* ------------------------------------------------------------------ items & layout */

void rebuild_items(void)
{
    int used[MAX_APPS] = { 0 };
    S.nitems = 0;
    for (int o = 0; o < S.norder; o++) {
        for (int i = 0; i < S.napps; i++) {
            if (!used[i] && !strcmp(S.apps[i].id, S.order[o])) {
                used[i] = 1;
                if (!is_hidden(S.apps[i].id) && !S.apps[i].deleted) S.items[S.nitems++] = i;
            }
        }
    }
    for (int i = 0; i < S.napps; i++) if (!used[i] && !is_hidden(S.apps[i].id) && !S.apps[i].deleted) S.items[S.nitems++] = i;
    int any_hidden = 0;
    for (int i = 0; i < S.napps; i++) if (is_hidden(S.apps[i].id)) any_hidden = 1;
    if (any_hidden) S.items[S.nitems++] = -1;
    if (S.focus >= S.nitems) S.focus = S.nitems > 0 ? S.nitems - 1 : 0;
}

void save_layout(void)
{
    /* order = what's on screen, then hidden apps (their position doesn't matter) */
    S.norder = 0;
    for (int i = 0; i < S.nitems; i++) if (S.items[i] >= 0) copy_id(S.order[S.norder++], S.apps[S.items[i]].id);
    for (int i = 0; i < S.napps; i++) if (is_hidden(S.apps[i].id)) copy_id(S.order[S.norder++], S.apps[i].id);

    cJSON *o = cJSON_CreateObject(), *ord = cJSON_AddArrayToObject(o, "order"), *hid = cJSON_AddArrayToObject(o, "hidden");
    for (int i = 0; i < S.norder; i++) cJSON_AddItemToArray(ord, cJSON_CreateString(S.order[i]));
    for (int i = 0; i < S.nhidden; i++) cJSON_AddItemToArray(hid, cJSON_CreateString(S.hidden[i]));
    char *body = cJSON_PrintUnformatted(o);
    net_post("/api/layout", body, 0);
    free(body);
    cJSON_Delete(o);
}

void parse_layout(const char *json)
{
    cJSON *o = cJSON_Parse(json);
    if (!o) return;
    cJSON *v;
    S.norder = S.nhidden = 0;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(o, "order"))
        if (cJSON_IsString(v) && S.norder < MAX_APPS) snprintf(S.order[S.norder++], ID_LEN, "%s", v->valuestring);
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(o, "hidden"))
        if (cJSON_IsString(v) && S.nhidden < MAX_APPS) snprintf(S.hidden[S.nhidden++], ID_LEN, "%s", v->valuestring);
    cJSON_Delete(o);
    S.layout_loaded = 1;
    if (S.napps) { /* the apps arrived first (the requests race): put them in this order now */
        rebuild_items();
        S.shelf_cur = S.page_app = S.prev_app = -1;
    }
}

static void free_app(App *a)
{
    tex_free(&a->logo_tex);
    tex_free(&a->cover_tex);
    tex_free(&a->icon_tex);
    tex_free(&a->art_tex);
    tex_free(&a->label);
    tex_free(&a->big);
}

/* 0: no apps in it (not JSON, or none: webOS always has some), an answer as good as none */
int parse_apps(const char *json)
{
    cJSON *o = cJSON_Parse(json);
    cJSON *arr = cJSON_GetObjectItem(o, "apps"), *v;
    if (!cJSON_GetArraySize(arr)) {
        cJSON_Delete(o);
        return 0;
    }

    /* skip the rebuild when nothing changed: keeps textures and focus */
    char sig[4096] = "";
    cJSON_ArrayForEach(v, arr) {
        append(sig, sizeof sig, jstr(v, "id"));
        append(sig, sizeof sig, ",");
    }
    if (S.napps && !strcmp(sig, S.apps_sig)) {
        cJSON_Delete(o);
        return 1;
    }
    snprintf(S.apps_sig, sizeof S.apps_sig, "%s", sig);

    char keep[ID_LEN] = "";
    App *fa = item_app(S.focus);
    if (fa) snprintf(keep, sizeof keep, "%s", fa->id);

    /* the Top Shelf's picture stays until one from the new list covers it */
    for (int l = 0; l < 2; l++) {
        Pic *p = l ? &S.pic_top : &S.pic_under;
        if (p->kind == PIC_ART) pic_hold(p, &S.apps[p->i].art_tex);
        else if (p->kind == PIC_GLOW || p->kind == PIC_COVER) pic_hold(p, &S.apps[p->i].cover_tex);
    }
    S.page_app = S.prev_app = -1;
    for (int i = 0; i < S.napps; i++) free_app(&S.apps[i]);
    memset(S.apps, 0, sizeof S.apps);
    S.napps = 0;
    net_drop(TAG_ICON, -1, S.generation); /* the old list's pictures, waiting or on their way: their apps are gone */
    net_drop(TAG_ART, -1, S.generation);
    S.generation++;
    cJSON_ArrayForEach(v, arr) {
        if (S.napps >= MAX_APPS) break;
        App *a = &S.apps[S.napps++];
        snprintf(a->id, sizeof a->id, "%s", jstr(v, "id"));
        snprintf(a->title, sizeof a->title, "%s", jstr(v, "title"));
        snprintf(a->icon, sizeof a->icon, "%s", jstr(v, "icon"));
        snprintf(a->art, sizeof a->art, "%s", jstr(v, "art"));
        a->has_col = parse_hex(jstr(v, "color"), a->col);
        a->removable = cJSON_IsTrue(cJSON_GetObjectItem(v, "removable"));
    }
    cJSON_Delete(o);

    rebuild_items();
    for (int i = 0; i < S.nitems; i++) {
        App *a = item_app(i);
        if (a && keep[0] && !strcmp(a->id, keep)) S.focus = i;
    }
    request_icons();
    S.shelf_cur = -1;
    return 1;
}

/* Every app's icon (its tile's logo and cover are made from it): the rows on screen first, then the
 * ones next to them, then the rest; apps you've hidden last. */
void request_icons(void)
{
    int asked[MAX_APPS] = { 0 }, row = S.focus / COLS;
    for (int k = 0; k < S.nitems + S.napps; k++) {
        int i = k < S.nitems ? S.items[k] : k - S.nitems; /* (in their order on screen, then any left: hidden) */
        if (i < 0 || asked[i] || !S.apps[i].icon[0]) continue;
        asked[i] = 1;
        int d = k < S.nitems ? abs(k / COLS - row) : 99;
        S.apps[i].icon_state = 1;
        net_get_image(S.apps[i].icon, TAG_ICON, i, S.generation, 1, ICON_FIT, ICON_FIT, d <= 1 ? PRIO_NOW : d <= 3 ? PRIO_SOON : PRIO_LATER);
    }
}

/* Opens an app. params: where in it (its launch parameters, as JSON), or NULL; hint: an
 * optional message the system shows over the app once it has opened. */
void launch_app(const char *id, const char *params, const char *hint)
{
    cJSON *o = cJSON_CreateObject(), *p = params ? cJSON_Parse(params) : NULL;
    cJSON_AddStringToObject(o, "id", id);
    if (p) cJSON_AddItemToObject(o, "params", p);
    if (hint && hint[0]) cJSON_AddStringToObject(o, "toast", hint);
    char *body = cJSON_PrintUnformatted(o);
    net_post("/api/launch", body, TAG_LAUNCH);
    free(body);
    cJSON_Delete(o);
    S.away = 1;
}

void launch(const char *id, const char *hint) { launch_app(id, NULL, hint); }

void launch_recommendation(const char *id, const char *recommendation_id, const char *hint)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", id);
    cJSON_AddStringToObject(o, "recommendationId", recommendation_id);
    if (hint && hint[0]) cJSON_AddStringToObject(o, "toast", hint);
    char *body = cJSON_PrintUnformatted(o);
    net_post("/api/launch", body, TAG_LAUNCH);
    free(body);
    cJSON_Delete(o);
    S.away = 1;
}

void close_apps(const char *id)
{
    cJSON *o = cJSON_CreateObject();
    if (id) cJSON_AddStringToObject(o, "id", id);
    else cJSON_AddTrueToObject(o, "all");
    char *body = cJSON_PrintUnformatted(o);
    net_post("/api/close", body, TAG_CLOSED);
    free(body);
    cJSON_Delete(o);
}

App *app_by_id(const char *id)
{
    for (int i = 0; i < S.napps; i++)
        if (!strcmp(S.apps[i].id, id)) return &S.apps[i];
    return NULL;
}


void open_app_sheet(void)
{
    App *a = item_app(S.focus);
    if (!a || S.zone != 0) return;
    sheet_open(a->title);
    sheet_add("Move", ACT_MOVE, 0, 0);
    sheet_add("Move to Front", ACT_FRONT, 0, 0);
    int src = media_source(a);
    if (src && !(src == SRC_STREMIO ? S.rail_stremio : S.src_connected[src])) {
        char t[48];
        snprintf(t, sizeof t, "Sign In to %s", SRC_NAME[src]);
        sheet_add(t, ACT_MEDIA_LINK, src, 0);
    }
    sheet_add("Hide App", ACT_HIDE, 0, 0); /* not red: Hidden gives it back */
    if (a->removable) sheet_add_app("Delete App", ACT_DELETE_ASK, a->id, 1); /* from the TV, like tvOS's */
    sheet_add("Cancel", ACT_CANCEL, 0, 0);
}

void open_hidden_sheet(void)
{
    sheet_open("Hidden Apps");
    int n = 0;
    for (int i = 0; i < S.napps; i++) {
        if (!is_hidden(S.apps[i].id)) continue;
        char label[200];
        snprintf(label, sizeof label, "Show \xE2\x80\x9C%s\xE2\x80\x9D", S.apps[i].title);
        sheet_add_app(label, ACT_UNHIDE, S.apps[i].id, 0);
        n++;
    }
    if (n > 1) sheet_add("Show All", ACT_UNHIDE_ALL, 0, 0);
    sheet_add("Cancel", ACT_CANCEL, 0, 0);
}

void move_item_to(int to)
{
    int last = S.nitems - 1;
    if (last >= 0 && S.items[last] < 0) last--; /* never past the Hidden tile */
    if (to > last) to = last;
    if (to < 0 || to == S.focus) return;
    int v = S.items[S.focus];
    if (to < S.focus) memmove(&S.items[to + 1], &S.items[to], (S.focus - to) * sizeof(int));
    else memmove(&S.items[S.focus], &S.items[S.focus + 1], (to - S.focus) * sizeof(int));
    S.items[to] = v;
    S.focus = to;
}

/* the app list hasn't come yet (and Home+ isn't stuck on an error) */
int apps_loading(void) { return !S.nitems && (S.setup <= SETUP_RESTARTING || S.setup == SETUP_DONE); }
