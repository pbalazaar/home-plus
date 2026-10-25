/* Home+ native: util.c. Which media source an app's Top Shelf shows: by its id or name, and the Jellyfin and
 * Plex apps from Homebrew Channel whose names don't say so. `make -C native test` builds and runs it. */
#include <stdio.h>
#include <string.h>

#include "home.h"

/* what util.c's other helpers reach for, not needed here */
State S;
Uint32 SDL_GetTicks(void) { return 0; }
void net_post(const char *path, const char *body, int tag) {}
void r_tex_free(Tex **t) {}
Tex *text_render(Font *f, float px, const char *utf8, int max_w, int *out_w, int *out_h) { return NULL; }

static int failures;

static void check(const char *id, const char *title, int want)
{
    App a;
    memset(&a, 0, sizeof a);
    snprintf(a.id, sizeof a.id, "%s", id);
    snprintf(a.title, sizeof a.title, "%s", title);
    int got = media_source(&a);
    if (got != want) {
        failures++;
        printf("  FAIL: %s (%s) is source %d, not %d\n", id, title, got, want);
    }
}

int main(void)
{
    /* by name, as the store's builds differ */
    check("io.strem.tv", "Stremio", SRC_STREMIO);
    check("org.jellyfin.webos", "Jellyfin", SRC_JELLYFIN);
    check("cdp-30", "Plex", SRC_PLEX);
    check("com.plexapp.webos", "Plex: Stream Movies & TV", SRC_PLEX);
    /* Homebrew Channel's other clients (issue #37: Litefin) */
    check("org.litefin.app", "Litefin", SRC_JELLYFIN);
    check("org.moonfin.webos", "Moonfin", SRC_JELLYFIN);
    check("com.breezyfin.app", "Breezyfin", SRC_JELLYFIN);
    check("com.ocenfin.app", "OcenFin", SRC_JELLYFIN);
    check("com.beb.plxnative", "PlxNative", SRC_PLEX);
    /* and everything else has none */
    check("netflix", "Netflix", SRC_NONE);
    check("org.litefin.app.extra", "Something Else", SRC_NONE);
    if (!media_source(NULL)) printf("ok: no app, no source\n");
    else failures++;

    printf(failures ? "util: %d failed\n" : "util: all passed\n", failures);
    return failures != 0;
}
