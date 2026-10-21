/* Home+ native: drawing what's shared: text, glass panels, tiles' faces, skeletons; the home screen itself. */
#include <math.h>

#include "home.h"

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
void ensure_label(App *a)
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

/* An app's tile and logo in the look Settings picks (Display & OLED -> App Icons): the icon's own
 * colours; a dark tile with the logo in colour (a light icon's dark ink turned light, a white logo
 * on a coloured icon in that colour); or frosted glass with the logo in white (Clear) or, on glass
 * in a colour, in a pale tint of it (Tinted). lk: the logo's scale. 0 when there's no logo (its name or a placeholder
 * goes there). In the backdrop pass glass can't sample itself: a faint flat tile stands in. */
int draw_face(App *a, float x, float y, float w, float h, float rad, float lk, Uint8 alpha, float focus, int backdrop)
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
                     (Uint8)(tinted ? 150 : 46 - 26 * f) };
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
