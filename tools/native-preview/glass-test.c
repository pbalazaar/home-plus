/* Renders a Liquid Glass test card with native/src/render.c and saves a PNG,
 * for tuning the material without the TV or the service:
 *   glass-test <art.jpg> <out.png> [focus 0..1] [light_x light_y]
 * Build and run it with tools/native-preview/glass-test.sh. */
#include <stdio.h>
#include <stdlib.h>

#include <SDL.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "render.h"

#define W 1920
#define H 1080

static Tex *gradient(int vertical, float a0, float a1)
{
    unsigned char px[256 * 4];
    for (int i = 0; i < 256; i++) {
        px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = 0;
        px[i * 4 + 3] = (unsigned char)((a0 + (a1 - a0) * i / 255.0f) * 255);
    }
    return vertical ? r_tex(px, 1, 256) : r_tex(px, 256, 1);
}

static void background(Tex *art, Tex *scrim_v, Tex *scrim_h)
{
    r_cover(art, 0, 0, W, H, 255, 0);
    r_image(scrim_v, 0, H * 0.55f, W, H * 0.45f, 255, 255, 255, 255, 0);
    r_image(scrim_h, 0, 0, W * 0.5f, H, 255, 255, 255, 255, 0);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: glass-test art.jpg out.png [focus] [light_x light_y]\n");
        return 2;
    }
    float focus = argc > 3 ? (float)atof(argv[3]) : 1.0f;
    float lx = argc > 5 ? (float)atof(argv[4]) : -0.6f, ly = argc > 5 ? (float)atof(argv[5]) : -0.8f;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    r_gl_attributes();
    SDL_Window *win = SDL_CreateWindow("glass-test", 0, 0, W, H, SDL_WINDOW_OPENGL);
    if (!win || !r_init(win, W, H)) {
        fprintf(stderr, "init failed: %s\n", SDL_GetError());
        return 1;
    }
    int aw, ah, n;
    unsigned char *px = stbi_load(argv[1], &aw, &ah, &n, 4);
    if (!px) {
        fprintf(stderr, "can't load %s\n", argv[1]);
        return 1;
    }
    Tex *art = r_tex(px, aw, ah);
    stbi_image_free(px);
    Tex *scrim_v = gradient(1, 0.0f, 0.6f), *scrim_h = gradient(0, 0.5f, 0.0f);

    /* what the glass refracts: the Top Shelf and the icons under the dock */
    r_backdrop_begin(0);
    background(art, scrim_v, scrim_h);
    r_backdrop_end();

    r_frame_begin(8, 8, 11);
    background(art, scrim_v, scrim_h);

    Glass g = { 0 };
    g.lens = 1;
    g.light_x = lx;
    g.light_y = ly;
    g.alpha = 1;

    /* spacing system (native/src/home.h): tvOS 26's dock at x 48-1872, y 785-1015, six
     * columns of 5:3 tiles 50 px apart; containers keep their corners concentric */
    const float DOCK_X = 48, TOP_Y = 52, TILE_W = 248, TILE_H = 150, TILE_R = 22, GAP = 50, PAD = 40, PAD_X = 43;
    const float LEFT = DOCK_X + PAD_X, DOCK_Y = 785;

    /* dock behind the top row: clear glass, the icons on it are opaque */
    g.x = DOCK_X; g.y = DOCK_Y; g.w = W - 2 * DOCK_X; g.h = TILE_H + 2 * PAD; g.radius = TILE_R + PAD;
    g.legible = 0; g.focus = 0;
    r_shadow(g.x, g.y, g.w, g.h, g.radius, 40, 14, 0.30f);
    r_glass(&g);

    static const Uint8 cols[6][3] = { { 30, 30, 46 }, { 235, 235, 235 }, { 24, 24, 24 }, { 31, 110, 220 }, { 240, 240, 240 }, { 225, 30, 40 } };
    for (int i = 0; i < 6; i++) {
        float x = LEFT + i * (TILE_W + GAP), y = DOCK_Y + PAD;
        int f = i == 1;
        float s = f ? 1.0f + 0.2f * focus : 1.0f;
        float sw = TILE_W * s, sh = TILE_H * s, sx = x - (sw - TILE_W) / 2, sy = y - (sh - TILE_H) / 2;
        if (f) r_shadow(sx, sy, sw, sh, TILE_R * s, 30, 18, 0.55f * focus);
        r_shape(sx, sy, sw, sh, TILE_R * s, cols[i][0], cols[i][1], cols[i][2], 255, 0, 1, 1);
        r_shape(sx, sy, sw, sh, TILE_R * s, 255, 255, 255, f ? 255 : 110, f ? 2.5f : 1.5f, f ? 0.9f : 0.5f, 0.15f);
    }

    /* top right: the clock capsule, its circle (Settings) inset at the right end */
    const float BAR = 56, CLOCK_W = 180, DOT = BAR - 10;
    g.legible = 1;
    g.x = W - DOCK_X - CLOCK_W; g.y = TOP_Y; g.w = CLOCK_W; g.h = BAR; g.radius = BAR / 2; g.focus = 0;
    r_glass(&g);
    r_shape(W - DOCK_X - 5 - DOT, TOP_Y + 5, DOT, DOT, DOT / 2, 255, 255, 255, 41, 0, 1, 1);
    r_shape(g.x + 24, TOP_Y + 19, 90, 18, 9, 255, 255, 255, 200, 0, 1, 1); /* stand-in for the time */

    /* an open menu: 40 px padding, 80 px capsule buttons 16 px apart, corner 40 + 40 */
    const float MW = 720, BH = 80, MGAP = 16, TITLE = 36;
    float mh = PAD + TITLE + 24 + 3 * BH + 2 * MGAP + PAD;
    g.x = (W - MW) / 2; g.y = (H - mh) / 2; g.w = MW; g.h = mh; g.radius = BH / 2 + PAD; g.focus = 0;
    r_shadow(g.x, g.y, g.w, g.h, g.radius, 60, 24, 0.45f);
    r_glass(&g);
    r_shape(g.x + PAD, g.y + PAD + 8, 260, 20, 10, 255, 255, 255, 150, 0, 1, 1); /* stand-in for the title */
    for (int i = 0; i < 3; i++) {
        float bx = g.x + PAD, by = g.y + PAD + TITLE + 24 + i * (BH + MGAP), bw = MW - 2 * PAD;
        if (i == 0) {
            r_shadow(bx, by, bw, BH, BH / 2, 18, 8, 0.4f);
            r_shape(bx, by, bw, BH, BH / 2, 245, 245, 247, 255, 0, 1, 1);
        } else {
            r_shape(bx, by, bw, BH, BH / 2, 255, 255, 255, 30, 0, 1, 1);
        }
    }

    unsigned char *shot = malloc((size_t)W * H * 4);
    int sw, sh;
    r_read_pixels(shot, &sw, &sh);
    int ok = stbi_write_png(argv[2], sw, sh, 4, shot, sw * 4);
    printf("%s %dx%d\n", ok ? "wrote" : "FAILED", sw, sh);
    return ok ? 0 : 1;
}
