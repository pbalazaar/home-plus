/* Home+ native: textures made on the CPU once at start-up (glow, scrims, top-bar
 * icons) and text. Everything is white or black with alpha, so a draw tints it;
 * shapes and glass are drawn by shaders (render.h), not textures. */
#ifndef HOMEPLUS_GFX_H
#define HOMEPLUS_GFX_H

#include "render.h"

/* White radial glow: alpha 1 in the centre fading to 0 at the edge. */
Tex *gfx_glow(int w, int h);

/* Black gradient strip for scrims. vertical: alpha goes a0 (top) -> a1 (bottom);
 * otherwise left -> right. Stretch it over the area you want darkened. */
Tex *gfx_gradient(int vertical, float a0, float a1);

/* White line icons, drawn once at start-up: the chevron above the dock (Up leads
 * into the Top Shelf), and the Control Center's: its own glyph (two switches, on
 * the clock) and its controls'. */
enum { ICON_RECENTS, ICON_SETTINGS, ICON_CHEVRON, ICON_CONTROLS, ICON_POWER, ICON_WIFI, ICON_SCREEN_OFF,
       ICON_SOUND, ICON_TIMER, ICON_INPUT, ICON_ACCESS, ICON_ETHERNET, ICON_PICTURE, ICON_EYE,
       ICON_SOUND_MODE, ICON_BRIGHTNESS, ICON_ENERGY, ICON_GAME, ICON_HDR, ICON_AMBIENT, ICON_CAPTURE, ICON_PLUS,
       ICON_REMOTE, ICON_TILES, ICON_PERSON, ICON_INFO, ICON_TV, ICON_COUNT };
Tex *gfx_icon(int kind, int size);

/* ---- text ---- */

typedef struct Font Font;

Font *font_load(const char *path);
void font_free(Font *f);

/* Renders UTF-8 text in white at the given pixel height. max_w > 0 truncates
 * with an ellipsis. Returns NULL for empty text. */
Tex *text_render(Font *f, float px, const char *utf8, int max_w, int *out_w, int *out_h);
int text_width(Font *f, float px, const char *utf8);

#endif
