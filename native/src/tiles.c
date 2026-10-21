/* Home+ native: uniform tiles: an icon's logo cut out of its background, and the cover made from it. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "home.h"

/* ------------------------------------------------------------------ uniform tiles */

/* Like tvOS app icons, every tile is the icon's own background colour from edge
 * to edge with its logo centred at the same visual size. LG icons are small
 * squares with their own background, so: find that colour on the border, key
 * it out, trim to the logo and size it to a common target. Icons without a
 * solid background keep their whole picture, at one size, with round corners;
 * logo-only icons (transparent) get one graphite tile (light if the logo is dark). */

/* A cover for apps without artwork: the icon stretched over a small 16:9
 * frame (over the app's colour where it's transparent), blurred into soft
 * colour fields, darkened towards the edges. Drawn full screen, the GPU's
 * filtering smooths it further. */
static void make_cover(App *a, const unsigned char *px, int w, int h)
{
    enum { CW = 160, CH = 90, PASSES = 3, R = 7 };
    float *f = malloc(sizeof(float) * CW * CH * 3), *t = malloc(sizeof(float) * CW * CH * 3);
    unsigned char *out = malloc((size_t)CW * CH * 4);
    if (!f || !t || !out) {
        free(f);
        free(t);
        free(out);
        return;
    }
    float sc = fmaxf((float)CW / w, (float)CH / h);
    for (int y = 0; y < CH; y++) {
        for (int x = 0; x < CW; x++) {
            int sx = (int)((x + 0.5f - CW / 2.0f) / sc + w / 2.0f), sy = (int)((y + 0.5f - CH / 2.0f) / sc + h / 2.0f);
            sx = sx < 0 ? 0 : sx >= w ? w - 1 : sx;
            sy = sy < 0 ? 0 : sy >= h ? h - 1 : sy;
            const unsigned char *p = px + ((size_t)sy * w + sx) * 4;
            float al = p[3] / 255.0f;
            for (int c = 0; c < 3; c++) f[(y * CW + x) * 3 + c] = p[c] * al + a->col[c] * (1 - al);
        }
    }
    for (int pass = 0; pass < PASSES; pass++) { /* box blur, horizontal then vertical */
        for (int y = 0; y < CH; y++)
            for (int x = 0; x < CW; x++)
                for (int c = 0; c < 3; c++) {
                    float sum = 0;
                    for (int k = -R; k <= R; k++) {
                        int xx = x + k < 0 ? 0 : x + k >= CW ? CW - 1 : x + k;
                        sum += f[(y * CW + xx) * 3 + c];
                    }
                    t[(y * CW + x) * 3 + c] = sum / (2 * R + 1);
                }
        for (int y = 0; y < CH; y++)
            for (int x = 0; x < CW; x++)
                for (int c = 0; c < 3; c++) {
                    float sum = 0;
                    for (int k = -R; k <= R; k++) {
                        int yy = y + k < 0 ? 0 : y + k >= CH ? CH - 1 : y + k;
                        sum += t[(yy * CW + x) * 3 + c];
                    }
                    f[(y * CW + x) * 3 + c] = sum / (2 * R + 1);
                }
    }
    for (int y = 0; y < CH; y++) {
        for (int x = 0; x < CW; x++) {
            float dx = (x - CW / 2.0f) / (CW / 2.0f), dy = (y - CH / 2.0f) / (CH / 2.0f);
            /* calm: halfway to grey and dark, so a focused app doesn't flood the room
             * (HyperHDR mirrors the screen into the lights); darker towards the edges */
            float shade = (S.vivid_covers ? 0.82f : 0.45f) * (1 - 0.35f * fminf(1, (dx * dx + dy * dy) * 0.6f));
            const float *pc = &f[(y * CW + x) * 3];
            float luma = 0.299f * pc[0] + 0.587f * pc[1] + 0.114f * pc[2];
            unsigned char *o = out + ((size_t)y * CW + x) * 4;
            for (int c = 0; c < 3; c++) {
                float v = (pc[c] + (luma - pc[c]) * (S.vivid_covers ? 0 : 0.55f)) * shade;
                o[c] = (unsigned char)(v > 255 ? 255 : v);
            }
            o[3] = 255;
        }
    }
    tex_free(&a->cover_tex);
    a->cover_tex = r_tex(out, CW, CH);
    free(f);
    free(t);
    free(out);
}

/* the most (or least) of v within 2 px, across then down: out (tmp is scratch) */
static void spread(const unsigned short *v, unsigned short *tmp, unsigned short *out, int w, int h, int most)
{
    for (int pass = 0; pass < 2; pass++) {
        const unsigned short *from = pass ? tmp : v;
        unsigned short *to = pass ? out : tmp;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int at = pass ? y : x, len = pass ? h : w, step = pass ? w : 1;
                unsigned short r = from[y * w + x];
                for (int d = at > 2 ? -2 : -at; d <= 2 && at + d < len; d++) {
                    unsigned short u = from[y * w + x + d * step];
                    r = most ? (u > r ? u : r) : (u < r ? u : r);
                }
                to[y * w + x] = r;
            }
    }
}

void make_logo(App *a, const unsigned char *px, int w, int h)
{
    /* background: the first opaque pixel (2 px in, past the anti-aliased edge)
     * on 24 lines inward from each side */
    enum { LINES = 24 };
    unsigned char samp[4 * LINES][3];
    int n = 0;
    double m[3] = { 0 };
    for (int side = 0; side < 4; side++) {
        for (int k = 1; k <= LINES; k++) {
            int along = side < 2 ? w : h, depth = side < 2 ? h : w, t = k * along / (LINES + 1);
            for (int d = 0; d < depth / 3; d++) {
                int dd = d;
                int x = side == 0 || side == 1 ? t : side == 2 ? dd : w - 1 - dd;
                int y = side == 0 ? dd : side == 1 ? h - 1 - dd : t;
                if (px[((size_t)y * w + x) * 4 + 3] <= 200) continue;
                dd = d + 2 < depth ? d + 2 : d;
                x = side == 0 || side == 1 ? t : side == 2 ? dd : w - 1 - dd;
                y = side == 0 ? dd : side == 1 ? h - 1 - dd : t;
                const unsigned char *p = px + ((size_t)y * w + x) * 4;
                memcpy(samp[n++], p, 3);
                m[0] += p[0];
                m[1] += p[1];
                m[2] += p[2];
                break;
            }
        }
    }
    enum { SOLID, PICTURE, CLEAR } mode = CLEAR;
    if (n >= 4 * LINES * 3 / 4) {
        double dev = 0;
        for (int c = 0; c < 3; c++) m[c] /= n;
        for (int i = 0; i < n; i++) dev += fabs(samp[i][0] - m[0]) + fabs(samp[i][1] - m[1]) + fabs(samp[i][2] - m[2]);
        mode = dev / n < 24 ? SOLID : PICTURE;
    }

    /* keyed copy: background pixels fade to transparent; then the logo's bounding box */
    unsigned char *k = malloc((size_t)w * h * 4);
    if (!k) return;
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    double lum = 0, lum_n = 0;
    /* for the looks: how far the logo's ink stands out from the icon's background (a logo with
     * none: its own luminance), and how much of it is grey or white */
    float ref = mode == CLEAR ? -1 : (float)((0.299 * m[0] + 0.587 * m[1] + 0.114 * m[2]) / 255);
    int stand[32] = { 0 }, grey = 0, far = 0;
    /* A logo's edge pixels are part background. How far one is from the background, against how far
     * the logo's ink is nearby (the most within 2 px), says how much of it is logo; taking the
     * background's part out of its colour leaves the ink's. On the icon's own colour the tile looks
     * the same; on a look's tile no rim of the old background shows round the logo. */
    unsigned short *dist = mode == SOLID ? malloc(sizeof *dist * (size_t)w * h * 4) : NULL, *ink_near = NULL, *bg_near = NULL;
    unsigned char *key = dist ? malloc((size_t)w * h) : NULL; /* the plain key: the logo's box and size, as before */
    if (dist) {
        unsigned short *tmp = dist + (size_t)w * h;
        ink_near = tmp + (size_t)w * h;
        bg_near = ink_near + (size_t)w * h;
        for (int i = 0; i < w * h; i++) {
            const unsigned char *p = px + (size_t)i * 4;
            dist[i] = (unsigned short)(fabs(p[0] - m[0]) + fabs(p[1] - m[1]) + fabs(p[2] - m[2]));
        }
        spread(dist, tmp, ink_near, w, h, 1);
        spread(dist, tmp, bg_near, w, h, 0); /* only where the background is near: inside, two inks meet */
    }
    for (int i = 0; i < w * h; i++) {
        const unsigned char *p = px + (size_t)i * 4;
        unsigned char *q = k + (size_t)i * 4;
        float alpha = p[3], d = 0, plain;
        memcpy(q, p, 3);
        if (dist && key) {
            float n = ink_near[i], logo = bg_near[i] >= 24 ? 1 : n > 24 ? fminf(1, fmaxf(0, (dist[i] - 12) / (n - 12))) : 0;
            d = dist[i];
            key[i] = (unsigned char)(alpha * smooth01(24, 72, d));
            alpha *= logo * smooth01(24, 72, n);
            if (logo > 0.02f)
                for (int c = 0; c < 3; c++) q[c] = (unsigned char)fmin(255, fmax(0, m[c] + (p[c] - m[c]) / logo + 0.5));
        } else if (mode == SOLID) { /* (no memory for that: a plain key) */
            d = fabsf(p[0] - (float)m[0]) + fabsf(p[1] - (float)m[1]) + fabsf(p[2] - (float)m[2]);
            alpha *= smooth01(24, 72, d);
        }
        q[3] = (unsigned char)alpha;
        plain = key ? key[i] : alpha;
        if (mode == PICTURE ? p[3] > 40 : plain > 40) {
            int x = i % w, y = i / w;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
            float l = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / 255, lq = (0.299f * q[0] + 0.587f * q[1] + 0.114f * q[2]) / 255;
            lum += l;
            lum_n++;
            stand[(int)((ref < 0 ? lq : fabsf(lq - ref)) * 31.99f)]++;
            if (d > 200) { /* the ink itself, not its edge blending into the background */
                int hi = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]);
                int lo = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]);
                far++;
                if (hi - lo < 40) grey++;
            }
        }
    }
    free(dist);
    /* the most it stands out: past all but its 5% that stand out most (a few stray pixels don't count) */
    a->ink_ref = ref;
    a->ink_span = 1;
    for (int b = 31, acc = 0; b >= 0; b--)
        if ((acc += stand[b]) > lum_n / 20) {
            a->ink_span = (b + 1) / 32.0f;
            break;
        }
    int hi = (int)fmax(m[0], fmax(m[1], m[2])), lo = (int)fmin(m[0], fmin(m[1], m[2]));
    a->ink_tint = mode == SOLID && hi - lo > 60 && far && grey > far * 0.85;
    if (x1 < x0) { /* nothing but background: keep the whole icon */
        x0 = y0 = 0;
        x1 = w - 1;
        y1 = h - 1;
        mode = PICTURE;
        a->ink_ref = -1;
        a->ink_span = 1;
    }
    if (mode == PICTURE) {
        memcpy(k, px, (size_t)w * h * 4);
        free(key), key = NULL;
    }
    double ink = 0; /* how much logo there is: sizes logos by what you see, not their box */
    x0 = x0 > 2 ? x0 - 2 : 0;
    y0 = y0 > 2 ? y0 - 2 : 0;
    x1 = x1 < w - 3 ? x1 + 2 : w - 1;
    y1 = y1 < h - 3 ? y1 + 2 : h - 1;
    int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    unsigned char *crop = malloc((size_t)cw * ch * 4);
    if (!crop) {
        free(k);
        free(key);
        return;
    }
    for (int y = 0; y < ch; y++) memcpy(crop + (size_t)y * cw * 4, k + ((size_t)(y0 + y) * w + x0) * 4, (size_t)cw * 4);
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++) ink += (key ? key[(size_t)(y0 + y) * w + x0 + x] : crop[((size_t)y * cw + x) * 4 + 3]) / 255.0;
    free(key);
    free(k);
    tex_free(&a->logo_tex);
    a->logo_tex = r_tex(crop, cw, ch);
    free(crop);

    /* one visual size: about the same area for every logo, within a box; as
     * big as tvOS's glyphs, which fill about 60% of a tile's height */
    if (mode == PICTURE) {
        float side = TILE_H * 0.66f;
        a->logo_w = a->logo_h = side;
        a->logo_r = TILE_R * side / TILE_H; /* same corner-to-size ratio as the tile */
    } else {
        /* the same amount of logo on every tile (a square of about 40% of the
         * tile's height, filled), but never wider or taller than the tile allows */
        float sc = TILE_H * 0.42f / sqrtf((float)(ink > 1 ? ink : 1));
        sc = fminf(sc, fminf(TILE_W * 0.72f / cw, TILE_H * 0.62f / ch));
        sc = fminf(sc, 1.5f); /* LG ships 130 px icons: more than this goes soft */
        a->logo_w = cw * sc;
        a->logo_h = ch * sc;
        a->logo_r = 0;
    }
    /* the tile: the icon's background, a darkened average behind pictures, graphite behind bare logos */
    if (mode == SOLID) {
        for (int c = 0; c < 3; c++) a->col[c] = (Uint8)(m[c] + 0.5);
    } else if (mode == PICTURE) {
        for (int c = 0; c < 3; c++) a->col[c] = (Uint8)(n ? m[c] * 0.55 : 40);
    } else {
        int light = lum_n > 0 && lum / lum_n < 0.25;
        a->col[0] = a->col[1] = light ? 236 : 40;
        a->col[2] = light ? 240 : 46;
    }
    a->has_col = 1;
    a->fullbleed = mode == SOLID;
    make_cover(a, px, w, h);
}
