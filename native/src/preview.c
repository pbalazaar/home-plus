/* Home+ native: headless preview helpers for the desktop build (screenshots in
 * tools/native-preview.sh). Linked into the TV build too, where they're unused. */
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "render.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

int homeplus_screenshot(const char *path)
{
    int w, h;
    r_read_pixels(NULL, &w, &h);
    unsigned char *px = malloc((size_t)w * h * 4);
    if (!px) return 0;
    r_read_pixels(px, &w, &h);
    int ok = stbi_write_png(path, w, h, 4, px, w * 4);
    free(px);
    return ok;
}

/* HOMEPLUS_KEYS: a script of remote presses, one every 150 ms from 1.5 s in.
 * L R U D = arrows, O = OK, H = hold OK (opens the sheet), B = back, . = wait.
 * Pointer: M moves it to the next point of HOMEPLUS_POINTS ("x,y x,y ...",
 * logical px), C clicks where it is. X: another app comes in front (Home+ is
 * hidden), Y: Home+ is back (the Home button). */
static void pointer_event(Uint32 type, int x, int y)
{
    SDL_Event e = { 0 };
    e.type = type;
    if (type == SDL_MOUSEMOTION) {
        e.motion.x = x;
        e.motion.y = y;
    } else {
        e.button.button = SDL_BUTTON_LEFT;
        e.button.x = x;
        e.button.y = y;
    }
    SDL_PushEvent(&e);
}

void homeplus_preview_input(Uint32 elapsed)
{
    static size_t done;
    static Uint32 hold_until;
    const char *keys = getenv("HOMEPLUS_KEYS");
    if (!keys) return;
    if (hold_until && elapsed >= hold_until) {
        SDL_Event e = { 0 };
        e.type = SDL_KEYUP;
        e.key.keysym.sym = SDLK_RETURN;
        SDL_PushEvent(&e);
        hold_until = 0;
    }
    if (hold_until || done >= strlen(keys) || elapsed < 1500 + done * 150) return;
    char c = keys[done++];
    static int px = -1, py = -1;
    static const char *pts;
    if (c == '.') return;
    if (c == 'M') {
        if (!pts) pts = getenv("HOMEPLUS_POINTS");
        if (pts && sscanf(pts, "%d,%d", &px, &py) == 2) {
            /* a real move, from a little way off: a pointer that only twitches leaves the keys in charge */
            pointer_event(SDL_MOUSEMOTION, px > 960 ? px - 120 : px + 120, py);
            pointer_event(SDL_MOUSEMOTION, px, py);
            pts = strchr(pts, ' ');
            if (pts) pts++;
        }
        return;
    }
    if (c == 'C') {
        pointer_event(SDL_MOUSEBUTTONDOWN, px, py);
        pointer_event(SDL_MOUSEBUTTONUP, px, py);
        return;
    }
    if (c == 'X' || c == 'Y') {
        SDL_Event e = { 0 };
        e.type = SDL_WINDOWEVENT;
        e.window.event = c == 'X' ? SDL_WINDOWEVENT_HIDDEN : SDL_WINDOWEVENT_SHOWN;
        SDL_PushEvent(&e);
        return;
    }
    SDL_Keycode k = c == 'L' ? SDLK_LEFT : c == 'R' ? SDLK_RIGHT : c == 'U' ? SDLK_UP : c == 'D' ? SDLK_DOWN
                  : c == 'B' ? SDLK_ESCAPE : SDLK_RETURN;
    SDL_Event e = { 0 };
    e.type = SDL_KEYDOWN;
    e.key.keysym.sym = k;
    SDL_PushEvent(&e);
    if (c == 'H') {
        hold_until = elapsed + 800;
        return;
    }
    e.type = SDL_KEYUP;
    SDL_PushEvent(&e);
}
