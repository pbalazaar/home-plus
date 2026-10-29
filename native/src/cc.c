/* Home+ native: the Control Center, like tvOS's: its controls under the clock, editing them, Settings and Input keys. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "home.h"

static const struct { const char *name; int icon; } CC_ICONS[] = { /* the service names a control's glyph */
    { "power", ICON_POWER }, { "wifi", ICON_WIFI }, { "ethernet", ICON_ETHERNET }, { "screenOff", ICON_SCREEN_OFF },
    { "sound", ICON_SOUND }, { "timer", ICON_TIMER }, { "input", ICON_INPUT }, { "recents", ICON_RECENTS },
    { "access", ICON_ACCESS }, { "settings", ICON_SETTINGS }, { "picture", ICON_PICTURE }, { "eye", ICON_EYE },
    { "soundMode", ICON_SOUND_MODE }, { "brightness", ICON_BRIGHTNESS }, { "energy", ICON_ENERGY }, { "game", ICON_GAME },
    { "hdr", ICON_HDR }, { "ambient", ICON_AMBIENT }, { "capture", ICON_CAPTURE },
};

/* a control's name and value (whole: a long one scrolls while it has focus) */
void cc_texts(CCItem *c, const char *title, const char *value)
{
    if (!c->title || strcmp(c->title_s, title)) {
        snprintf(c->title_s, sizeof c->title_s, "%s", title);
        tex_free(&c->title);
        c->title = text_render(S.f_semi, c->units == 4 ? 24 : CC_TITLE_PX, title, 0, &c->tw, &c->th);
    }
    if (strcmp(c->value_s, value)) {
        snprintf(c->value_s, sizeof c->value_s, "%s", value);
        tex_free(&c->value);
        if (value[0]) c->value = text_render(S.f_medium, 18, value, 0, &c->vw, &c->vh);
    }
}

static int cc_icon(const char *name)
{
    for (size_t k = 0; k < sizeof CC_ICONS / sizeof CC_ICONS[0]; k++)
        if (!strcmp(CC_ICONS[k].name, name)) return CC_ICONS[k].icon;
    return ICON_CONTROLS; /* one the launcher doesn't know: the Control Center's own */
}

/* the Control Center's state, from the service (also its answer to every action): the
 * controls on show, the catalogue, the picture modes and the inputs */
void parse_control(const char *json)
{
    cJSON *o = cJSON_Parse(json), *v;
    if (!o) return;
    if (S.cc.picked < 0) { /* (while one is being moved, the list on screen is the one that counts) */
        static CCItem old[MAX_CC];
        int nold = S.cc.n;
        memcpy(old, S.cc.it, sizeof old);
        memset(S.cc.it, 0, sizeof S.cc.it);
        S.cc.n = 0;
        cJSON_ArrayForEach(v, cJSON_GetObjectItem(o, "controls")) {
            if (S.cc.n >= MAX_CC) break;
            CCItem *c = &S.cc.it[S.cc.n++];
            snprintf(c->id, sizeof c->id, "%s", jstr(v, "id"));
            snprintf(c->kind, sizeof c->kind, "%s", jstr(v, "kind"));
            snprintf(c->name_s, sizeof c->name_s, "%s", jstr(v, "name"));
            const char *size = jstr(v, "size");
            c->units = !strcmp(size, "large") ? 4 : !strcmp(size, "wide") ? 2 : 1;
            c->icon = cc_icon(jstr(v, "icon"));
            cJSON *on = cJSON_GetObjectItem(v, "on"); /* true, false, or none: tvOS shows which in the glyph */
            c->on = cJSON_IsTrue(on);
            c->stateful = cJSON_IsBool(on);
            c->available = cJSON_IsTrue(cJSON_GetObjectItem(v, "available"));
            for (int k = 0; k < nold; k++) { /* the same control: its texts and motion stay */
                CCItem *p = &old[k];
                if (strcmp(p->id, c->id) || p->units != c->units) continue;
                memcpy(c->title_s, p->title_s, sizeof c->title_s);
                memcpy(c->value_s, p->value_s, sizeof c->value_s);
                c->title = p->title, c->tw = p->tw, c->th = p->th, p->title = NULL;
                c->value = p->value, c->vw = p->vw, c->vh = p->vh, p->value = NULL;
                c->s = p->s, c->sv = p->sv, c->flash = p->flash, c->shake = p->shake;
                break;
            }
            cc_texts(c, jstr(v, "title"), jstr(v, "value"));
        }
        for (int k = 0; k < nold; k++) {
            tex_free(&old[k].title);
            tex_free(&old[k].value);
        }
        cc_pack();
        int last = S.cc.n + (S.cc.editing ? 2 : 0) - 1, net = cc_find("network");
        if (S.cc.to_network && net >= 0) S.cc.index = net;
        S.cc.to_network = 0;
        if (S.cc.index > last) S.cc.index = last < 0 ? 0 : last;
    }
    S.cc.ncat = 0;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(o, "catalog")) {
        if (S.cc.ncat >= MAX_CAT) break;
        __typeof__(S.cc.cat[0]) *e = &S.cc.cat[S.cc.ncat++];
        snprintf(e->id, sizeof e->id, "%s", jstr(v, "id"));
        snprintf(e->title, sizeof e->title, "%s", jstr(v, "title"));
        snprintf(e->group, sizeof e->group, "%s", jstr(v, "group"));
        int ns = 0;
        cJSON *sz;
        cJSON_ArrayForEach(sz, cJSON_GetObjectItem(v, "sizes")) {
            const char *t = cJSON_IsString(sz) ? sz->valuestring : "";
            if (ns < 3) e->sizes[ns++] = !strcmp(t, "large") ? 4 : !strcmp(t, "wide") ? 2 : 1;
        }
        e->sizes[ns] = 0;
        e->available = cJSON_IsTrue(cJSON_GetObjectItem(v, "available"));
        e->placed = cJSON_IsTrue(cJSON_GetObjectItem(v, "placed"));
        e->pinned = cJSON_IsTrue(cJSON_GetObjectItem(v, "pinned"));
        e->icon = cc_icon(jstr(v, "icon"));
    }
    cJSON *pic = cJSON_GetObjectItem(o, "picture");
    const char *mode = jstr(pic, "mode");
    S.cc.nmodes = 0;
    S.cc.mode = -1;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(pic, "modes")) {
        if (S.cc.nmodes >= (int)(sizeof S.cc.modes / sizeof S.cc.modes[0])) break;
        snprintf(S.cc.modes[S.cc.nmodes].id, sizeof S.cc.modes[0].id, "%s", jstr(v, "id"));
        snprintf(S.cc.modes[S.cc.nmodes].name, sizeof S.cc.modes[0].name, "%s", jstr(v, "label"));
        if (!strcmp(S.cc.modes[S.cc.nmodes].id, mode)) S.cc.mode = S.cc.nmodes;
        S.cc.nmodes++;
    }
    S.cc.ninputs = 0;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(o, "inputs")) {
        if (S.cc.ninputs >= 12) break;
        __typeof__(S.cc.inputs[0]) *in = &S.cc.inputs[S.cc.ninputs++];
        copy_id(in->app, jstr(v, "appId"));
        const char *dev = jstr(v, "device");
        if (dev[0]) snprintf(in->name, sizeof in->name, "%s \xC2\xB7 %s", jstr(v, "label"), dev); /* "Soundbar · PS5" */
        else snprintf(in->name, sizeof in->name, "%s", jstr(v, "label"));
        in->plugged = cJSON_IsTrue(cJSON_GetObjectItem(v, "plugged"));
    }
    if (S.cc.list == 1 || S.cc.list == 2) cc_list_texts(); /* the tick follows the picture mode; the inputs come in */
    cJSON_Delete(o);
}

void parse_system(const char *json)
{
    cJSON *o = cJSON_Parse(json), *v;
    if (!o) return;
    S.nsettings = 0;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(o, "settings")) {
        if (S.nsettings >= MAX_SYS) break;
        copy_id(S.settings[S.nsettings].id, jstr(v, "id"));
        snprintf(S.settings[S.nsettings].label, sizeof S.settings[0].label, "%s", jstr(v, "label"));
        S.nsettings++;
    }
    cJSON_Delete(o);
}

/* ---- Control Center */

float cc_grid_x(void) { return RIGHT - 4 * CC_U - 3 * CC_G; } /* its right edge: the clock's */
float cc_grid_y(void) { return TOP_Y + BAR + 26; }

/* Opening, the Control Center comes in from past the screen's right edge as one panel, each row a moment after
 * the one above; closing, it goes back out there, the bottom row first. With Reduce Motion it fades in and out
 * where it is. How far in row r is, 0..1 (under the grid, the hints and Reset are the row after the last): a row
 * sets off a little after the one above and gets there with it, so none stops short. */
static float cc_in(int r)
{
    float open = fminf(fmaxf(S.cc.t, 0), 1), lag = 0.03f * (r > 0 ? r : 0);
    return fminf(fmaxf((open - lag) / (1 - lag), 0), 1);
}

/* how far right of its place the Control Center draws something whose left edge is `left`, `in` of the way in */
float cc_slide(float in, float left) { return S.reduce_motion ? 0 : (1 - in) * (W + 40 - left); }

/* ...and how strongly: coming in from off the screen, it's all there by half way */
static float cc_fade(float in) { return S.reduce_motion ? in : smooth01(0, 0.5f, in); }

/* Places the controls in order on the four-unit grid, each in the first space it fits (reading
 * order), in the rows that fit under the clock; the + slot (while editing) takes the first free
 * unit after the last of them. */
void cc_pack(void)
{
    unsigned char used[CC_ROWS][4];
    memset(used, 0, sizeof used);
    int last = -1;
    for (int i = 0; i < S.cc.n; i++) {
        CCItem *c = &S.cc.it[i];
        c->w = c->units == 1 ? 1 : 2;
        c->h = c->units == 4 ? 2 : 1;
        c->row = -1;
        for (int r = 0; r + c->h <= CC_ROWS && c->row < 0; r++)
            for (int col = 0; col + c->w <= 4 && c->row < 0; col++) {
                int fits = 1;
                for (int y = r; y < r + c->h; y++)
                    for (int x = col; x < col + c->w; x++) fits &= !used[y][x];
                if (!fits) continue;
                for (int y = r; y < r + c->h; y++)
                    for (int x = col; x < col + c->w; x++) used[y][x] = 1;
                c->row = r, c->col = col;
                if (r * 4 + col > last) last = r * 4 + col;
            }
    }
    S.cc.plus_row = -1;
    for (int u = last + 1; u < CC_ROWS * 4; u++)
        if (!used[u / 4][u % 4]) {
            S.cc.plus_row = u / 4, S.cc.plus_col = u % 4;
            break;
        }
}

/* the controls with a place on the grid */
int cc_placed(void)
{
    int k = 0;
    for (int i = 0; i < S.cc.n; i++) k += S.cc.it[i].row >= 0;
    return k;
}

/* Gives control i the first of `sizes` (units, 0-ended) that leaves `need` controls on the grid, it
 * among them; 0, and as it was, if none does (as the overlay's fit()) */
int cc_fit(int i, const char *sizes, int need)
{
    CCItem *c = &S.cc.it[i];
    int was = c->units;
    for (const char *u = sizes; *u; u++) {
        c->units = *u;
        cc_pack();
        if (c->row >= 0 && cc_placed() >= need) return 1;
    }
    c->units = was;
    cc_pack();
    return 0;
}

/* the rows in use (with the + slot while editing) */
static int cc_rows(void)
{
    int rows = 0;
    for (int i = 0; i < S.cc.n; i++)
        if (S.cc.it[i].row >= 0 && S.cc.it[i].row + S.cc.it[i].h > rows) rows = S.cc.it[i].row + S.cc.it[i].h;
    if (S.cc.editing && S.cc.plus_row + 1 > rows) rows = S.cc.plus_row + 1;
    return rows;
}

/* Where focusable thing i is: a control (0..n-1); while editing, the + slot (n) and Reset
 * (n + 1). 0: it isn't on screen (no room for it). */
int cc_rect(int i, float *x, float *y, float *w, float *h)
{
    int col, row, cw = 1, ch = 1;
    if (i >= 0 && i < S.cc.n) {
        const CCItem *c = &S.cc.it[i];
        if (c->row < 0) return 0;
        col = c->col, row = c->row, cw = c->w, ch = c->h;
    } else if (i == S.cc.n && S.cc.editing && S.cc.plus_row >= 0) {
        col = S.cc.plus_col, row = S.cc.plus_row;
    } else if (i == S.cc.n + 1 && S.cc.editing) { /* under the grid and its hint */
        *x = cc_grid_x(), *y = cc_grid_y() + cc_rows() * (CC_U + CC_G) + 44;
        *w = 4 * CC_U + 3 * CC_G, *h = 64;
        return 1;
    } else {
        return 0;
    }
    *x = cc_grid_x() + col * (CC_U + CC_G);
    *y = cc_grid_y() + row * (CC_U + CC_G);
    *w = cw * CC_U + (cw - 1) * CC_G;
    *h = ch * CC_U + (ch - 1) * CC_G;
    return 1;
}

/* ...and how far right of that it's drawn while the panel comes in (cc_in): the pointer finds it there, not in
 * the place it's on its way to */
float cc_shift(int i)
{
    int r = i >= 0 && i < S.cc.n ? S.cc.it[i].row : i == S.cc.n ? S.cc.plus_row : cc_rows();
    return cc_slide(cc_in(r), cc_grid_x());
}

int cc_find(const char *id)
{
    for (int i = 0; i < S.cc.n; i++)
        if (!strcmp(S.cc.it[i].id, id)) return i;
    return -1;
}

/* what went wrong, said in the hint's place under the grid as over other apps (NULL: nothing) */
void cc_problem(const char *text)
{
    tex_free(&S.cc.problem);
    if (text) S.cc.problem = text_render(S.f_medium, HINT_PX, text, 4 * CC_U + 3 * CC_G, &S.cc.prw, &S.cc.prh);
}

/* asks the service to do something (the overlay's run()): what went wrong before is let go */
void cc_post(const char *body)
{
    cc_problem(NULL);
    net_post("/api/control", body, TAG_CONTROL);
}

void cc_open(void)
{
    S.cc.open = 1;
    S.cc.editing = S.cc.list = S.cc.list_only = 0;
    cc_problem(NULL);
    S.cc.picked = S.cc.ran = -1;
    int net = cc_find("network"); /* nearest the clock, and harmless (unlike Power Off) */
    S.cc.index = net >= 0 ? net : 0;
    S.cc.to_network = net < 0; /* the first time, the list is on its way */
    S.cc.focus_at = SDL_GetTicks();
    net_get_json("/api/control", TAG_CONTROL, 0, 0);
}

void cc_close(void)
{
    if (S.cc.picked >= 0) { /* one being moved stays where it is now */
        S.cc.picked = -1;
        cc_save();
    }
    S.cc.open = S.cc.editing = S.cc.list = S.cc.list_only = 0;
}

/* saves the list as it is on screen (the service answers with the whole state) */
void cc_save(void)
{
    cJSON *o = cJSON_CreateObject(), *arr;
    cJSON_AddStringToObject(o, "action", "layout");
    arr = cJSON_AddArrayToObject(o, "controls");
    for (int i = 0; i < S.cc.n; i++) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "id", S.cc.it[i].id);
        cJSON_AddStringToObject(c, "size", S.cc.it[i].units == 4 ? "large" : S.cc.it[i].units == 2 ? "wide" : "small");
        cJSON_AddItemToArray(arr, c);
    }
    char *body = cJSON_PrintUnformatted(o);
    cc_post(body);
    free(body);
    cJSON_Delete(o);
}

/* the picked control moves one place earlier or later in the order (Left, Right), or to the row
 * above or below (Up, Down), and the others reflow around it */
static void cc_move_picked(int dx, int dy)
{
    int i = S.cc.picked, to = i + dx;
    if (dy) {
        const CCItem *c = &S.cc.it[i];
        int row = dy > 0 ? c->row + c->h : c->row - 1, best = -1;
        float bd = 1e9f, cx = c->col + c->w / 2.0f;
        for (int k = 0; k < S.cc.n; k++) {
            const CCItem *o = &S.cc.it[k];
            if (k == i || o->row < 0 || row < o->row || row >= o->row + o->h) continue;
            float d = fabsf(o->col + o->w / 2.0f - cx);
            if (d < bd) bd = d, best = k;
        }
        if (best < 0 && dy > 0) best = S.cc.n - 1; /* below the last row: the end */
        if (best < 0) return;
        to = best;
    }
    if (to < 0 || to >= S.cc.n || to == i) return;
    CCItem t = S.cc.it[i];
    if (to > i) memmove(&S.cc.it[i], &S.cc.it[i + 1], (size_t)(to - i) * sizeof t);
    else memmove(&S.cc.it[to + 1], &S.cc.it[to], (size_t)(i - to) * sizeof t);
    S.cc.it[to] = t;
    S.cc.picked = S.cc.index = to;
    cc_pack();
}

/* the nearest control that way (they come in three sizes); up past the top row: the clock */
void cc_move(int dx, int dy)
{
    if (S.cc.picked >= 0) {
        cc_move_picked(dx, dy);
        return;
    }
    float x, y, w, h, best_d = 1e9f;
    int best = -1, n = S.cc.n + (S.cc.editing ? 2 : 0);
    if (!cc_rect(S.cc.index, &x, &y, &w, &h)) x = cc_grid_x(), y = cc_grid_y(), w = h = 0;
    float cx = x + w / 2, cy = y + h / 2;
    for (int i = 0; i < n; i++) {
        float ix, iy, iw, ih;
        if (i == S.cc.index || !cc_rect(i, &ix, &iy, &iw, &ih)) continue;
        float ax = ix + iw / 2 - cx, ay = iy + ih / 2 - cy, along = dx ? ax * dx : ay * dy, across = fabsf(dx ? ay : ax);
        int beside = dx ? iy < y + h && iy + ih > y : ix < x + w && ix + iw > x;
        if (along <= 1 || (!beside && across > along)) continue; /* as tvOS's focus: beside it, or within 45 degrees */
        float d = along + 2.5f * across;
        if (d < best_d) best_d = d, best = i;
    }
    if (best >= 0) S.cc.index = best, S.cc.focus_at = SDL_GetTicks(), cc_problem(NULL); /* what went wrong goes with the focus */
    else if (dy < 0 && !S.cc.editing) cc_close();
}

/* While the Control Center is being edited, or its last row is over the dock, the home screen steps
 * back (as iOS's does): this much black over it, 0..1, in the picture its glass refracts too, so the
 * glass over the dock (that row, Reset, the hints) dims with what's under it */
float cc_dim(void)
{
    int over_dock = cc_grid_y() + cc_rows() * (CC_U + CC_G) - CC_G > DOCK_Y;
    return S.cc.editing || over_dock ? 150 / 255.0f * fminf(fmaxf(S.cc.t, 0), 1) : 0;
}

/* editing: OK picks a control up (and puts it down), the + slot adds one, Reset asks */
static void cc_edit_ok(void)
{
    if (S.cc.picked >= 0) {
        S.cc.picked = -1;
        cc_save();
    } else if (S.cc.index < S.cc.n) {
        S.cc.picked = S.cc.index;
    } else if (S.cc.index == S.cc.n) {
        if (S.cc.plus_row >= 0) cc_list_open(3);
    } else {
        sheet_open("Reset Control Center?");
        sheet_body("The controls go back to how they were at first.");
        sheet_add("Reset", ACT_CC_RESET, 0, 1);
        sheet_add("Cancel", ACT_CANCEL, 0, 0);
        S.sheet.index = 1;
    }
}

/* the catalogue's entry for a control */
static int cc_cat(const char *id)
{
    for (int i = 0; i < S.cc.ncat; i++)
        if (!strcmp(S.cc.cat[i].id, id)) return i;
    return -1;
}

/* holding OK: Edit Controls; while editing, the focused control's sizes and Remove */
void cc_long_press(void)
{
    if (S.cc.list || S.cc.picked >= 0) return;
    if (!S.cc.editing) {
        sheet_open("Control Center");
        sheet_add("Edit Controls", ACT_CC_EDIT, 0, 0);
        sheet_add("Cancel", ACT_CANCEL, 0, 0);
        return;
    }
    if (S.cc.index >= S.cc.n) return;
    CCItem *c = &S.cc.it[S.cc.index];
    int e = cc_cat(c->id);
    int sizes = e >= 0 && S.cc.cat[e].sizes[1]; /* a choice of size only when there are two or more */
    int removable = e < 0 || !S.cc.cat[e].pinned; /* not Settings: the only way in */
    if (!sizes && !removable) { /* nothing to change about it */
        c->shake = SDL_GetTicks();
        return;
    }
    sheet_open(c->name_s[0] ? c->name_s : c->title_s);
    static const char *const NAME[5] = { "", "Small", "Wide", "", "Large" };
    for (int k = 0; sizes && S.cc.cat[e].sizes[k]; k++) {
        int u = S.cc.cat[e].sizes[k];
        sheet_add(NAME[u], ACT_CC_SIZE, u, 0);
        S.sheet.it[S.sheet.n - 1].check = u == c->units; /* in the middle like the rest, ticked on the right */
    }
    if (removable) sheet_add("Remove", ACT_CC_REMOVE, 0, 1);
    sheet_add("Cancel", ACT_CANCEL, 0, 0);
    if (!sizes) S.sheet.index = S.sheet.n - 1; /* Remove first: Cancel has the focus, so a stray OK keeps it */
}

/* a control from the catalogue, at the end in the first of its sizes that fits, with the focus; with
 * no room for it at any of them it isn't added, and that's said under the grid */
void cc_add(int cat)
{
    if (cat < 0 || cat >= S.cc.ncat || S.cc.n >= MAX_CC) return;
    int had = cc_placed();
    CCItem *c = &S.cc.it[S.cc.n++];
    memset(c, 0, sizeof *c);
    memcpy(c->id, S.cc.cat[cat].id, sizeof c->id); /* the same size, and ended */
    c->units = S.cc.cat[cat].sizes[0] ? S.cc.cat[cat].sizes[0] : 1;
    c->icon = S.cc.cat[cat].icon;
    c->available = 1;
    if (!cc_fit(S.cc.n - 1, S.cc.cat[cat].sizes[0] ? S.cc.cat[cat].sizes : "\1", had + 1)) {
        char t[80];
        snprintf(t, sizeof t, "No room for %s", S.cc.cat[cat].title);
        S.cc.n--;
        cc_pack();
        cc_problem(t);
        return;
    }
    cc_texts(c, S.cc.cat[cat].title, ""); /* its value comes with the service's answer */
    S.cc.index = S.cc.n - 1;
    cc_save();
}

void cc_activate(void)
{
    if (S.cc.list) {
        cc_list_ok();
        return;
    }
    if (S.cc.editing) {
        cc_edit_ok();
        return;
    }
    if (S.cc.index >= S.cc.n) return;
    CCItem *c = &S.cc.it[S.cc.index];
    if (!c->available) { /* Not Available: nothing happens, which it shows */
        c->shake = SDL_GetTicks();
        return;
    }
    if (!strcmp(c->id, "power")) {
        cc_post("{\"action\":\"powerOff\"}");
        cc_close();
    } else if (!strcmp(c->id, "screenOff")) {
        cc_post("{\"action\":\"screenOff\"}");
        cc_close();
    } else if (!strcmp(c->id, "network")) { /* LG's network settings, as its own Quick Settings open them */
        cc_close();
        launch_app("com.webos.app.firstuse-overlay", "{\"target\":\"network\"}", NULL);
    } else if (!strcmp(c->id, "pictureMode")) {
        cc_list_open(1);
    } else if (!strcmp(c->id, "inputs")) {
        cc_list_open(2);
    } else if (!strcmp(c->id, "recents")) {
        cc_close();
        sw_open();
    } else if (!strcmp(c->id, "accessibility")) {
        cc_close();
        settings_page(PAGE_ACCESS, 0);
    } else if (!strcmp(c->id, "settings")) {
        cc_close();
        open_settings_sheet();
    } else if (!strcmp(c->kind, "stepper") || !strcmp(c->kind, "switch")) { /* the service steps or switches it */
        char body[96];
        snprintf(body, sizeof body, "{\"action\":\"run\",\"value\":\"%s\"}", c->id);
        cc_post(body);
        S.cc.ran = S.cc.index;
        if (c->units == 1 && !strcmp(c->kind, "stepper")) c->flash = SDL_GetTicks(); /* its new value shows under it */
    }
}

/* a switch that's off: its glyph on a pale disc (on a focused, white control, a grey one) */
static void cc_disc(float cx, float cy, float d, float g, float a)
{
    r_shape(cx - d / 2, cy - d / 2, d, d, d / 2, 255, 255, 255, (Uint8)(255 * a * 0.24f * (1 - g)), 0, 1, 1);
    if (g > 0.01f) r_shape(cx - d / 2, cy - d / 2, d, d, d / 2, 0, 0, 0, (Uint8)(255 * a * 0.08f * g), 0, 1, 1);
}

Tex *cc_glyph(int icon, int big)
{
    if (icon < 0 || icon >= ICON_COUNT) icon = ICON_CONTROLS;
    Tex **t = big ? &S.cc.glyph_big[icon] : &S.cc.glyph[icon];
    if (!*t) *t = gfx_icon(icon, big ? 128 : 64);
    return *t;
}

/* Makes the glyphs of the controls on show while nothing moves: made as they were first drawn, they held
 * up the Control Center's first frames (74 ms on the C3, four refreshes; request_data() loads the list). */
void cc_warm(void)
{
    for (int i = 0; i < S.cc.n; i++) cc_glyph(S.cc.it[i].icon, S.cc.it[i].units == 4);
}

void draw_control_center(void)
{
    S.cc.marquee = 0;
    if (S.cc.t < 0.004f) return;
    float open = fminf(fmaxf(S.cc.t, 0), 1), lt = fminf(fmaxf(S.cc.list_t, 0), 1);
    Uint32 now = SDL_GetTicks();
    float dim = cc_dim(); /* (also behind the gallery: still editing) */
    if (dim > 0.002f) r_fill(0, 0, 0, (Uint8)(255 * dim)); /* editing, or a row over the dock: the home screen steps back */
    for (int i = 0; i < S.cc.n; i++) {
        CCItem *c = &S.cc.it[i];
        float x, y, w, h;
        if (!cc_rect(i, &x, &y, &w, &h)) continue; /* no room for it */
        float in = cc_in(c->row), ti = cc_fade(in) * (1 - lt); /* a list takes their place */
        if (ti <= 0.004f) continue;
        float k = c->s, g = fminf(fmaxf(k, 0), 1);
        float sc = (1 + 0.06f * k) * (i == S.cc.picked ? 1.08f : 1);
        float cx = x + w / 2 + cc_slide(in, cc_grid_x()), cy = y + h / 2;
        if (k > 0.01f) {
            float dx, dy;
            sway(&dx, &dy);
            cx += dx * k;
            cy += dy * k;
        }
        if (S.cc.editing && !S.reduce_motion && i != S.cc.picked) { /* editing: each wobbles a little, out of step */
            float ph = now / 1000.0f * 7.0f + i * 1.7f;
            cx += sinf(ph) * 1.4f;
            cy += cosf(ph * 1.3f) * 1.1f;
        }
        if (c->shake && now - c->shake < 450) { /* the TV said no */
            float p = (now - c->shake) / 450.0f;
            cx += sinf(p * 28) * 9 * (1 - p);
        }
        float rw = w * sc, rh = h * sc, rx = cx - rw / 2, ry = cy - rh / 2, rad = c->h == 2 ? 40 * sc : rh / 2;
        float lift = i == S.cc.picked ? 1 : k;
        if (lift > 0.01f) r_shadow(rx, ry, rw, rh, rad, 18, 8, 0.4f * lift * ti);
        panel(0, rx, ry, rw, rh, rad, 1, k, ti);
        if (g > 0.01f) r_shape(rx, ry, rw, rh, rad, 245, 245, 247, (Uint8)(255 * ti * g), 0, 1, 1);
        if (S.cc.editing && (S.reduce_motion || i == S.cc.picked)) /* still: an outline says it's editing */
            r_shape(rx, ry, rw, rh, rad, 255, 255, 255, (Uint8)(150 * ti), 2.0f, 1, 1);
        Uint8 col = (Uint8)(255 - 227 * g), ca = (Uint8)(255 * ti * (c->available ? 1 : 0.45f)); /* Not Available: dimmed */
        Uint8 gr = col, gg = col, gb = col; /* the glyph: tvOS 26's cyan when it's on (darker on white) */
        if (c->on && c->available) {
            gr = (Uint8)(16 + (0 - 16) * g), gg = (Uint8)(232 + (137 - 232) * g), gb = (Uint8)(244 + (173 - 244) * g);
        }
        int disc = c->stateful && !c->on && c->available && !strcmp(c->kind, "switch"); /* a switch that's off: on a pale disc */
        if (c->h == 2) { /* Large: its glyph large, its name under it */
            float is = 76 * sc;
            if (disc) cc_disc(cx, ry + rh * 0.42f, is * 1.35f, g, ti);
            r_image(cc_glyph(c->icon, 1), cx - is / 2, ry + rh * 0.42f - is / 2, is, is, gr, gg, gb, ca, 0);
            r_image(c->title, cx - c->tw / 2.0f, ry + rh - 26 * sc - c->th, c->tw, c->th, col, col, col, ca, 0);
        } else if (c->w == 2) { /* Wide: the glyph, then the name over its value, clear of the round end */
            float is = 30 * sc, ix = rx + 18 * sc, tx = ix + is + 12 * sc, room = rx + rw - 22 * sc - tx;
            if (disc) cc_disc(ix + is / 2, cy, is * 1.4f, g, ti);
            r_image(cc_glyph(c->icon, 0), ix, cy - is / 2, is, is, gr, gg, gb, ca, 0);
            float vh = c->value ? c->vh : 0, ty = cy - (c->th + vh) / 2;
            Uint32 since = i == S.cc.index && S.cc.open && !S.cc.editing ? S.cc.focus_at : 0; /* only the focused one scrolls */
            draw_marquee(c->title, c->tw, c->th, tx, ty, room, col, ca, since);
            draw_marquee(c->value, c->vw, c->vh, tx, ty + c->th, room, col, (Uint8)(TEXT_SECONDARY * ca / 255), since);
        } else { /* Small: the glyph; a stepper's new value shows under it for a moment */
            float is = 40 * sc;
            if (disc) cc_disc(cx, cy, is * 1.4f, g, ti);
            r_image(cc_glyph(c->icon, 0), cx - is / 2, cy - is / 2, is, is, gr, gg, gb, ca, 0);
            if (c->flash && now - c->flash < 1800 && c->value) {
                float f = 1 - smooth01(1200, 1800, (float)(now - c->flash));
                draw_text(c->value, c->vw, c->vh, cx - c->vw / 2.0f, ry + rh + 8, (Uint8)(TEXT_STATIC * ti * f));
            }
        }
    }
    if (S.cc.editing && lt < 0.99f) {
        float x, y, w, h, in = cc_in(S.cc.plus_row), a = cc_fade(in) * (1 - lt);
        if (cc_rect(S.cc.n, &x, &y, &w, &h)) { /* the + slot: a ring with a plus */
            float g = fminf(fmaxf(S.cc.plus_s, 0), 1);
            x += cc_slide(in, cc_grid_x());
            r_shape(x, y, w, h, h / 2, 255, 255, 255, (Uint8)(110 * a * (1 - g)), 2.0f, 1, 1);
            if (g > 0.01f) r_shape(x, y, w, h, h / 2, 245, 245, 247, (Uint8)(255 * a * g), 0, 1, 1);
            Uint8 col = (Uint8)(255 - 227 * g);
            r_image(cc_glyph(ICON_PLUS, 0), x + (w - 34) / 2, y + (h - 34) / 2, 34, 34, col, col, col, (Uint8)(255 * a), 0);
        }
        if (!S.cc.edit_hint) { /* what OK does, under the grid */
            S.cc.edit_hint = text_render(S.f_medium, HINT_PX, "OK picks it up  \xC2\xB7  Hold for options", 0, &S.cc.ehw, &S.cc.ehh);
            S.cc.move_hint = text_render(S.f_medium, HINT_PX, "Arrows move it  \xC2\xB7  OK puts it down", 0, &S.cc.mhw, &S.cc.mhh);
            S.cc.no_room = text_render(S.f_medium, HINT_PX, "No room  \xC2\xB7  Hold OK on one for options", 0, &S.cc.nrw, &S.cc.nrh);
            S.cc.reset = text_render(S.f_semi, 24, "Reset", 0, &S.cc.rsw, &S.cc.rsh);
        }
        Tex *ht = S.cc.picked >= 0 ? S.cc.move_hint : S.cc.plus_row < 0 ? S.cc.no_room : S.cc.edit_hint;
        int hw = ht == S.cc.move_hint ? S.cc.mhw : ht == S.cc.no_room ? S.cc.nrw : S.cc.ehw;
        int hh = ht == S.cc.move_hint ? S.cc.mhh : ht == S.cc.no_room ? S.cc.nrh : S.cc.ehh;
        in = cc_in(cc_rows()), a = cc_fade(in) * (1 - lt); /* under the grid: what OK does, and Reset */
        float dx = cc_slide(in, cc_grid_x());
        if (!S.cc.problem) draw_text(ht, hw, hh, RIGHT - hw + dx, cc_grid_y() + cc_rows() * (CC_U + CC_G) + 4, (Uint8)(TEXT_SECONDARY * a));
        if (cc_rect(S.cc.n + 1, &x, &y, &w, &h)) { /* Reset: red, like sheets' destructive buttons */
            static const Uint8 G[3] = { RED_ON_GLASS }, F[3] = { RED_ON_WHITE };
            float g = fminf(fmaxf(S.cc.reset_s, 0), 1);
            x += dx;
            panel(0, x, y, w, h, h / 2, 1, g, a);
            if (g > 0.01f) r_shape(x, y, w, h, h / 2, 245, 245, 247, (Uint8)(255 * a * g), 0, 1, 1);
            r_image(S.cc.reset, x + (w - S.cc.rsw) / 2, y + (h - S.cc.rsh) / 2, S.cc.rsw, S.cc.rsh, (Uint8)(G[0] + (F[0] - G[0]) * g),
                    (Uint8)(G[1] + (F[1] - G[1]) * g), (Uint8)(G[2] + (F[2] - G[2]) * g), (Uint8)(255 * a), 0);
        }
    }
    if (S.cc.problem && lt < 0.99f) { /* what went wrong, in the hint's place under the grid */
        int rows = cc_rows() > 1 ? cc_rows() : 1;
        float in = cc_in(rows);
        draw_text(S.cc.problem, S.cc.prw, S.cc.prh, RIGHT - S.cc.prw + cc_slide(in, cc_grid_x()), cc_grid_y() + rows * (CC_U + CC_G) + 4,
                  (Uint8)(TEXT_STATIC * cc_fade(in) * (1 - lt)));
    }
    if (lt > 0.004f) { /* (a list, or the gallery, comes and goes with the panel too) */
        if (S.cc.list_kind == 3) draw_cc_gallery(lt * cc_fade(open), open);
        else draw_cc_list(lt * cc_fade(open), open);
    }
}

/* The remote's Settings key while Home+ is in front (Home+'s key filter, or LG Input Hook's script, asks the
 * service): the Control Center, as from the clock, over whatever was open; pressed again, it closes. */
void settings_key(void)
{
    wake(0); /* a dimmed or dark screen comes back too */
    if (S.cc.open) {
        cc_close();
        return;
    }
    if (S.sheet.open) sheet_dismiss();
    if (S.recents.open) sw_reset();
    if (S.moving) { /* moving an app: it stays where it is now */
        S.moving = 0;
        save_layout();
    }
    if (S.zone != ZONE_TOP) enter_top(S.focus % COLS);
    cc_open();
}

/* The remote's Input key (Home+'s key filter, LG Input Hook), while Home+ is in front: the Control Center's
 * inputs, as over other apps. Opened for them alone, Back closes them; the key again closes them either way. */
void input_key(void)
{
    wake(0); /* a dimmed or dark screen comes back too */
    if (S.cc.open && S.cc.list == 2) {
        cc_close();
        return;
    }
    int was_open = S.cc.open;
    if (!was_open) settings_key(); /* opens the Control Center, as the Settings key does */
    cc_list_open(2);
    S.cc.list_only = !was_open;
}
