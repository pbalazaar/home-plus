/* Home+ native: uniform tiles: an icon's logo cut out of its background, and the cover made from it. */
#include <stdlib.h>
#include <string.h>

#include "home.h"

/* ------------------------------------------------------------------ uniform tiles */

/* Like tvOS app icons, every tile is the icon's own background colour from edge
 * to edge with its logo centred at the same visual size. LG icons are small
 * squares with their own background, so: find that colour on the border, key
 * it out, trim to the logo and size it to a common target. Icons without a
 * solid background keep their whole picture, at one size, with round corners;
 * logo-only icons (transparent) get one graphite tile (light if the logo is dark).
 * Apps without artwork get a cover too, made from the icon. icon.c works all that
 * out as the icon arrives, off the main thread; here it goes onto the GPU. */

/* App a's tile from what icon_prepare made of its icon: its logo and its cover, shaded as Settings ->
 * Calm Colours says now. */
void icon_apply(App *a, const IconPrep *ip)
{
    a->ink_ref = ip->ink_ref;
    a->ink_span = ip->ink_span;
    a->ink_tint = (float)ip->ink_tint;
    tex_free(&a->logo_tex);
    a->logo_tex = r_tex(ip->crop, ip->cw, ip->ch);
    a->logo_w = ip->logo_w;
    a->logo_h = ip->logo_h;
    a->logo_r = ip->logo_r;
    memcpy(a->col, ip->col, sizeof a->col);
    a->has_col = 1;
    a->fullbleed = ip->fullbleed;
    unsigned char *out = ip->has_cover ? malloc((size_t)COVER_W * COVER_H * 4) : NULL;
    if (!out) return;
    icon_cover(ip, S.vivid_covers, out);
    tex_free(&a->cover_tex);
    a->cover_tex = r_tex(out, COVER_W, COVER_H);
    free(out);
}

/* the same on the main thread, for an icon that came without it (no memory to make it on the way) */
void make_logo(App *a, const unsigned char *px, int w, int h)
{
    IconPrep *ip = icon_prepare(px, w, h);
    if (!ip) return;
    icon_apply(a, ip);
    free(ip);
}

/* a network worker's, as each icon arrives (net_prepare) */
void prepare_icon(Result *r)
{
    r->prep = icon_prepare(r->rgba, r->w, r->h);
}
