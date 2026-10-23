/* Home+ native: the Control Center's lists (Picture Mode, Inputs) and its gallery (Add a Control). */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "home.h"

/* A list control's list (Picture's modes, the inputs) opens in the Control Center's place, like
 * tvOS's: a picture mode applies there and then (and the tick moves); Back goes back. */
#define CC_LIST_ROWS 8

void cc_list_texts(void)
{
    tex_free(&S.cc.list_title);
    S.cc.list_title = text_render(S.f_semi, 24, S.cc.list == 1 ? "Picture Mode" : "Inputs", 0, &S.cc.ltw, &S.cc.lth);
    S.cc.list_n = S.cc.list == 1 ? S.cc.nmodes : S.cc.ninputs;
    if (S.cc.list_n > 24) S.cc.list_n = 24;
    for (int i = 0; i < 24; i++) {
        tex_free(&S.cc.list_row[i]);
        tex_free(&S.cc.list_val[i]);
        if (i >= S.cc.list_n) continue;
        const char *name = S.cc.list == 1 ? S.cc.modes[i].name : S.cc.inputs[i].name;
        const char *val = S.cc.list == 1 ? (i == S.cc.mode ? "\xE2\x9C\x93" : "") : S.cc.inputs[i].plugged ? "" : "Nothing Connected";
        S.cc.list_row[i] = text_render(S.f_medium, CC_LIST_PX, name, 300, &S.cc.lrw[i], &S.cc.lrh[i]);
        if (val[0]) S.cc.list_val[i] = text_render(S.f_medium, val[0] == '\xE2' ? CC_LIST_PX : CC_DETAIL_PX, val, 0, &S.cc.lvw[i], &S.cc.lvh[i]);
    }
}

/* The gallery's layout: each group's name, then its controls four to a row, buttons with their names
 * under them (as iOS 18's Add a Control shows them). Wider than the grid (its right edge the grid's),
 * so names read from the couch */
#define GAL_W 680
#define GAL_PAD 24
#define GAL_HEAD 66
#define GAL_GROUP 52    /* a group's name, near the top: room between it and its controls */
#define GAL_GROUP_TOP 6
#define GAL_BTN 72
#define GAL_ROW 144
#define GAL_NAME_PX 20
#define GAL_NAME_GAP 16 /* between neighbours' names: a longer one takes two lines */
static void cc_gallery_build(void)
{
    for (int i = 0; i < MAX_CAT; i++) tex_free(&S.cc.gal_label[i][0]), tex_free(&S.cc.gal_label[i][1]);
    for (int i = 0; i < 6; i++) tex_free(&S.cc.gal_head[i]);
    tex_free(&S.cc.list_title);
    S.cc.list_title = text_render(S.f_semi, 24, "Add a Control", 0, &S.cc.ltw, &S.cc.lth);
    float cw = (GAL_W - 2 * GAL_PAD) / 4.0f, y = GAL_HEAD;
    const char *group = "";
    int col = 0;
    S.cc.gal_n = S.cc.gal_nhead = 0;
    for (int i = 0; i < S.cc.ncat; i++) { /* the catalogue is in its groups' order */
        const __typeof__(S.cc.cat[0]) *e = &S.cc.cat[i];
        if (!e->available || cc_find(e->id) >= 0) continue;
        if (strcmp(e->group, group) && S.cc.gal_nhead < 6) { /* a new group: its name, under the last one's controls */
            if (S.cc.gal_n) y += GAL_ROW;
            group = e->group;
            S.cc.gal_head[S.cc.gal_nhead] = text_render(S.f_semi, 22, group, 0, &S.cc.gal_hw[S.cc.gal_nhead], &S.cc.gal_hh[S.cc.gal_nhead]);
            S.cc.gal_hy[S.cc.gal_nhead++] = y;
            y += GAL_GROUP;
            col = 0;
        } else if (col == 4) {
            y += GAL_ROW;
            col = 0;
        }
        int k = S.cc.gal_n++;
        S.cc.gal_cat[k] = i;
        S.cc.gal_x[k] = GAL_PAD + col * cw + (cw - GAL_BTN) / 2;
        S.cc.gal_y[k] = y;
        int room = (int)cw - GAL_NAME_GAP, w0, h0, split = -1, best = 1 << 30;
        Tex *t = text_render(S.f_medium, GAL_NAME_PX, e->title, 0, &w0, &h0);
        tex_free(&t);
        for (const char *sp = e->title; w0 > room && (sp = strchr(sp, ' ')); sp++) { /* two lines: the split whose longer line is shortest */
            char first[48];
            int w1, w2, h;
            snprintf(first, sizeof first, "%.*s", (int)(sp - e->title), e->title);
            t = text_render(S.f_medium, GAL_NAME_PX, first, 0, &w1, &h), tex_free(&t);
            t = text_render(S.f_medium, GAL_NAME_PX, sp + 1, 0, &w2, &h), tex_free(&t);
            if ((w1 > w2 ? w1 : w2) < best) best = w1 > w2 ? w1 : w2, split = (int)(sp - e->title);
        }
        if (split >= 0) { /* "Dynamic" / "Tone Mapping" */
            char first[48];
            snprintf(first, sizeof first, "%.*s", split, e->title);
            S.cc.gal_label[k][0] = text_render(S.f_medium, GAL_NAME_PX, first, room, &S.cc.gal_lw[k][0], &S.cc.gal_lh[k][0]);
            S.cc.gal_label[k][1] = text_render(S.f_medium, GAL_NAME_PX, e->title + split + 1, room, &S.cc.gal_lw[k][1], &S.cc.gal_lh[k][1]);
        } else {
            S.cc.gal_label[k][0] = text_render(S.f_medium, GAL_NAME_PX, e->title, room, &S.cc.gal_lw[k][0], &S.cc.gal_lh[k][0]);
        }
        col++;
    }
    S.cc.gal_h = (S.cc.gal_n ? y + GAL_ROW : GAL_HEAD + 60) + 4;
}

/* the gallery's control in direction (dx, dy) from the focused one: the nearest that way */
static void cc_gallery_move(int dx, int dy)
{
    int cur = S.cc.list_index, best = -1;
    float bd = 1e9f;
    for (int k = 0; k < S.cc.gal_n; k++) {
        float ax = S.cc.gal_x[k] - S.cc.gal_x[cur], ay = S.cc.gal_y[k] - S.cc.gal_y[cur];
        if (k == cur || (dx && ax * dx <= 1) || (dy && ay * dy <= 1)) continue;
        float d = dx ? fabsf(ax) + 2.5f * fabsf(ay) : fabsf(ay) + 2.5f * fabsf(ax);
        if (d < bd) bd = d, best = k;
    }
    if (best >= 0) S.cc.list_index = best;
}

void cc_list_open(int which)
{
    S.cc.list = S.cc.list_kind = which;
    S.cc.list_index = S.cc.list_first = 0;
    if (which == 3) {
        cc_gallery_build();
        return;
    }
    cc_list_texts();
    S.cc.list_index = which == 1 && S.cc.mode >= 0 ? S.cc.mode : 0;
    S.cc.list_first = S.cc.list_index >= CC_LIST_ROWS ? S.cc.list_index - CC_LIST_ROWS + 1 : 0;
}

void cc_list_key(SDL_Keycode k)
{
    if (S.cc.list == 3) { /* the gallery's buttons, in their grid */
        if (k == SDLK_LEFT) cc_gallery_move(-1, 0);
        else if (k == SDLK_RIGHT) cc_gallery_move(1, 0);
        else if (k == SDLK_UP) cc_gallery_move(0, -1);
        else if (k == SDLK_DOWN) cc_gallery_move(0, 1);
        else if (is_back(k)) S.cc.list = 0;
        return;
    }
    if (k == SDLK_UP && S.cc.list_index > 0) S.cc.list_index--;
    else if (k == SDLK_DOWN && S.cc.list_index < S.cc.list_n - 1) S.cc.list_index++;
    else if (is_back(k) && S.cc.list_only) cc_close();
    else if (is_back(k)) S.cc.list = 0;
    if (S.cc.list_index < S.cc.list_first) S.cc.list_first = S.cc.list_index;
    if (S.cc.list_index >= S.cc.list_first + CC_LIST_ROWS) S.cc.list_first = S.cc.list_index - CC_LIST_ROWS + 1;
}

void cc_list_ok(void)
{
    int i = S.cc.list_index;
    if (S.cc.list == 3) { /* add it: the gallery closes, and it has the focus */
        if (i < S.cc.gal_n) {
            S.cc.list = 0;
            cc_add(S.cc.gal_cat[i]);
        }
        return;
    }
    if (i < 0 || i >= S.cc.list_n) return;
    if (S.cc.list == 1) {
        char body[96];
        snprintf(body, sizeof body, "{\"action\":\"pictureMode\",\"value\":\"%s\"}", S.cc.modes[i].id);
        cc_post(body);
    } else {
        cc_close();
        launch(S.cc.inputs[i].app, NULL);
    }
}

/* A list control's list, in the Control Center's place: its name, then the choices (the one on
 * ticked; inputs with nothing behind them say so). in: how far in the panel is (cc_slide) */
void draw_cc_list(float a, float in)
{
    float x = cc_grid_x(), y = cc_grid_y(), w = 4 * CC_U + 3 * CC_G;
    x += cc_slide(in, x);
    int rows = S.cc.list_n < CC_LIST_ROWS ? S.cc.list_n : CC_LIST_ROWS;
    const float rh = CC_LIST_ROW_H, gap = CC_LIST_GAP, head = CC_LIST_HEAD;
    float h = head + (rows ? rows * (rh + gap) - gap : 40) + 14;
    if (!S.reduce_motion) y -= 12 * (1 - a); /* it settles in */
    r_shadow(x, y, w, h, 34, 30, 12, 0.35f * a);
    panel(0, x, y, w, h, 34, 1, 0, a);
    Uint8 al = (Uint8)(255 * a);
    draw_text(S.cc.list_title, S.cc.ltw, S.cc.lth, x + (w - S.cc.ltw) / 2, y + (head - S.cc.lth) / 2 + 3, al);
    for (int r = 0; r < rows; r++) {
        int i = S.cc.list_first + r;
        if (i >= S.cc.list_n) break;
        float ry = y + head + r * (rh + gap), rx = x + 14, rw = w - 28;
        int f = i == S.cc.list_index;
        if (f) r_shape(rx, ry, rw, rh, rh / 2, 245, 245, 247, al, 0, 1, 1);
        Uint8 c = f ? 28 : 255, ra = (Uint8)(f ? al : TEXT_STATIC * al / 255);
        r_image(S.cc.list_row[i], rx + 24, ry + (rh - S.cc.lrh[i]) / 2, S.cc.lrw[i], S.cc.lrh[i], c, c, c, ra, 0);
        if (S.cc.list_val[i]) /* the tick as its name; a detail dimmer */
            r_image(S.cc.list_val[i], rx + rw - 24 - S.cc.lvw[i], ry + (rh - S.cc.lvh[i]) / 2, S.cc.lvw[i], S.cc.lvh[i], c, c, c,
                    S.cc.list_kind == 1 ? ra : (Uint8)(TEXT_SECONDARY * al / 255), 0);
    }
}

/* The gallery, in the Control Center's place: its name, each group's name, and the controls on offer
 * as round buttons with their names under them; the focused one white, like the controls themselves */
void draw_cc_gallery(float a, float in)
{
    float x = RIGHT - GAL_W, y = cc_grid_y(), w = GAL_W, h = S.cc.gal_h;
    x += cc_slide(in, x);
    if (!S.reduce_motion) y -= 12 * (1 - a); /* it settles in */
    r_shadow(x, y, w, h, 34, 30, 12, 0.35f * a);
    panel(0, x, y, w, h, 34, 1, 0, a);
    Uint8 al = (Uint8)(255 * a);
    draw_text(S.cc.list_title, S.cc.ltw, S.cc.lth, x + (w - S.cc.ltw) / 2, y + (GAL_HEAD - S.cc.lth) / 2 + 3, al);
    for (int g = 0; g < S.cc.gal_nhead; g++) /* the group's name, small, on the left like iOS's */
        draw_text(S.cc.gal_head[g], S.cc.gal_hw[g], S.cc.gal_hh[g], x + GAL_PAD + 8, y + S.cc.gal_hy[g] + GAL_GROUP_TOP,
                  (Uint8)(TEXT_TERTIARY * al / 255));
    if (!S.cc.gal_n) { /* nothing left to add */
        static Tex *none;
        static int nw, nh;
        if (!none) none = text_render(S.f_medium, 20, "Every control this TV has is placed.", 0, &nw, &nh);
        draw_text(none, nw, nh, x + (w - nw) / 2, y + GAL_HEAD + 14, (Uint8)(TEXT_SECONDARY * al / 255));
    }
    for (int k = 0; k < S.cc.gal_n; k++) {
        const __typeof__(S.cc.cat[0]) *e = &S.cc.cat[S.cc.gal_cat[k]];
        int f = k == S.cc.list_index;
        float bx = x + S.cc.gal_x[k], by = y + S.cc.gal_y[k], d = GAL_BTN * (f ? 1.08f : 1), ox = bx - (d - GAL_BTN) / 2, oy = by - (d - GAL_BTN) / 2;
        if (f) r_shadow(ox, oy, d, d, d / 2, 14, 6, 0.4f * a);
        panel(0, ox, oy, d, d, d / 2, 1, f, a);
        if (f) r_shape(ox, oy, d, d, d / 2, 245, 245, 247, al, 0, 1, 1);
        Uint8 c = f ? 28 : 255;
        float is = 32 * d / GAL_BTN;
        r_image(cc_glyph(e->icon, 0), ox + (d - is) / 2, oy + (d - is) / 2, is, is, c, c, c, al, 0);
        float ly = by + GAL_BTN + 8;
        for (int l = 0; l < 2 && S.cc.gal_label[k][l]; l++) {
            draw_text(S.cc.gal_label[k][l], S.cc.gal_lw[k][l], S.cc.gal_lh[k][l], bx + GAL_BTN / 2.0f - S.cc.gal_lw[k][l] / 2.0f, ly,
                      (Uint8)(f ? al : TEXT_STATIC * al / 255));
            ly += S.cc.gal_lh[k][l] - 2;
        }
    }
}
