/* Home+ native: a small GLES2 renderer with shaders for shapes and Liquid Glass.
 *
 * SDL only provides the window, the GL context and vsync. Everything is drawn
 * in a fixed logical space (1920x1080) whatever the target, one quad per call:
 * a frame is a few dozen draws. Glass refracts a "backdrop": a half-resolution
 * copy of whatever is behind it, drawn between r_backdrop_begin/end, which
 * also builds the blurred versions the glass samples. Glass is clear unless
 * what's on it needs help to stay legible. */
#ifndef HOMEPLUS_RENDER_H
#define HOMEPLUS_RENDER_H

#include <SDL.h>

typedef struct Tex {
    unsigned id;
    int w, h;
} Tex;

/* Call before SDL_CreateWindow: asks for an OpenGL ES 2.0 context. */
void r_gl_attributes(void);
/* Creates the context, programs and render targets. 0 on failure. */
int r_init(SDL_Window *win, int logical_w, int logical_h);
void r_shutdown(void);

void r_frame_begin(Uint8 r, Uint8 g, Uint8 b); /* screen target, cleared */
void r_present(void);
/* Shifts everything drawn on screen by (dx, dy) logical px: the OLED pixel
 * orbit. What the glass refracts stays put, so the two stay aligned. */
void r_set_offset(float dx, float dy);
/* Moves what's drawn next by (dx, dy) logical pixels, on any target; (0, 0) resets it. */
void r_translate(float dx, float dy);

/* RGBA8, straight alpha. */
Tex *r_tex(const unsigned char *rgba, int w, int h);
void r_tex_free(Tex **t);

/* Textured quad tinted by rgba; radius > 0 rounds its corners. */
void r_image(const Tex *t, float x, float y, float w, float h, Uint8 r, Uint8 g, Uint8 b, Uint8 a, float radius);
/* Only the part of it from u0 to u1 (0..1, across), drawn into the rect: text that scrolls. */
void r_image_part(const Tex *t, float x, float y, float w, float h, float u0, float u1, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
/* An app icon's logo in a look other than its own colours. mono: in one colour (r, g, b): the
 * ink that stood out most from the icon's background solid, the rest down to `faint` (0..1)
 * opaque; ref is that background's luminance (< 0: none, the ink's own luminance counts) and
 * span the most any ink stood out. Else, for a dark tile: its own colours, with a light icon's
 * (ref > 0.5) grey and black ink turned light, and grey ink into (r, g, b) as much as ink (0..1) says. */
typedef struct {
    int mono;
    float ref, span, faint, ink;
    Uint8 r, g, b;
} LogoLook;
void r_logo(const Tex *t, float x, float y, float w, float h, float radius, const LogoLook *l, Uint8 a);
/* Same, scaled to cover the rect and cropped (biased upward, like the shelf art). */
void r_cover(const Tex *t, float x, float y, float w, float h, Uint8 a, float radius);
/* Rounded rect (radius = h/2 for a capsule). stroke > 0 draws only a ring that
 * fades from top_a at the top edge to bottom_a at the bottom (both 0..1). */
void r_shape(float x, float y, float w, float h, float radius, Uint8 r, Uint8 g, Uint8 b, Uint8 a,
             float stroke, float top_a, float bottom_a);
/* Soft shadow of a rounded rect: blur in px, offset dy, alpha 0..1. */
void r_shadow(float x, float y, float w, float h, float radius, float blur, float dy, float alpha);
/* White rim of a rounded rect that catches the light: brightest where it faces
 * the direction the light comes from (light_x, light_y). */
void r_rim(float x, float y, float w, float h, float radius, float width, Uint8 a, float light_x, float light_y);
/* Plain fill over the whole target (scrims, dimming). */
void r_fill(Uint8 r, Uint8 g, Uint8 b, Uint8 a);

/* Everything drawn between these lands in the backdrop the glass refracts. */
void r_backdrop_begin(void);
void r_backdrop_end(void);

/* The backdrop itself over the whole screen (blurred: the soft copy), e.g. a
 * blurred background behind content that should stand out. */
void r_backdrop_image(int blurred, Uint8 alpha);

typedef struct {
    float x, y, w, h, radius;
    float legible;           /* 0..1: content on it must stay readable, so blur and dim the
                                backdrop where it's busy or bright; 0 = always fully clear */
    float lens;              /* 0..1 how strongly it bends light; animate it to appear/disappear */
    float light_x, light_y;  /* direction the light comes from (normalised); default top-left */
    float focus;             /* 0..1 illumination when focused */
    float alpha;             /* 0..1 */
    Uint8 tr, tg, tb, ta;    /* tint mixed over the glass */
} Glass;
void r_glass(const Glass *g);

/* Reads the screen as RGBA8, top row first; buf holds w*h*4 of the drawable. */
int r_read_pixels(unsigned char *buf, int *w, int *h);

/* The GPU's own time for what's drawn between r_gpu_begin and r_gpu_end (a frame), where the
 * driver can say (GL_EXT_disjoint_timer_query: r_gpu_timer() is 1, and starts it). Frames come
 * back in order a few frames later: r_gpu_result gives the next one's time in µs, -1 while it
 * isn't in (wait: until it is), -2 when the GPU lost it (a disjoint: its clock changed). */
int r_gpu_timer(void);
void r_gpu_begin(void);
void r_gpu_end(void);
long r_gpu_result(int wait);
/* GL_RENDERER (0) or GL_VERSION (1) */
const char *r_gl_info(int what);
/* Waits until the GPU has done everything asked of it so far. */
void r_finish(void);

#endif
