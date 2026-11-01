/* Writes Home+'s glyphs as white PNGs on transparent, for the Control Center that opens over other
 * apps (native/qml/: LG's compositor draws it, so it can't use our renderer). Built and run by
 * tools/make-glyphs.sh in the preview container:  glyphs <dir> <size> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render.h"

static unsigned char *grabbed; /* what gfx_icon hands the renderer */

Tex *r_tex(const unsigned char *rgba, int w, int h)
{
    static Tex t;
    free(grabbed);
    grabbed = malloc((size_t)w * h * 4);
    if (grabbed) memcpy(grabbed, rgba, (size_t)w * h * 4);
    return &t;
}

#include "gfx.c"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* the service's names for them (control.js, settings.js), as cc.c's CC_ICONS */
static const struct { const char *name; int icon; } GLYPHS[] = {
    { "power", ICON_POWER }, { "wifi", ICON_WIFI }, { "ethernet", ICON_ETHERNET }, { "screenOff", ICON_SCREEN_OFF },
    { "sound", ICON_SOUND }, { "timer", ICON_TIMER }, { "input", ICON_INPUT }, { "recents", ICON_RECENTS },
    { "access", ICON_ACCESS }, { "settings", ICON_SETTINGS }, { "picture", ICON_PICTURE }, { "eye", ICON_EYE },
    { "soundMode", ICON_SOUND_MODE }, { "brightness", ICON_BRIGHTNESS }, { "energy", ICON_ENERGY }, { "game", ICON_GAME },
    { "hdr", ICON_HDR }, { "ambient", ICON_AMBIENT }, { "capture", ICON_CAPTURE }, { "controls", ICON_CONTROLS },
    { "plus", ICON_PLUS }, { "remote", ICON_REMOTE }, { "tiles", ICON_TILES }, { "person", ICON_PERSON }, { "tv", ICON_TV },
    { "info", ICON_INFO },
};

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: glyphs <dir> <size>\n");
        return 2;
    }
    int size = atoi(argv[2]);
    for (size_t i = 0; i < sizeof GLYPHS / sizeof GLYPHS[0]; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s.png", argv[1], GLYPHS[i].name);
        gfx_icon(GLYPHS[i].icon, size);
        if (!grabbed || !stbi_write_png(path, size, size, 4, grabbed, size * 4)) {
            fprintf(stderr, "glyphs: couldn't write %s\n", path);
            return 1;
        }
    }
    return 0;
}
