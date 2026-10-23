/* Home+ native: drawing what's shared: text, glass panels, tiles' faces, skeletons; the home screen itself. */
#include <math.h>
#include <stdio.h>
#include <time.h>

#include "home.h"

/* ------------------------------------------------------------------ animation */

/* OLED protection: after a while without input the screen dims (all but the
 * focused tile, like tvOS), then fades to black and stops drawing. */

void update_clock(void)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    int m = t->tm_hour * 60 + t->tm_min;
    if (m == S.clock_min && S.clock_tex) return;
    char buf[16];
    int h12 = t->tm_hour % 12 ? t->tm_hour % 12 : 12;
    if (S.clock24) snprintf(buf, sizeof buf, "%02d:%02d", t->tm_hour, t->tm_min);
    else snprintf(buf, sizeof buf, "%d:%02d %s", h12, t->tm_min, t->tm_hour < 12 ? "AM" : "PM");
    tex_free(&S.clock_tex);
    S.clock_tex = text_render(S.f_semi, HEADING_PX, buf, 0, &S.clock_w, &S.clock_h);
    S.clock_min = m;
}

/* ------------------------------------------------------------------ drawing */

void draw_text(Tex *t, int w, int h, float x, float y, Uint8 a)
{
    r_image(t, x, y, (float)w, (float)h, 255, 255, 255, a, 0);
}

/* offset of the focused element while it sways after focus lands */
void sway(float *dx, float *dy)
{
    float amp = S.sway_t < 1.6f ? 1 - S.sway_t / 1.6f : 0;
    amp *= amp;
    *dx = 3.0f * amp * sinf(S.sway_t * 2 * (float)M_PI / 1.1f);
    *dy = 1.5f * amp * sinf(S.sway_t * 2 * (float)M_PI / 0.9f + 1);
}

/* Liquid Glass (render.h): clear unless what's on it must stay legible. In the
 * backdrop pass glass can't sample itself, so a faint flat panel stands in. */
/* Glass, or with Reduce Transparency a solid dark panel (and a light wash when focused). */
void glass(const Glass *g)
{
    if (!S.reduce_transparency) {
        r_glass(g);
        return;
    }
    float a = g->alpha * (g->lens < 1 ? g->lens : 1);
    r_shape(g->x, g->y, g->w, g->h, g->radius, 38, 38, 44, (Uint8)(245 * a), 0, 1, 1);
    if (g->focus > 0.01f) r_shape(g->x, g->y, g->w, g->h, g->radius, 255, 255, 255, (Uint8)(60 * g->focus * a), 0, 1, 1);
    r_shape(g->x, g->y, g->w, g->h, g->radius, 255, 255, 255, (Uint8)(50 * a), 1.5f, 1, 1); /* a hairline edge */
}

/* A shadow under glass at full strength: only what shows around it (Reduce Transparency's panel lets a little through) */
void glass_shadow(float x, float y, float w, float h, float radius, float blur, float dy, float alpha)
{
    if (S.reduce_transparency) r_shadow(x, y, w, h, radius, blur, dy, alpha);
    else r_shadow_under(x, y, w, h, radius, blur, dy, alpha);
}

void panel(int backdrop, float x, float y, float w, float h, float radius, float legible, float focus, float alpha)
{
    if (alpha <= 0.004f) return;
    if (backdrop) {
        r_shape(x, y, w, h, radius, 255, 255, 255, (Uint8)(14 * alpha), 0, 1, 1);
        return;
    }
    int lit = focus > 0.01f; /* focused glass catches the moving light */
    Glass g = { x, y, w, h, radius, legible, 1.0f, lit ? S.light_x : -0.6f, lit ? S.light_y : -0.8f, focus, alpha, 0, 0, 0, 0 };
    glass(&g);
}

/* text is made when first shown: a big title is ~0.4 MB of GPU memory */
static void ensure_label(App *a)
{
    if (a->label) return;
    char name[ID_LEN];
    short_title(a, name, sizeof name);
    a->label = text_render(S.f_medium, 26, name, TILE_W + 60, &a->lw, &a->lh);
}

void ensure_big(App *a)
{
    if (a->big) return;
    char name[ID_LEN];
    short_title(a, name, sizeof name);
    a->big = text_render(S.f_bold, 84, name, 1000, &a->bw, &a->bh);
}

/* a colour light enough to read on a dark tile: luminance 0.55 at least */
static void lift(const Uint8 *c, Uint8 *out)
{
    float l = (0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]) / 255, k = l < 0.55f ? 0.55f / fmaxf(l, 0.05f) : 1, v[3];
    for (int i = 0; i < 3; i++) v[i] = fminf(255, c[i] * k);
    float l2 = (0.299f * v[0] + 0.587f * v[1] + 0.114f * v[2]) / 255, w = l2 < 0.55f ? (0.55f - l2) / (1 - l2) : 0;
    for (int i = 0; i < 3; i++) out[i] = (Uint8)(v[i] + (255 - v[i]) * w + 0.5f); /* blue can't get there alone */
}

/* An app's tile and logo in the look Settings picks (Display & OLED -> App Tiles): the icon's own
 * colours; a dark tile with the logo in colour (a light icon's dark ink turned light, a white logo
 * on a coloured icon in that colour); or frosted glass with the logo in white (Clear) or, on glass
 * in a colour, in a pale tint of it (Tinted). lk: the logo's scale. 0 when there's no logo (its name or a placeholder
 * goes there). In the backdrop pass glass can't sample itself: a faint flat tile stands in. on_blur: how far it sits
 * on the blurred, dimmed picture (the grid's, Recents'), which its glass then shows as it is, not sharp and bright. */
static int draw_face(App *a, float x, float y, float w, float h, float rad, float lk, Uint8 alpha, float focus, int backdrop,
                     float on_blur)
{
    int look = S.icon_look, glassy = look == LOOK_CLEAR || look == LOOK_TINTED;
    const Uint8 *t = TINTS[S.icon_tint].rgb;
    float f = fminf(fmaxf(focus, 0), 1);
    Uint8 r, g, b;
    app_color(a, &r, &g, &b);
    if (look == LOOK_DARK) r = g = 14, b = 16; /* near black: the hairline keeps its shape */
    if (glassy && backdrop) {
        r_shape(x, y, w, h, rad, 255, 255, 255, (Uint8)(24 * alpha / 255), 0, 1, 1);
    } else if (glassy) {
        /* a dark tint keeps a white logo readable over a bright picture; focused, the glass lights up */
        int lit = focus > 0.01f, tinted = look == LOOK_TINTED;
        float k = 0.55f + 0.15f * f;
        Glass gl = { x, y, w, h, rad, 0.7f, 1.0f, lit ? S.light_x : -0.6f, lit ? S.light_y : -0.8f, focus, alpha / 255.0f,
                     (Uint8)(tinted ? t[0] * k : 0), (Uint8)(tinted ? t[1] * k : 0), (Uint8)(tinted ? t[2] * k : 0),
                     (Uint8)(tinted ? 150 : 46 - 26 * f), on_blur, on_blur * BLUR_DIM / 255.0f };
        glass(&gl);
    } else {
        r_shape(x, y, w, h, rad, r, g, b, alpha, 0, 1, 1);
        if (0.299f * r + 0.587f * g + 0.114f * b < 32) /* dark tiles keep their shape */
            r_shape(x, y, w, h, rad, 255, 255, 255, (Uint8)(26 * alpha / 255), 1.5f, 1, 1);
    }
    if (!a->logo_tex) return 0;
    if (glassy && backdrop) return 1; /* glass would refract its own logo, as a halo */
    float lw = a->logo_w * lk, lh = a->logo_h * lk, lx = x + (w - lw) / 2, ly = y + (h - lh) / 2, lr = a->logo_r * lk;
    if (look == LOOK_DEFAULT || (look == LOOK_DARK && lr > 0)) { /* a picture keeps its colours */
        r_image(a->logo_tex, lx, ly, lw, lh, 255, 255, 255, alpha, lr);
    } else if (look == LOOK_DARK) {
        Uint8 c[3];
        lift(a->col, c);
        LogoLook l = { 0, a->ink_ref, 0, 0, a->ink_tint, c[0], c[1], c[2] };
        r_logo(a->logo_tex, lx, ly, lw, lh, lr, &l, alpha);
    } else {
        Uint8 c[3] = { 255, 255, 255 };
        if (look == LOOK_TINTED)
            for (int i = 0; i < 3; i++) c[i] = (Uint8)(t[i] + (255 - t[i]) * (0.6f + 0.4f * f)); /* a pale tint, white focused */
        LogoLook shade = { 1, a->ink_ref, a->ink_span, lr > 0 ? 0 : 0.45f, 0, 0, 0, 0 };
        LogoLook l = { 1, a->ink_ref, a->ink_span, lr > 0 ? 0 : 0.45f, 0, c[0], c[1], c[2] };
        r_logo(a->logo_tex, lx, ly + 2 * lk, lw, lh, lr, &shade, (Uint8)(alpha * 0.3f)); /* lifts it off the glass */
        r_logo(a->logo_tex, lx, ly, lw, lh, lr, &l, (Uint8)(alpha * (0.92f + 0.08f * f)));
    }
    return 1;
}

/* A placeholder while something loads (a skeleton): a soft bar that a slow shimmer passes over. */
void draw_skeleton(float x, float y, float w, float h, float radius, float alpha)
{
    float wave = S.reduce_motion ? 0.5f : 0.5f + 0.5f * sinf(2 * (float)M_PI * (SDL_GetTicks() / 1400.0f - (x + 0.5f * y) / 1800.0f));
    r_shape(x, y, w, h, radius, 255, 255, 255, (Uint8)(255 * alpha * (0.08f + 0.08f * wave)), 0, 1, 1);
}

/* darkens where text sits: the bottom (dock), the left (titles), the top (top bar) */
void draw_scrims(Uint8 alpha)
{
    r_image(S.scrim_v, 0, H * 0.55f, W, H * 0.45f, 255, 255, 255, alpha, 0);
    r_image(S.scrim_h, 0, 0, 1150, H, 255, 255, 255, alpha, 0);
    r_image(S.scrim_top, 0, 0, W, 280, 255, 255, 255, alpha, 0);
}

/* An app's tile drawn large: the Top Shelf's big tile and the App Switcher's
 * cards. Its logo grows with it (up to 3.5x); a tile without one shows its name. */
void draw_big_tile(App *a, float x, float y, float w, float h, float radius, Uint8 alpha, int shadow, int backdrop, float on_blur)
{
    float k = fminf(fminf(w / TILE_W, h / TILE_H), 3.5f);
    if (shadow) r_shadow(x, y, w, h, radius, 40, 20, 0.45f * alpha / 255);
    if (draw_face(a, x, y, w, h, radius, k, alpha, 0, backdrop, on_blur)) return;
    ensure_big(a);
    if (!a->big) return;
    float f = fminf(fminf(1, w * 0.8f / a->bw), k / 2.5f), bw = a->bw * f, bh = a->bh * f;
    r_image(a->big, x + (w - bw) / 2, y + (h - bh) / 2, bw, bh, 255, 255, 255, alpha, 0);
}

/* The clock capsule, like tvOS 26's: the time, and at its end a circle (the Control
 * Center's glyph, where tvOS shows the profile picture). The whole capsule is the
 * button: focused, it lifts and turns into a white platter. With the Control Center
 * open, it's tvOS's header: the capsule's glass goes, the time stays on its own, and
 * the circle grows into the selected tab, solid white with the glyph dark. */
static void draw_clock(Uint8 alpha, int backdrop)
{
    if (!alpha) return;
    float k = S.top_s[TOP_SETTINGS], g = fminf(fmaxf(k, 0), 1), sc = 1 + 0.08f * k;
    float w0 = top_w(), pw = w0 * sc, ph = BAR * sc, cx = top_x() + w0 / 2, cy = TOP_Y + BAR / 2.0f;
    if (k > 0.01f) {
        float dx, dy;
        sway(&dx, &dy);
        cx += dx * k;
        cy += dy * k;
    }
    float x = cx - pw / 2, y = cy - ph / 2, o = fminf(fmaxf(S.cc.t, 0), 1) * (1 - g);
    if (k > 0.01f && !backdrop) r_shadow(x, y, pw, ph, ph / 2, 16, 6, 0.35f * k * alpha / 255.0f);
    panel(backdrop, x, y, pw, ph, ph / 2, 1, k, alpha / 255.0f * (1 - o));
    if (g > 0.01f) /* focused: an opaque white platter with dark contents, like the sheet buttons */
        r_shape(x, y, pw, ph, ph / 2, 245, 245, 247, (Uint8)(alpha * g), 0, 1, 1);
    Uint8 tc = (Uint8)(255 - 227 * g); /* the time stays its own size, so it stays sharp */
    r_image(S.clock_tex, x + 24 * sc, cy - S.clock_h / 2.0f, S.clock_w, S.clock_h, tc, tc, (Uint8)(255 - 225 * g),
            (Uint8)(alpha * (TEXT_STATIC + (255 - TEXT_STATIC) * g) / 255), 0);
    float dc = x + pw - 5 * sc - TOP_DOT * sc / 2; /* the circle's centre: it grows around it */
    float d = (TOP_DOT + (BAR + 2 - TOP_DOT) * o) * sc, ox = dc - d / 2, oy = cy - d / 2;
    if (o > 0.01f && !backdrop) r_shadow(ox, oy, d, d, d / 2, 14, 5, 0.3f * o * alpha / 255.0f);
    r_shape(ox, oy, d, d, d / 2, 255, 255, 255, (Uint8)(alpha * (0.16f * (1 - g) * (1 - o) + 0.97f * o)), 0, 1, 1);
    if (g > 0.01f) r_shape(ox, oy, d, d, d / 2, 0, 0, 0, (Uint8)(alpha * 0.07f * g), 0, 1, 1);
    float is = S.top_w[TOP_SETTINGS] * sc * (1 + 0.2f * o), gk = fmaxf(g, o);
    Uint8 gc = (Uint8)(255 - 227 * gk);
    r_image(S.top_tex[TOP_SETTINGS], ox + (d - is) / 2, oy + (d - is) / 2, is, is, gc, gc, (Uint8)(255 - 225 * gk),
            (Uint8)(alpha * (0.85f + 0.15f * gk)), 0);
}

/* item i's tile is on screen (draw_tile draws it) */
static int tile_on_screen(int i)
{
    float by = GRID_TOP + (i / COLS) * ROW_H + S.scroll;
    return !(by > H + 40 || by + ROW_H < -40);
}

/* App i is on show, or may be (what's over the grid shows apps too; the Top Shelf, its cover or tile):
 * something new of it redraws. */
int app_shown(int i)
{
    if (S.recents.t > 0.004f || S.sheet.open || S.ghost.on || S.cc.t > 0.004f || S.moving || S.pic_a < 1) return 1;
    const App *a = &S.apps[i];
    if (a == item_app(S.shelf_cur) || a == item_app(S.focus)) return 1;
    for (int k = 0; k < S.nitems; k++)
        if (S.items[k] == i) return tile_on_screen(k);
    return 0;
}

static void draw_tile(int i, int backdrop)
{
    App *a = item_app(i);
    float bx = LEFT + (i % COLS) * (TILE_W + GAP_X);
    float by = GRID_TOP + (i / COLS) * ROW_H + S.scroll;
    if (!tile_on_screen(i)) return;
    int focused = i == S.focus && S.zone == ZONE_GRID;
    float k = a ? a->s : S.special_s;
    float sc = 1 + (FOCUS_SCALE - 1) * k;
    int launching = S.launching && i == S.launch_item;
    if (launching) { /* opening: breathes a little below its focus size for 2 s, then holds */
        float lt = (SDL_GetTicks() - S.launch_at) / 1000.0f;
        k = 1;
        sc = lt < 2 ? FOCUS_SCALE - 0.015f + 0.015f * cosf(lt * 2 * (float)M_PI * 1.25f) : FOCUS_SCALE;
    }
    float w = TILE_W * sc, h = TILE_H * sc, rad = TILE_R * sc;
    float x = bx - (w - TILE_W) / 2, y = by - (h - TILE_H) / 2 - 6 * k;
    if (S.moving && focused && !S.reduce_motion) x += sinf(SDL_GetTicks() / 40.0f) * 2.0f; /* gentle wobble while moving */
    if (k > 0.01f) {
        float dx, dy;
        sway(&dx, &dy);
        x += dx * k;
        y += dy * k;
    }
    if (k > 0.01f && !backdrop) { /* an app's face covers its tile: opaque, or glass at full strength */
        if (a) glass_shadow(x, y, w, h, rad, 28, 16, 0.5f * k);
        else r_shadow(x, y, w, h, rad, 28, 16, 0.5f * k);
    }
    if (!a) {
        static Tex *ht;
        static int hw, hh;
        r_shape(x, y, w, h, rad, 70, 70, 82, 34, 0, 1, 1);
        if (!ht) ht = text_render(S.f_semi, 28, "\xE2\x80\xA2 \xE2\x80\xA2 \xE2\x80\xA2   Hidden", 0, &hw, &hh);
        draw_text(ht, hw, hh, x + (w - hw) / 2, y + (h - hh) / 2, 205);
    } else if (draw_face(a, x, y, w, h, rad, sc, 255, k, backdrop, S.scrolled)) { /* opaque, the icon's own colour (or a look's) */
    } else if (a->icon_state == 1) { /* its icon is on its way: a placeholder */
        float bw = w * 0.46f, bh = h * 0.15f;
        draw_skeleton(x + (w - bw) / 2, y + (h - bh) / 2, bw, bh, bh / 2, 1);
    } else { /* no icon: its name, kept inside the tile */
        ensure_label(a);
        if (a->label) {
            float k = fminf(1, (w - 32) / a->lw), lw = a->lw * k, lh = a->lh * k;
            r_image(a->label, x + (w - lw) / 2, y + (h - lh) / 2, lw, lh, 255, 255, 255, 230, 0);
        }
    }
    if (a && k > 0.01f && !backdrop && (S.icon_look == LOOK_DEFAULT || S.icon_look == LOOK_DARK))
        r_sheen(x, y, w, h, rad, S.light_x, S.light_y, k); /* focused, it catches the moving light as glass does */
    if (S.moving && focused) r_shape(x, y, w, h, rad, 255, 255, 255, 200, 2.0f, 1, 1); /* the tile you're moving */
    if (S.zone == ZONE_TOP && i == top_down_tile() && !backdrop) /* where Down from the top bar lands */
        r_shape(x, y, w, h, rad, 255, 255, 255, 128, 2.0f, 1, 1);
    if (focused && S.ok_down && !S.ok_long && !S.sheet.open && !S.recents.open && !S.moving && !backdrop) {
        float p = (SDL_GetTicks() - S.ok_down - 250) / (float)(LONG_PRESS_MS - 250);
        if (p > 0) { /* holding OK: a bar fills until the options open */
            float hw = w * 0.5f, hx = x + (w - hw) / 2, hy = y + h + 10;
            r_shape(hx, hy, hw, 4, 2, 255, 255, 255, 60, 0, 1, 1);
            r_shape(hx, hy, hw * fminf(p, 1), 4, 2, 255, 255, 255, 230, 0, 1, 1);
        }
    }
    if (a && k > 0.01f && !backdrop && (i >= COLS || launching)) { /* like tvOS, the top row has no names */
        ensure_label(a);
        float ly = i < COLS ? DOCK_Y + S.scroll + DOCK_H + 6 : by + TILE_H + 26; /* "Opening…" under the dock */
        Tex *lt = launching && S.launch_tex ? S.launch_tex : a->label;
        int lw = lt == a->label ? a->lw : S.launch_w, lh = lt == a->label ? a->lh : S.launch_h;
        draw_text(lt, lw, lh, bx + (TILE_W - lw) / 2, ly, (Uint8)(TEXT_STATIC * fminf(k, 1)));
    }
}

/* Everything below the overlays. backdrop: the copy the glass refracts. */
static void draw_scene(int backdrop)
{
    Uint8 shelf_a = (Uint8)(255 * (1 - S.scrolled));
    /* The backdrop (what glass refracts, and what's blurred below the dock) keeps the
     * picture at full strength and never holds its own blur: blurring last frame's blur
     * smeared it wider every frame. */
    draw_shelf_art(backdrop ? 255 : shelf_a, backdrop);
    if (S.scrolled > 0.004f && !backdrop) { /* below the dock the picture stays, blurred and dimmed, so the apps stand out */
        r_backdrop_image(1, (Uint8)(255 * S.scrolled), (Uint8)(BLUR_DIM * S.scrolled));
    }
    /* the shelf's content leaves faster than the grid arrives, drifting up behind it */
    r_translate(0, 0.35f * S.scroll);
    draw_shelf_content((Uint8)(255 * fmaxf(0, 1 - 2.5f * S.scrolled)), backdrop);
    r_translate(0, 0);
    /* Scrolling down, the dock's glass grows to the screen's edges and frosts over: it
     * becomes the blurred background the grid sits on, crossfading into it at the end.
     * (Its stand-in in the backdrop stays out meanwhile: blurred, it made a white band.) */
    float dock_y = DOCK_Y + S.scroll, dock_a = 1 - S.scrolled, t = smooth01(0, 1, S.scrolled);
    if (t < 0.999f) {
        float gx = DOCK_X * (1 - t), gy = dock_y * (1 - t), gw = W - 2 * gx, gh = dock_y + DOCK_H + (H - dock_y - DOCK_H) * t - gy;
        float rad = (TILE_R + PAD) * (1 - t), ga = t < 0.75f ? 1 : 1 - (t - 0.75f) / 0.25f;
        if (!backdrop && t <= 0.004f) glass_shadow(gx, gy, gw, gh, rad, 40, 14, 0.3f); /* only at rest: it'd be a big moving blur */
        /* clear while it's the dock (its icons are opaque), frosted as soon as it moves: growing
         * towards the whole screen, the clear glass's sharp, colour-split edge (four texture
         * reads a pixel against one) dropped frames on the TV */
        if (!backdrop || t <= 0.004f) panel(backdrop, gx, gy, gw, gh, rad, smooth01(0, 0.15f, t), 0, ga);
    }
    if (!backdrop && S.chevron_a > 0.004f && dock_a > 0.004f) { /* tvOS's chevron, 30 px above the dock */
        float cs = 56, cx = (W - cs) / 2, cy = dock_y - 30 - cs / 2;
        Uint8 ca = (Uint8)(S.chevron_a * dock_a * 255);
        r_image(S.chevron, cx, cy + 2, cs, cs, 0, 0, 0, (Uint8)(ca * 0.35f), 0); /* a soft shadow, for bright pictures */
        r_image(S.chevron, cx, cy, cs, cs, 255, 255, 255, (Uint8)(ca * 0.7f), 0);
    }
    int dimmed = !backdrop && S.dim > 0.004f, top = S.zone == ZONE_TOP;
    if (!(dimmed && top)) draw_clock((Uint8)(255 * (1 - S.scrolled)), backdrop);
    if (apps_loading()) /* the apps are on their way: placeholders in the dock */
        for (int c = 0; c < COLS; c++) draw_skeleton(LEFT + c * (TILE_W + GAP_X), GRID_TOP + S.scroll, TILE_W, TILE_H, TILE_R, 1);
    for (int i = 0; i < S.nitems; i++)
        if (i != S.focus) draw_tile(i, backdrop);
    if (dimmed) r_fill(0, 0, 0, (Uint8)(S.dim * 115)); /* OLED: idle dims all but what has focus */
    if (S.launching && !backdrop) { /* the rest steps back while the app opens */
        float d = (SDL_GetTicks() - S.launch_at - 250) / 200.0f;
        if (d > 0) r_fill(0, 0, 0, (Uint8)(77 * fminf(d, 1)));
    }
    if (S.focus < S.nitems) draw_tile(S.focus, backdrop);
    if (dimmed && top) draw_clock((Uint8)(255 * (1 - S.scrolled)), backdrop);
    if (S.moving) { /* how to finish, in a capsule at the top while an app is being moved */
        static Tex *hint;
        static int hw, hh;
        if (!hint) hint = text_render(S.f_medium, HINT_PX, "Arrows move it  \xC2\xB7  OK puts it down", 0, &hw, &hh);
        float pw = hw + 56, px = (W - pw) / 2;
        panel(backdrop, px, SAFE_Y, pw, BAR, BAR / 2, 1, 0, 1);
        draw_text(hint, hw, hh, px + 28, SAFE_Y + (BAR - hh) / 2.0f, TEXT_STATIC);
    }
}

/* The Control Center: tvOS's glass tiles under the clock, growing out of it one after
 * another; the focused one turns white, like every focused button. Round buttons have
 * only their icon: the focused one's name shows under the grid. */
/* Text in a room it may not fit, like tvOS's: what doesn't fit fades out at the room's edge,
 * and from `since` (0: never) it waits 2 s, then slides left to show the rest, a gap, then
 * the start again. Sets S.cc.marquee while it moves. */
void draw_marquee(Tex *t, int tw, int th, float x, float y, float room, Uint8 c, Uint8 a, Uint32 since)
{
    if (!t || !a) return;
    if (tw <= room) {
        r_image(t, x, y, tw, th, c, c, c, a, 0);
        return;
    }
    const float gap = 40, speed = 36, rest = 2.0f, fade = 16; /* px, px/s, s, px */
    float period = tw + gap, off = 0;
    if (since && !S.reduce_motion) {
        float cyc = rest + period / speed, ts = fmodf((SDL_GetTicks() - since) / 1000.0f, cyc);
        off = ts > rest ? (ts - rest) * speed : 0;
        S.cc.marquee = 1;
    }
    /* the strip is the text, a gap and the text again; [s0, s1] of it is drawn at x + s0 - off */
    for (int k = 0; k < 2; k++) {
        float base = k * period, s0 = fmaxf(off, base), s1 = fminf(off + room, base + tw);
        if (s1 <= s0) continue;
        /* the middle at full strength, the edges in thin slices that fade (the left one only
         * while the text has moved) */
        float lo = off > 0.5f ? off + fade : off, hi = off + room - fade;
        for (float u = s0; u < s1;) {
            float e = u < lo ? fminf(fminf(u + 2, lo), s1) : u < hi ? fminf(hi, s1) : fminf(u + 2, s1);
            float m = (u + e) / 2, f = m < lo ? (m - off) / fade : m > hi ? (off + room - m) / fade : 1;
            r_image_part(t, x + u - off, y, e - u, th, (u - base) / tw, (e - base) / tw, c, c, c, (Uint8)(a * fmaxf(0, fminf(1, f))));
            u = e;
        }
    }
}

static void draw_toast(void)
{
    float t = S.toast_t < 0 ? 0 : S.toast_t > 1 ? 1 : S.toast_t;
    if (!S.toast_tex || t < 0.01f) return;
    float tw = S.toast_w + 72, th = S.toast_h + 40, tx = (W - tw) / 2, ty = H - SAFE_Y - th + (1 - S.toast_t) * 40;
    r_shadow(tx, ty, tw, th, th / 2, 30, 12, 0.4f * t);
    Glass g = { tx, ty, tw, th, th / 2, 1, t, -0.6f, -0.8f, 0, t, 0, 0, 0, 0 };
    glass(&g);
    draw_text(S.toast_tex, S.toast_w, S.toast_h, (W - S.toast_w) / 2, ty + 20, (Uint8)(245 * t));
}

extern int homeplus_screenshot(const char *path);

/* What a backdrop holds: 0 the Top Shelf picture alone, what the dock and top bar refract (the icons
 * sit on the glass), and the blurred background below the dock; 1 the whole scene, what overlays refract */
static void backdrop_content(int which)
{
    if (which == 0) {
        draw_shelf_art(255, 1);
        return;
    }
    draw_scene(1);
    if (cc_dim() > 0.002f) r_fill(0, 0, 0, (Uint8)(255 * cc_dim())); /* (what the glass refracts steps back too) */
}

/* A backdrop is drawn (and blurred) only when what it holds would change: noted first, drawn if the
 * fingerprint differs. A sheet, Recents or the Control Center moving over a still scene, or the grid
 * under a picture that stays, no longer redraws and blurs both backdrops every frame. */
static void draw_backdrop(int which)
{
    r_backdrop_note();
    backdrop_content(which);
    if (r_backdrop_unchanged(which)) return;
    r_backdrop_begin(which);
    backdrop_content(which);
    r_backdrop_end();
}

/* Back from another app, the home screen comes into focus, as tvOS's does: its blurred, dimmed copy over it
 * clears. A gentle ease out (quadratic): a cubic one cleared most of it in its first fifth, and felt like a cut.
 * 1: in focus. */
static float back_in(void)
{
    if (!S.back_at) return 1;
    float t = (SDL_GetTicks() - S.back_at) / (float)BACK_MS;
    if (t >= 1) {
        S.back_at = 0;
        return 1;
    }
    return 1 - (1 - t) * (1 - t);
}

int render(float dt, const char *shot)
{
    perf_begin(dt);
    int anim = update(dt);
    perf_updated();
    float in = back_in();
    draw_backdrop(0);
    perf_probe(PROBE_ART);
    if (S.sheet.open || S.ghost.on || S.toast_t > 0.01f || S.recents.t > 0.004f || S.cc.t > 0.004f || in < 1) {
        /* overlays refract the whole scene (and it's what comes into focus) */
        draw_backdrop(1);
        perf_probe(PROBE_BACKDROP);
    }

    r_frame_begin(8, 8, 11);
    perf_probe(PROBE_SCREEN);
    r_use_backdrop(0);
    if (S.recents.t < 1) draw_scene(0); /* (open, Recents covers it) */
    perf_probe(PROBE_SCENE);
    r_use_backdrop(1);
    draw_switcher();
    draw_control_center();
    draw_ghost();
    draw_sheet();
    draw_toast();
    if (in < 1) r_backdrop_image(1, (Uint8)(255 * (1 - in)), (Uint8)(BLUR_DIM * (1 - in)));
    if (S.black > 0.004f) r_fill(0, 0, 0, (Uint8)(S.black * 255)); /* OLED: idle fades to black */
    perf_probe(PROBE_OVERLAYS);
    if (shot) homeplus_screenshot(shot);
    perf_drawn();
    r_present();
    perf_end();
    return anim;
}
