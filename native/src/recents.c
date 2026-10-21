/* Home+ native: Recents, an App Switcher like tvOS's: the running apps as big cards over the home screen. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "home.h"

static void sw_clear(void)
{
    for (int i = 0; i < S.recents.n; i++) tex_free(&S.recents.c[i].name);
    S.recents.n = S.recents.index = S.recents.close_all = S.recents.zooming = 0;
    S.recents.zoom = S.recents.zoomv = 0;
    S.recents.empty_at = 0;
}

/* gone at once (Home+ went to the back, or came home) */
void sw_reset(void)
{
    sw_clear();
    S.recents.open = S.recents.loading = S.recents.failed = 0;
    S.recents.t = S.recents.tv = 0;
}

/* Recents opens at once over the blurred home screen; the running apps arrive a
 * moment later, so keys pressed meanwhile never reach the grid behind. */
void sw_open(void)
{
    sheet_close();
    sw_clear();
    S.recents.open = S.recents.loading = 1;
    S.recents.failed = 0;
    net_get_json("/api/running", TAG_RUNNING, 0, 0);
}

void sw_fill(const char *json)
{
    if (!S.recents.open || !S.recents.loading) return; /* closed meanwhile */
    S.recents.loading = 0;
    cJSON *o = cJSON_Parse(json), *v;
    cJSON *list = o ? cJSON_GetObjectItem(o, "apps") : NULL;
    cJSON_ArrayForEach(v, list) {
        if (S.recents.n >= MAX_RUNNING) break;
        int i = S.recents.n++;
        memset(&S.recents.c[i], 0, sizeof S.recents.c[i]);
        copy_id(S.recents.c[i].id, jstr(v, "id"));
        S.recents.c[i].name = text_render(S.f_semi, 30, jstr(v, "title"), SW_W, &S.recents.c[i].nw, &S.recents.c[i].nh);
        S.recents.c[i].x = i + 0.5f; /* they slide in from the right */
    }
    cJSON_Delete(o);
}

void sw_ok(void)
{
    if (S.recents.zooming || !S.recents.n) return;
    if (S.recents.close_all) {
        close_apps(NULL);
        for (int i = 0; i < S.recents.n; i++) {
            S.recents.c[i].flying = 1;
            S.recents.c[i].fly = -0.12f * abs(i - S.recents.index); /* one after another, outwards */
        }
        S.recents.close_all = 0;
        return;
    }
    if (S.recents.c[S.recents.index].flying) return;
    S.recents.zooming = 1; /* the card grows to fill the screen while the app opens */
    S.recents.zoom_at = SDL_GetTicks();
    launch(S.recents.c[S.recents.index].id, NULL);
}

void sw_update(float dt, int *anim)
{
    spring(&S.recents.t, &S.recents.tv, S.recents.open ? 1.0f : 0.0f, S.recents.open ? 0.42f : 0.3f, S.recents.open ? 0.86f : 1.0f, dt, anim);
    if (!S.recents.open && S.recents.t < 0.004f) {
        if (S.recents.n) sw_clear();
        return;
    }
    for (int i = 0; i < S.recents.n; i++) {
        spring(&S.recents.c[i].x, &S.recents.c[i].xv, (float)(i - S.recents.index), 0.4f, 0.86f, dt, anim);
        spring(&S.recents.c[i].s, &S.recents.c[i].sv, i == S.recents.index && !S.recents.close_all ? 1.0f : 0.0f, 0.35f, 0.75f, dt, anim);
        if (S.recents.c[i].flying) spring(&S.recents.c[i].fly, &S.recents.c[i].flyv, 1.2f, 0.35f, 1.0f, dt, anim);
    }
    /* a card that flew off is gone; the others close the gap on their springs */
    for (int i = 0; i < S.recents.n; i++) {
        if (S.recents.c[i].fly < 1.0f) continue;
        tex_free(&S.recents.c[i].name);
        memmove(&S.recents.c[i], &S.recents.c[i + 1], (size_t)(S.recents.n - i - 1) * sizeof S.recents.c[0]);
        S.recents.n--;
        if (S.recents.index > i) S.recents.index--;
        if (S.recents.index >= S.recents.n) S.recents.index = S.recents.n > 0 ? S.recents.n - 1 : 0;
        if (!S.recents.n) S.recents.empty_at = SDL_GetTicks();
        i--;
        *anim = 1;
    }
    Uint32 now = SDL_GetTicks();
    if (S.recents.open && S.recents.empty_at && now - S.recents.empty_at > 500) S.recents.open = 0; /* nothing left to show */
    if (S.recents.zooming) {
        spring(&S.recents.zoom, &S.recents.zoomv, 1.0f, 0.45f, 0.95f, dt, anim);
        if (now - S.recents.zoom_at > 10000) { /* the app never came up */
            sw_reset();
            toast("That app is taking a while to open", 3500);
        }
    }
    if (S.recents.loading) *anim = 1; /* its placeholders shimmer */
}

/* Left/Right pick an app, Up closes it (its card flies away), Down reaches Close All */
void sw_key(SDL_Keycode k)
{
    if (S.recents.zooming) return;
    if (is_back(k) || k == WEBOS_KEY_RED) {
        S.recents.open = 0; /* animates out; sw_update() clears it */
        return;
    }
    if (!S.recents.n) return;
    if (S.recents.close_all) {
        if (k == SDLK_UP) S.recents.close_all = 0;
        return;
    }
    if (k == SDLK_LEFT && S.recents.index > 0) S.recents.index--;
    else if (k == SDLK_RIGHT && S.recents.index < S.recents.n - 1) S.recents.index++;
    else if (k == SDLK_DOWN) S.recents.close_all = 1;
    else if (k == SDLK_UP && !S.recents.c[S.recents.index].flying) {
        S.recents.c[S.recents.index].flying = 1;
        close_apps(S.recents.c[S.recents.index].id);
    }
}

/* One App Switcher card: the app's tile, large, with its name above (tvOS). */
static void draw_switcher_card(int i, float t)
{
    __typeof__(S.recents.c[0]) *c = &S.recents.c[i];
    App *a = app_by_id(c->id);
    float fly = fminf(fmaxf(c->fly, 0), 1);
    float sc = (0.84f + 0.16f * c->s) * (0.92f + 0.08f * t);
    float w = SW_W * sc, h = SW_H * sc, rad = TILE_R * w / TILE_W;
    float x = W / 2.0f + c->x * SW_PITCH - w / 2, y = SW_CY + 40 * (1 - t) - fly * 760 - h / 2;
    float alpha = t * (1 - fly) * (0.75f + 0.25f * c->s);
    int zoom = S.recents.zooming && i == S.recents.index;
    if (zoom) { /* it grows to fill the screen while the app opens */
        float z = fminf(fmaxf(S.recents.zoom, 0), 1);
        x -= x * z;
        y -= y * z;
        w += (W - w) * z;
        h += (H - h) * z;
        rad *= 1 - z;
        alpha = t;
    }
    if (alpha < 0.004f || x > W || x + w < 0) return;
    Uint8 al = (Uint8)(255 * alpha);
    if (a) {
        draw_big_tile(a, x, y, w, h, rad, al, c->s > 0.01f && !zoom, 0);
    } else {
        r_shape(x, y, w, h, rad, 58, 58, 64, al, 0, 1, 1);
        draw_text(c->name, c->nw, c->nh, x + (w - c->nw) / 2, y + (h - c->nh) / 2, al);
    }
    if (!zoom) draw_text(c->name, c->nw, c->nh, x + 4, y - 18 - c->nh, (Uint8)(al * 0.9f));
}

void draw_switcher(void)
{
    float t = fminf(fmaxf(S.recents.t, 0), 1);
    if (t < 0.004f) return;
    r_backdrop_image(1, (Uint8)(255 * t)); /* the home screen behind, blurred */
    r_fill(0, 0, 0, (Uint8)(110 * t));
    Uint8 ca = (Uint8)(255 * t);
    if (!S.recents.n && S.recents.loading) { /* placeholders where the cards go */
        for (int k = -1; k <= 1; k++) {
            float sc = k ? 0.84f : 1, w = SW_W * sc, h = SW_H * sc, x = W / 2.0f + k * SW_PITCH - w / 2;
            float y = SW_CY + 40 * (1 - t) - h / 2, a = t * (k ? 0.75f : 1);
            draw_skeleton(x, y, w, h, TILE_R * w / TILE_W, a);
            draw_skeleton(x + 4, y - 44, 170 * sc, 22, 11, a);
        }
        return;
    }
    if (!S.recents.n) {
        static Tex *cap[2];
        static int cw[2], ch[2];
        static const char *const text[2] = { "No apps running", "Couldn\xE2\x80\x99t load recent apps" };
        int which = S.recents.failed;
        if (!cap[which]) cap[which] = text_render(S.f_medium, HINT_PX, text[which], 0, &cw[which], &ch[which]);
        draw_text(cap[which], cw[which], ch[which], (W - cw[which]) / 2.0f, SW_CY - ch[which] / 2.0f, (Uint8)(TEXT_SECONDARY * ca / 255));
        return;
    }
    for (int i = 0; i < S.recents.n; i++)
        if (i != S.recents.index) draw_switcher_card(i, t);
    draw_switcher_card(S.recents.index, t); /* the focused card on top */
    if (S.recents.zooming) return;
    static Tex *hint, *close_all;
    static int hw, hh, cw, ch;
    if (!hint) hint = text_render(S.f_medium, HINT_PX, "OK opens  \xC2\xB7  Up closes", 0, &hw, &hh);
    if (!close_all) close_all = text_render(S.f_medium, SHEET_BUTTON_PX, "Close All", 0, &cw, &ch);
    draw_text(hint, hw, hh, (W - hw) / 2.0f, SW_CY + SW_H / 2.0f + 48, (Uint8)(TEXT_SECONDARY * ca / 255));
    SheetItem it = { 0 };
    it.tex = close_all;
    it.w = cw;
    it.h = ch;
    it.destructive = 1;
    draw_sheet_button(&it, S.recents.close_all, (W - 360) / 2.0f, H - SAFE_Y - SHEET_BH, 360, 1, ca);
}
