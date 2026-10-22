/* Home+ native: icon.c, off the GPU. The cover's blur, carried along from pixel to pixel, gives what the
 * sums it replaced gave; and icons with a solid background, a bare logo and a picture make the tiles they
 * should. `make -C native test` builds and runs it (tools/test.sh native). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "home.h"

static int failures;

#define CHECK(cond, ...)                            \
    do {                                            \
        if (!(cond)) {                              \
            failures++;                             \
            printf("  FAIL (line %d): ", __LINE__); \
            printf(__VA_ARGS__);                    \
            printf("\n");                           \
        }                                           \
    } while (0)

/* the blur tiles.c made covers with before: every pixel's 15 values summed, 3 times across and down */
static void old_blur(float *f, float *t)
{
    enum { CW = COVER_W, CH = COVER_H, PASSES = 3, R = 7 };
    for (int pass = 0; pass < PASSES; pass++) {
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
}

static void blur_matches(void)
{
    puts("the cover's blur gives what the old one did");
    enum { N = COVER_W * COVER_H * 3 };
    static float a[N], b[N], t[N];
    srand(7);
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < N; i++) /* noise, a hard edge, then a gradient */
            a[i] = round == 0 ? (float)(rand() % 256) : round == 1 ? (i / 3 % COVER_W < COVER_W / 2 ? 255.0f : 0) : (float)(i % 256);
        memcpy(b, a, sizeof a);
        old_blur(a, t);
        cover_blur(b, t);
        float most = 0;
        int levels = 0; /* where the 8-bit colours differ */
        for (int i = 0; i < N; i++) {
            most = fmaxf(most, fabsf(a[i] - b[i]));
            levels += (int)a[i] != (int)b[i];
        }
        CHECK(most < 0.01f, "round %d: up to %.4f apart", round, most);
        CHECK(levels <= N / 1000, "round %d: %d of %d values a level apart", round, levels, N);
    }
}

/* a 256 x 256 icon: background bg (alpha 0: transparent), with a disc of colour ink in its middle;
 * noisy: every pixel different (a photo) */
static unsigned char *icon(const unsigned char bg[4], const unsigned char ink[3], int noisy)
{
    enum { S = 256 };
    unsigned char *px = malloc(S * S * 4);
    srand(11);
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            unsigned char *p = px + (y * S + x) * 4;
            int in = (x - 128) * (x - 128) + (y - 128) * (y - 128) < 50 * 50;
            for (int c = 0; c < 3; c++) p[c] = noisy ? (unsigned char)(rand() % 256) : in ? ink[c] : bg[c];
            p[3] = noisy || in ? 255 : bg[3];
        }
    return px;
}

static void tiles(void)
{
    puts("icons with a solid background, a bare logo and a picture make their tiles");
    const unsigned char red[4] = { 200, 30, 40, 255 }, clear[4] = { 0, 0, 0, 0 }, white[3] = { 250, 250, 250 }, dark[3] = { 20, 20, 30 };
    unsigned char *px = icon(red, white, 0);
    IconPrep *p = icon_prepare(px, 256, 256);
    CHECK(p, "no tile for a red icon");
    if (p) {
        CHECK(p->fullbleed && abs(p->col[0] - 200) <= 1 && abs(p->col[1] - 30) <= 1, "red icon: fullbleed %d, colour %d,%d,%d",
              p->fullbleed, p->col[0], p->col[1], p->col[2]);
        CHECK(p->cw >= 100 && p->cw <= 110 && p->ch >= 100 && p->ch <= 110, "red icon: its logo cut to %d x %d, not the disc's 100 + 4", p->cw, p->ch);
        CHECK(p->crop[3] == 0 && p->crop[((p->ch / 2) * p->cw + p->cw / 2) * 4 + 3] == 255, "red icon: its background isn't keyed out");
        CHECK(p->has_cover && p->logo_w > 0 && p->logo_r == 0, "red icon: cover %d, logo %.0f wide", p->has_cover, p->logo_w);
        unsigned char calm[COVER_W * COVER_H * 4], vivid[COVER_W * COVER_H * 4];
        icon_cover(p, 0, calm);
        icon_cover(p, 1, vivid);
        int at = (COVER_H / 2 * COVER_W + COVER_W / 2) * 4;
        CHECK(calm[at + 3] == 255 && vivid[at] > calm[at], "red icon: its calm cover isn't calmer (%d against %d)", calm[at], vivid[at]);
    }
    free(p);
    free(px);
    px = icon(clear, dark, 0);
    p = icon_prepare(px, 256, 256);
    CHECK(p && !p->fullbleed && p->col[0] == 236 && p->col[2] == 240, "a dark logo alone isn't on a light tile");
    free(p);
    free(px);
    px = icon(red, white, 1);
    p = icon_prepare(px, 256, 256);
    CHECK(p && !p->fullbleed && p->cw == 256 && fabsf(p->logo_w - TILE_H * 0.66f) < 0.01f && p->logo_r > 0,
          "a picture isn't kept whole, rounded, at one size");
    free(p);
    free(px);
}

int main(void)
{
    blur_matches();
    tiles();
    printf(failures ? "icon: %d failed\n" : "icon: all passed\n", failures);
    return failures ? 1 : 0;
}
