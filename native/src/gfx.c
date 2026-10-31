/* Home+ native: start-up textures and text. See gfx.h. */
#include "gfx.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Signed distance from p to a rounded box with half-extents (bx, by) and corner radius rad. */
static float sd_round_box(float px, float py, float bx, float by, float rad)
{
    float qx = fabsf(px) - bx + rad, qy = fabsf(py) - by + rad;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float inside = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (inside < 0 ? inside : 0) - rad;
}

Tex *gfx_glow(int w, int h)
{
    unsigned char *px = malloc((size_t)w * h * 4);
    if (!px) return NULL;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float dx = (x + 0.5f) / w * 2 - 1, dy = (y + 0.5f) / h * 2 - 1;
            float d = clampf(sqrtf(dx * dx + dy * dy), 0, 1);
            float a = 1 - d;
            a = a * a * (3 - 2 * a); /* smoothstep falloff */
            unsigned char *p = px + ((size_t)y * w + x) * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = (unsigned char)(a * 255.0f);
        }
    }
    Tex *t = r_tex(px, w, h);
    free(px);
    return t;
}

Tex *gfx_gradient(int vertical, float a0, float a1)
{
    enum { N = 256 };
    unsigned char px[N * 4];
    for (int i = 0; i < N; i++) {
        px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = 0;
        float t = i / (float)(N - 1);
        t = t * t * (3 - 2 * t); /* smoothstep: no visible edge where it ends */
        px[i * 4 + 3] = (unsigned char)((a0 + (a1 - a0) * t) * 255.0f);
    }
    return vertical ? r_tex(px, 1, N) : r_tex(px, N, 1);
}

/* ------------------------------------------------------------------ icons */

/* Inside tests in a unit square centred on 0 (x, y in -0.5..0.5, y down). */
static int in_rrect(float x, float y, float cx, float cy, float hw, float hh, float r)
{
    return sd_round_box(x - cx, y - cy, hw, hh, r) <= 0;
}

static int in_ring_rrect(float x, float y, float cx, float cy, float hw, float hh, float r, float t)
{
    float d = sd_round_box(x - cx, y - cy, hw, hh, r);
    return d <= 0 && d > -t;
}

/* distance from (px, py) to the segment a-b */
static float sd_segment(float px, float py, float ax, float ay, float bx, float by)
{
    float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
    float h = (pax * bax + pay * bay) / (bax * bax + bay * bay);
    h = h < 0 ? 0 : h > 1 ? 1 : h;
    float dx = pax - bax * h, dy = pay - bay * h;
    return sqrtf(dx * dx + dy * dy);
}

static int icon_inside(int kind, float x, float y)
{
    const float T = 0.075f; /* stroke width */
    switch (kind) {
    case ICON_RECENTS:
        /* two stacked cards, the back one peeking out top-right */
        if (in_ring_rrect(x, y, -0.06f, 0.07f, 0.28f, 0.22f, 0.08f, T)) return 1;
        return in_ring_rrect(x, y, 0.06f, -0.07f, 0.28f, 0.22f, 0.08f, T) &&
               !in_rrect(x, y, -0.06f, 0.07f, 0.28f, 0.22f, 0.08f);
    case ICON_SETTINGS: {
        /* gear: 8 teeth around a ring with a hole */
        float rr = sqrtf(x * x + y * y), a = atan2f(y, x);
        float tooth = cosf(a * 8) > 0.25f ? 0.44f : 0.34f;
        return rr <= tooth && rr >= 0.14f;
    }
    case ICON_CHEVRON: /* tvOS's: a wide, shallow caret with round ends */
        return sd_segment(x, y, -0.42f, 0.08f, 0.0f, -0.08f) < 0.04f || sd_segment(x, y, 0.0f, -0.08f, 0.42f, 0.08f) < 0.04f;
    case ICON_CONTROLS: { /* Apple's Control Center glyph: two filled switches, knobs cut out, left and right */
        float py = y < 0 ? -0.15f : 0.15f, kx = y < 0 ? -0.2f : 0.2f;
        if (!in_rrect(x, y, 0, py, 0.34f, 0.12f, 0.12f)) return 0;
        return (x - kx) * (x - kx) + (y - py) * (y - py) > 0.085f * 0.085f;
    }
    case ICON_POWER: { /* a ring open at the top, and a bar into the gap */
        float rr = sqrtf(x * x + y * y), up = atan2f(x, -y);
        return (fabsf(rr - 0.31f) < T / 2 && fabsf(up) > 0.62f) || sd_segment(x, y, 0, -0.42f, 0, -0.06f) < T / 2;
    }
    case ICON_WIFI: { /* three arcs over a dot */
        float dy = y - 0.27f, rr = sqrtf(x * x + dy * dy), up = atan2f(x, -dy);
        if (rr < 0.06f) return 1;
        if (fabsf(up) > 0.78f) return 0;
        return fabsf(rr - 0.18f) < T / 2 || fabsf(rr - 0.33f) < T / 2 || fabsf(rr - 0.48f) < T / 2;
    }
    case ICON_SCREEN_OFF: /* a screen on a stand, struck through */
        if (in_ring_rrect(x, y, 0, -0.05f, 0.38f, 0.25f, 0.06f, T)) return sd_segment(x, y, -0.36f, -0.36f, 0.36f, 0.26f) > T * 1.4f;
        if (sd_segment(x, y, -0.13f, 0.32f, 0.13f, 0.32f) < T / 2) return 1;
        return sd_segment(x, y, -0.36f, -0.36f, 0.36f, 0.26f) < T / 2;
    case ICON_SOUND: { /* a speaker and two waves */
        if (x >= -0.36f && x <= -0.2f && fabsf(y) <= 0.11f) return 1;
        if (x >= -0.2f && x <= -0.02f && fabsf(y) <= 0.11f + (x + 0.2f) * 1.0f) return 1;
        float rr = sqrtf((x + 0.02f) * (x + 0.02f) + y * y), side = atan2f(y, x + 0.02f);
        return fabsf(side) < 0.8f && (fabsf(rr - 0.2f) < T / 2 || fabsf(rr - 0.36f) < T / 2);
    }
    case ICON_TIMER: { /* a stopwatch: a ring, a hand, a button on top */
        float dy = y - 0.05f, rr = sqrtf(x * x + dy * dy);
        if (fabsf(rr - 0.3f) < T / 2) return 1;
        if (sd_segment(x, y, 0, 0.05f, 0, -0.12f) < T / 2 || sd_segment(x, y, 0, 0.05f, 0.12f, 0.05f) < T / 2) return 1;
        return sd_segment(x, y, -0.09f, -0.38f, 0.09f, -0.38f) < T / 2 || sd_segment(x, y, 0, -0.38f, 0, -0.27f) < T / 2;
    }
    case ICON_INPUT: /* a screen with an arrow going in */
        if (in_ring_rrect(x, y, 0.1f, 0, 0.3f, 0.24f, 0.06f, T) && !(x < -0.1f && fabsf(y) < 0.12f)) return 1;
        return sd_segment(x, y, -0.44f, 0, 0.1f, 0) < T / 2 || sd_segment(x, y, 0.1f, 0, -0.03f, -0.13f) < T / 2 ||
               sd_segment(x, y, 0.1f, 0, -0.03f, 0.13f) < T / 2;
    case ICON_ETHERNET: /* a network socket: its outline, the latch on top, three pins */
        if (in_ring_rrect(x, y, 0, 0.04f, 0.36f, 0.28f, 0.06f, T)) return 1;
        if (x >= -0.13f && x <= 0.13f && y >= -0.24f && y <= -0.12f) return 1;
        return sd_segment(x, y, -0.13f, 0.02f, -0.13f, 0.16f) < T / 2 || sd_segment(x, y, 0, 0.02f, 0, 0.16f) < T / 2 ||
               sd_segment(x, y, 0.13f, 0.02f, 0.13f, 0.16f) < T / 2;
    case ICON_PICTURE: /* a photo: its frame, a mountain and the sun */
        if (in_ring_rrect(x, y, 0, 0, 0.4f, 0.3f, 0.07f, T)) return 1;
        if (!in_rrect(x, y, 0, 0, 0.4f - T, 0.3f - T, 0.02f)) return 0;
        if ((x - 0.15f) * (x - 0.15f) + (y + 0.1f) * (y + 0.1f) < 0.065f * 0.065f) return 1;
        return y > -0.04f + fabsf(x + 0.09f) * 0.95f && y < 0.24f;
    case ICON_EYE: { /* an eye: the lid's outline (where two circles overlap) and the pupil */
        const float R = 0.5f, C = 0.32f;
        float a = sqrtf(x * x + (y - C) * (y - C)), b = sqrtf(x * x + (y + C) * (y + C));
        if (x * x + y * y < 0.11f * 0.11f) return 1;
        return a < R && b < R && (a > R - T || b > R - T);
    }
    case ICON_SOUND_MODE: /* an equaliser: four bars of different heights */
        for (int k = 0; k < 4; k++) {
            static const float hgt[4] = { 0.18f, 0.36f, 0.26f, 0.12f };
            float bx = -0.27f + k * 0.18f;
            if (sd_segment(x, y, bx, 0.3f, bx, 0.3f - 2 * hgt[k]) < T / 2 + 0.01f) return 1;
        }
        return 0;
    case ICON_BRIGHTNESS: { /* the sun: a disc and eight rays */
        float rr = sqrtf(x * x + y * y), a = atan2f(y, x);
        if (rr < 0.17f) return 1;
        float ray = fmodf(a + 2 * (float)M_PI + (float)M_PI / 8, (float)M_PI / 4) - (float)M_PI / 8;
        return rr > 0.27f && rr < 0.42f && fabsf(ray) * rr < T / 2;
    }
    case ICON_ENERGY: { /* a leaf: two arcs meeting at its tip, and its vein */
        float a = sqrtf((x + 0.17f) * (x + 0.17f) + (y + 0.17f) * (y + 0.17f)), b = sqrtf((x - 0.17f) * (x - 0.17f) + (y - 0.17f) * (y - 0.17f));
        if (a < 0.42f && b < 0.42f) return a > 0.42f - T || b > 0.42f - T || fabsf(x + y) < T / 2;
        return sd_segment(x, y, -0.3f, 0.3f, -0.18f, 0.18f) < T / 2;
    }
    case ICON_GAME: /* a gamepad: its body, a cross on the left, two buttons on the right */
        if (!in_rrect(x, y, 0, 0.02f, 0.42f, 0.2f, 0.18f)) return 0;
        if ((fabsf(x + 0.2f) < 0.03f && fabsf(y - 0.02f) < 0.1f) || (fabsf(y - 0.02f) < 0.03f && fabsf(x + 0.2f) < 0.1f)) return 0;
        return (x - 0.17f) * (x - 0.17f) + (y + 0.03f) * (y + 0.03f) > 0.045f * 0.045f &&
               (x - 0.27f) * (x - 0.27f) + (y - 0.07f) * (y - 0.07f) > 0.045f * 0.045f;
    case ICON_HDR: { /* contrast: a ring, its right half filled */
        float rr = sqrtf(x * x + y * y);
        return fabsf(rr - 0.34f) < T / 2 || (rr < 0.34f && x > 0);
    }
    case ICON_AMBIENT: /* a screen glowing at its edges: the screen, and short rays around it */
        if (in_ring_rrect(x, y, 0, 0, 0.24f, 0.16f, 0.04f, T)) return 1;
        return sd_segment(x, y, -0.36f, -0.28f, -0.3f, -0.22f) < T / 2 || sd_segment(x, y, 0.36f, -0.28f, 0.3f, -0.22f) < T / 2 ||
               sd_segment(x, y, -0.36f, 0.28f, -0.3f, 0.22f) < T / 2 || sd_segment(x, y, 0.36f, 0.28f, 0.3f, 0.22f) < T / 2 ||
               sd_segment(x, y, -0.44f, 0, -0.34f, 0) < T / 2 || sd_segment(x, y, 0.44f, 0, 0.34f, 0) < T / 2 ||
               sd_segment(x, y, 0, -0.36f, 0, -0.26f) < T / 2 || sd_segment(x, y, 0, 0.36f, 0, 0.26f) < T / 2;
    case ICON_CAPTURE: /* a screen recording: the screen and a dot in it */
        if (in_ring_rrect(x, y, 0, -0.03f, 0.38f, 0.26f, 0.06f, T)) return 1;
        if (sd_segment(x, y, -0.13f, 0.34f, 0.13f, 0.34f) < T / 2) return 1;
        return x * x + (y + 0.03f) * (y + 0.03f) < 0.1f * 0.1f;
    case ICON_PLUS:
        return (fabsf(x) < T / 2 && fabsf(y) < 0.3f) || (fabsf(y) < T / 2 && fabsf(x) < 0.3f);
    case ICON_REMOTE: /* a remote, upright: its outline, the ring of arrows and two buttons below */
        if (in_ring_rrect(x, y, 0, 0, 0.17f, 0.44f, 0.13f, T)) return 1;
        if (fabsf(sqrtf(x * x + (y + 0.2f) * (y + 0.2f)) - 0.075f) < T / 2) return 1;
        return x * x + (y - 0.08f) * (y - 0.08f) < 0.04f * 0.04f || x * x + (y - 0.22f) * (y - 0.22f) < 0.04f * 0.04f;
    case ICON_TILES: /* four app tiles */
        return in_rrect(x, y, -0.19f, -0.19f, 0.15f, 0.15f, 0.06f) || in_rrect(x, y, 0.19f, -0.19f, 0.15f, 0.15f, 0.06f) ||
               in_rrect(x, y, -0.19f, 0.19f, 0.15f, 0.15f, 0.06f) || in_rrect(x, y, 0.19f, 0.19f, 0.15f, 0.15f, 0.06f);
    case ICON_PERSON: { /* a head over shoulders */
        if (x * x + (y + 0.17f) * (y + 0.17f) < 0.15f * 0.15f) return 1;
        float d = sqrtf(x * x + (y - 0.4f) * (y - 0.4f));
        return y < 0.4f && d < 0.36f && d > 0;
    }
    case ICON_INFO: { /* an i in a ring */
        float rr = sqrtf(x * x + y * y);
        if (fabsf(rr - 0.42f) < T / 2) return 1;
        if (x * x + (y + 0.18f) * (y + 0.18f) < 0.055f * 0.055f) return 1;
        return sd_segment(x, y, 0, -0.04f, 0, 0.22f) < T / 2 + 0.005f;
    }
    case ICON_TV: /* a screen on a stand */
        if (in_ring_rrect(x, y, 0, -0.05f, 0.4f, 0.26f, 0.06f, T)) return 1;
        return sd_segment(x, y, -0.15f, 0.34f, 0.15f, 0.34f) < T / 2 || sd_segment(x, y, 0, 0.21f, 0, 0.34f) < T / 2;
    case ICON_ACCESS: { /* the accessibility figure in a ring */
        float rr = sqrtf(x * x + y * y);
        if (fabsf(rr - 0.42f) < T / 2) return 1;
        if (x * x + (y + 0.2f) * (y + 0.2f) < 0.065f * 0.065f) return 1;
        return sd_segment(x, y, -0.22f, -0.07f, 0.22f, -0.07f) < T / 2 || sd_segment(x, y, 0, -0.07f, 0, 0.08f) < T / 2 ||
               sd_segment(x, y, 0, 0.08f, -0.11f, 0.27f) < T / 2 || sd_segment(x, y, 0, 0.08f, 0.11f, 0.27f) < T / 2;
    }
    }
    return 0;
}

Tex *gfx_icon(int kind, int size)
{
    enum { SS = 4 };
    unsigned char *px = malloc((size_t)size * size * 4);
    if (!px) return NULL;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int n = 0;
            for (int sy = 0; sy < SS; sy++)
                for (int sx = 0; sx < SS; sx++)
                    n += icon_inside(kind, (x + (sx + 0.5f) / SS) / size - 0.5f, (y + (sy + 0.5f) / SS) / size - 0.5f);
            unsigned char *p = px + ((size_t)y * size + x) * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = (unsigned char)(255 * n / (SS * SS));
        }
    }
    Tex *t = r_tex(px, size, size);
    free(px);
    return t;
}

/* ------------------------------------------------------------------ text */

struct Font {
    unsigned char *data; /* the font file, mapped read-only: pages load as glyphs need them */
    size_t size;
    stbtt_fontinfo info;
};

Font *font_load(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    void *map = fstat(fd, &st) == 0 && st.st_size > 0 ? mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    close(fd);
    if (map == MAP_FAILED) return NULL;
    Font *f = calloc(1, sizeof *f);
    int off = f ? stbtt_GetFontOffsetForIndex(map, 0) : -1;
    if (!f || off < 0 || !stbtt_InitFont(&f->info, map, off)) {
        munmap(map, (size_t)st.st_size);
        free(f);
        return NULL;
    }
    f->data = map;
    f->size = (size_t)st.st_size;
    return f;
}

void font_free(Font *f)
{
    if (!f) return;
    munmap(f->data, f->size);
    free(f);
}

/* System fonts for what Inter lacks, mapped when a title first needs one. On the
 * TV they're LG's own; on a Linux desktop, common ones. */
static const char *const FALLBACK_PATHS[] = {
    "/usr/share/fonts/LG_Smart_UI_AR_HE_TH-SemiBold.ttf", /* Arabic, Hebrew, Thai */
    "/usr/share/fonts/DroidSansFallback.ttf",             /* Chinese, Japanese, Korean */
    "/usr/share/fonts/LG_Smart_UI_Devanagari.ttf",
    "/usr/share/fonts/NotoEmoji-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
};
enum { NFALLBACK = sizeof FALLBACK_PATHS / sizeof *FALLBACK_PATHS };
static Font *fallback[NFALLBACK];

/* The font that draws codepoint c, and its glyph: f, else the first fallback that has it. */
static Font *font_for(Font *f, int c, int *glyph)
{
    static int loaded;
    *glyph = stbtt_FindGlyphIndex(&f->info, c);
    if (*glyph) return f;
    if (!loaded) {
        loaded = 1;
        for (int i = 0; i < NFALLBACK; i++) fallback[i] = font_load(FALLBACK_PATHS[i]);
    }
    for (int i = 0; i < NFALLBACK; i++) {
        int g = fallback[i] ? stbtt_FindGlyphIndex(&fallback[i]->info, c) : 0;
        if (g) {
            *glyph = g;
            return fallback[i];
        }
    }
    return f; /* its missing-glyph box */
}

/* Decodes one UTF-8 codepoint; advances *s. Invalid bytes map to '?'. */
static int utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    int c = p[0], n = 0;
    if (c < 0x80) n = 0;
    else if ((c & 0xE0) == 0xC0) { c &= 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; n = 3; }
    else { (*s)++; return '?'; }
    for (int i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *s += i; return '?'; }
        c = (c << 6) | (p[i] & 0x3F);
    }
    *s += n + 1;
    return c;
}

/* control and zero-width formatting characters: joiners, bidi marks, variation selectors */
static int ignorable(int c)
{
    return c < 0x20 || (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) || (c >= 0x2060 && c <= 0x206F) ||
           (c >= 0xFE00 && c <= 0xFE0F) || c == 0xFEFF;
}

/* ---- Arabic: each letter takes the form that joins its neighbours (U+FE70..FEFF) */

/* first presentation form (isolated) of U+0621..064A; 0 = none */
static const unsigned short AR_BASE[] = {
    0xFE80, 0xFE81, 0xFE83, 0xFE85, 0xFE87, 0xFE89, 0xFE8D, 0xFE8F, 0xFE93, 0xFE95, 0xFE99, 0xFE9D, 0xFEA1, 0xFEA5,
    0xFEA9, 0xFEAB, 0xFEAD, 0xFEAF, 0xFEB1, 0xFEB5, 0xFEB9, 0xFEBD, 0xFEC1, 0xFEC5, 0xFEC9, 0xFECD,
    0, 0, 0, 0, 0, 0, /* 063B..0640 */
    0xFED1, 0xFED5, 0xFED9, 0xFEDD, 0xFEE1, 0xFEE5, 0xFEE9, 0xFEED, 0xFEEF, 0xFEF1,
};

/* joining type: 'D' joins both sides, 'R' only the letter before it, 'U' neither,
 * 'T' marks (skipped when looking for neighbours), 0 not Arabic */
static int ar_type(int c)
{
    if ((c >= 0x064B && c <= 0x065F) || c == 0x0670) return 'T';
    if (c == 0x0640) return 'D'; /* tatweel */
    if (c < 0x0621 || c > 0x064A) return 0;
    switch (c) {
    case 0x0621: return 'U';
    case 0x0622: case 0x0623: case 0x0624: case 0x0625: case 0x0627: case 0x0629:
    case 0x062F: case 0x0630: case 0x0631: case 0x0632: case 0x0648: case 0x0649: return 'R';
    }
    return AR_BASE[c - 0x0621] ? 'D' : 0;
}

static int shape_arabic(const int *in, int n, int *out)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        int c = in[i], t = ar_type(c);
        if (t != 'D' && t != 'R' && t != 'U') {
            out[m++] = c;
            continue;
        }
        int p = i - 1, q = i + 1;
        while (p >= 0 && ar_type(in[p]) == 'T') p--;
        while (q < n && ar_type(in[q]) == 'T') q++;
        int tq = q < n ? ar_type(in[q]) : 0;
        int join_prev = t != 'U' && p >= 0 && ar_type(in[p]) == 'D';
        int join_next = t == 'D' && (tq == 'D' || tq == 'R');
        if (c == 0x0644 && (tq == 'R') && (in[q] == 0x0622 || in[q] == 0x0623 || in[q] == 0x0625 || in[q] == 0x0627)) {
            /* lam + alef are one ligature */
            out[m++] = (in[q] == 0x0622 ? 0xFEF5 : in[q] == 0x0623 ? 0xFEF7 : in[q] == 0x0625 ? 0xFEF9 : 0xFEFB) + join_prev;
            for (int k = i + 1; k < q; k++) out[m++] = in[k];
            i = q;
            continue;
        }
        int base = c == 0x0640 ? 0 : AR_BASE[c - 0x0621];
        out[m++] = base ? base + (join_prev && join_next ? 3 : join_prev ? 1 : join_next ? 2 : 0) : c;
    }
    return m;
}

/* ---- Devanagari: the i-matra (ि) is typed after its consonant but drawn before it */

static int deva_consonant(int c) { return (c >= 0x0915 && c <= 0x0939) || (c >= 0x0958 && c <= 0x095F); }

static void order_devanagari(int *c, int n)
{
    for (int i = 1; i < n; i++) {
        if (c[i] != 0x093F) continue;
        int s = i - 1;
        if (s > 0 && c[s] == 0x093C) s--; /* nukta */
        while (s >= 2 && c[s - 1] == 0x094D && deva_consonant(c[s - 2])) s -= 2; /* conjunct */
        if (!deva_consonant(c[s])) continue;
        memmove(&c[s + 1], &c[s], (size_t)(i - s) * sizeof *c);
        c[s] = 0x093F;
    }
}

/* ---- right-to-left: lines are drawn left to right, so Hebrew and Arabic runs are
 * reversed (numbers inside them keep their order). A line that starts with
 * right-to-left text is laid out right to left as a whole, Latin words in it kept
 * as they are; its ellipsis then sits on the left. No full Unicode bidi. */

static int is_rtl(int c) { return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF); }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_neutral(int c)
{
    return (c < 0x80 && !((c | 0x20) >= 'a' && (c | 0x20) <= 'z')) || (c >= 0xA0 && c <= 0xBF) ||
           (c >= 0x2000 && c <= 0x206F) || (c >= 0x3000 && c <= 0x303F);
}

static void reverse(int *a, int n)
{
    for (int i = 0, j = n - 1; i < j; i++, j--) {
        int t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
}

/* reverses each run of characters matching want, with the neutrals inside it;
 * keep_numbers: numbers inside a run still read left to right */
static void reverse_runs(int *c, int n, int (*want)(int), int keep_numbers)
{
    for (int i = 0; i < n; i++) {
        if (!want(c[i])) continue;
        int end = i;
        for (int j = i + 1; j < n && (want(c[j]) || is_neutral(c[j])); j++)
            if (want(c[j])) end = j;
        reverse(c + i, end - i + 1);
        for (int k = i; keep_numbers && k <= end; k++) {
            int e = k;
            while (e <= end && is_digit(c[e])) e++;
            if (e - k > 1) reverse(c + k, e - k);
            if (e > k) k = e - 1;
        }
        i = end;
    }
}

static int is_ltr_word(int c) { return !is_rtl(c) && (!is_neutral(c) || is_digit(c)); }

static void visual_order(int *c, int n)
{
    int first = 0; /* the line's direction: its first strong character */
    for (int i = 0; i < n && !first; i++)
        if (is_rtl(c[i])) first = 'R';
        else if (!is_neutral(c[i])) first = 'L';
    if (first != 'R') {
        reverse_runs(c, n, is_rtl, 1);
    } else {
        reverse(c, n);
        reverse_runs(c, n, is_ltr_word, 0); /* Latin words and numbers read left to right */
    }
}

/* ---- layout */

#define MAX_CPS 512

typedef struct {
    Font *f;
    int g;
    float scale;
} Glyph;

/* Picks each character's font and fills pos[0..n] with pen positions (pos[n] = width). */
static float layout_line(Font *f, float px, const int *cps, int n, Glyph *gl, float *pos)
{
    float pen = 0;
    for (int i = 0; i < n; i++) {
        gl[i].f = font_for(f, cps[i], &gl[i].g);
        gl[i].scale = stbtt_ScaleForMappingEmToPixels(&gl[i].f->info, px);
    }
    for (int i = 0; i < n; i++) {
        int adv, lsb;
        pos[i] = pen;
        stbtt_GetGlyphHMetrics(&gl[i].f->info, gl[i].g, &adv, &lsb);
        pen += adv * gl[i].scale;
        if (i + 1 < n && gl[i + 1].f == gl[i].f) pen += gl[i].scale * stbtt_GetGlyphKernAdvance(&gl[i].f->info, gl[i].g, gl[i + 1].g);
    }
    pos[n] = pen;
    return pen;
}

/* typed text -> what is drawn: Arabic joined, Devanagari reordered */
static int shape(const int *in, int n, int *out)
{
    int m = shape_arabic(in, n, out);
    order_devanagari(out, m);
    return m;
}

/* Match text_render's shaping and line-box width without making a texture. */
int text_width(Font *f, float px, const char *utf8)
{
    if (!f || !utf8 || !*utf8) return 0;
    int src[MAX_CPS + 1], cps[MAX_CPS + 1], n = 0;
    Glyph gl[MAX_CPS + 1];
    float pos[MAX_CPS + 2];
    for (const char *s = utf8; *s && n < MAX_CPS;) {
        int c = utf8_next(&s);
        if (!ignorable(c)) src[n++] = c;
    }
    if (!n) return 0;
    int m = shape(src, n, cps);
    return (int)ceilf(layout_line(f, px, cps, m, gl, pos)) + 4;
}

Tex *text_render(Font *f, float px, const char *utf8, int max_w,
                 int *out_w, int *out_h)
{
    if (!f || !utf8 || !*utf8) return NULL;
    int src[MAX_CPS + 1], n = 0;
    for (const char *s = utf8; *s && n < MAX_CPS;) {
        int c = utf8_next(&s);
        if (!ignorable(c)) src[n++] = c;
    }
    if (!n) return NULL;

    /* lay out; if too wide, replace trailing characters with an ellipsis (cut in
     * typed order, so joining and right-to-left order are redone around it) */
    static int cps[MAX_CPS + 1];
    static Glyph gl[MAX_CPS + 1];
    static float pos[MAX_CPS + 2];
    int m = shape(src, n, cps);
    if (max_w > 0 && layout_line(f, px, cps, m, gl, pos) > max_w) {
        while (n > 1) {
            n--;
            src[n - 1] = 0x2026;
            m = shape(src, n, cps);
            if (layout_line(f, px, cps, m, gl, pos) <= max_w) break;
        }
    }
    visual_order(cps, m);
    layout_line(f, px, cps, m, gl, pos);

    /* the line box fits every font used */
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
    float scale = stbtt_ScaleForMappingEmToPixels(&f->info, px), top = asc * scale, bottom = desc * scale;
    for (int i = 0; i < m; i++) {
        if (gl[i].f == f) continue;
        stbtt_GetFontVMetrics(&gl[i].f->info, &asc, &desc, &gap);
        top = fmaxf(top, asc * gl[i].scale);
        bottom = fminf(bottom, desc * gl[i].scale);
    }
    int w = (int)ceilf(pos[m]) + 4;
    int h = (int)ceilf(top - bottom) + 4;
    int baseline = (int)roundf(top) + 2;
    unsigned char *cov = calloc((size_t)w * h, 1);
    for (int i = 0; i < m; i++) {
        float x = pos[i] + 1;
        int ix = (int)floorf(x);
        int x0, y0, x1, y1;
        const stbtt_fontinfo *info = &gl[i].f->info;
        stbtt_GetGlyphBitmapBoxSubpixel(info, gl[i].g, gl[i].scale, gl[i].scale, x - ix, 0, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw <= 0 || gh <= 0) continue;
        unsigned char *g = malloc((size_t)gw * gh);
        stbtt_MakeGlyphBitmapSubpixel(info, g, gw, gh, gw, gl[i].scale, gl[i].scale, x - ix, 0, gl[i].g);
        for (int yy = 0; yy < gh; yy++) {
            int ty = baseline + y0 + yy;
            if (ty < 0 || ty >= h) continue;
            for (int xx = 0; xx < gw; xx++) {
                int tx = ix + x0 + xx;
                if (tx < 0 || tx >= w) continue;
                int v = cov[ty * w + tx] + g[yy * gw + xx];
                cov[ty * w + tx] = v > 255 ? 255 : (unsigned char)v;
            }
        }
        free(g);
    }

    unsigned char *rgba = malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) {
        rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = cov[i];
    }
    Tex *t = r_tex(rgba, w, h);
    free(cov);
    free(rgba);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return t;
}
