/* Home+ native: sheets: the menus that grow out of the button that opened them, and shrink back into it. */
#include <math.h>
#include <string.h>

#include "home.h"

void sheet_close(void)
{
    tex_free(&S.sheet.title);
    tex_free(&S.sheet_big);
    tex_free(&S.sheet_qr);
    S.sheet_qr_s = 0;
    S.media_poll = 0;
    for (int i = 0; i < S.sheet.nbody; i++) tex_free(&S.sheet.body[i]);
    S.sheet.nbody = 0;
    S.sheet.locked = 0;
    S.trakt_poll = 0;
    for (int i = 0; i < S.sheet.n; i++) {
        tex_free(&S.sheet.it[i].tex);
        tex_free(&S.sheet.it[i].val);
        S.sheet.it[i].chevron = S.sheet.it[i].check = S.sheet.it[i].swatches = 0;
    }
    S.sheet.open = 0;
    S.sheet.n = 0;
    S.sheet.page = S.sheet.parent = S.sheet.parent_index = S.sheet.wide = 0;
}

/* Closes the sheet the way it opened: its glass shrinks back into the button. */
void sheet_dismiss(void)
{
    if (S.sheet.open && S.sheet.pw > 0 && !S.reduce_motion) {
        S.ghost.on = 1;
        S.ghost.t = S.sheet.t > 1 ? 1 : S.sheet.t;
        S.ghost.tv = 0;
        S.ghost.fx = S.sheet.fx; S.ghost.fy = S.sheet.fy; S.ghost.fw = S.sheet.fw; S.ghost.fh = S.sheet.fh;
        S.ghost.px = S.sheet.px; S.ghost.py = S.sheet.py; S.ghost.pw = S.sheet.pw; S.ghost.ph = S.sheet.ph;
        S.ghost.pr = S.sheet.pr;
    }
    sheet_close();
}

void sheet_open(const char *title)
{
    int was_open = S.sheet.open || S.ghost.on; /* replacing a sheet (even one just closing): no morph */
    S.ghost.on = 0;
    sheet_close();
    if (was_open) {
        S.sheet.t = 1; /* swapping one sheet for another: no morph */
        S.sheet.tv = 0;
    } else {
        S.sheet.t = S.sheet.tv = 0;
        focus_rect(&S.sheet.fx, &S.sheet.fy, &S.sheet.fw, &S.sheet.fh);
    }
    if (S.reduce_motion) S.sheet.t = 1, S.sheet.tv = 0; /* appears in place, no growing */
    S.sheet.open = 1;
    S.sheet.index = S.sheet.first = 0;
    S.sheet.scroll = S.sheet.scroll_v = 0;
    S.sheet.title = text_render(S.f_semi, SHEET_TITLE_PX, title, 900, &S.sheet.tw, &S.sheet.th);
}

void sheet_add(const char *label, Action act, int arg, int destructive)
{
    if (S.sheet.n >= MAX_SHEET) return;
    SheetItem *it = &S.sheet.it[S.sheet.n++];
    it->act = act;
    it->arg = arg;
    it->destructive = destructive;
    it->tex = text_render(S.f_medium, SHEET_BUTTON_PX, label, 600, &it->w, &it->h);
}

/* A button for one app: by its id, not its place in S.apps, which a new app list rebuilds while the
 * sheet is open (a relaunch, or the list after a deletion) */
void sheet_add_app(const char *label, Action act, const char *id, int destructive)
{
    int n = S.sheet.n;
    sheet_add(label, act, 0, destructive);
    if (S.sheet.n > n) copy_id(S.sheet.it[n].id, id);
}

/* A few lines of explanation under the sheet's title; lines are split on '\n'. */
void sheet_body(const char *text)
{
    char line[300];
    const char *p = text;
    while (*p && S.sheet.nbody < SHEET_BODY_LINES) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = 0;
        int i = S.sheet.nbody++;
        S.sheet.body[i] = text_render(S.f_medium, SHEET_BODY_PX, line, 900, &S.sheet.bw[i], &S.sheet.bh[i]);
        p = e ? e + 1 : p + n;
    }
}

/* Cancel and the bulk actions after a list stay put while a long list scrolls. */
int sheet_footer(void)
{
    int k = 0;
    while (k < S.sheet.n) {
        Action a = S.sheet.it[S.sheet.n - 1 - k].act;
        if (a != ACT_CANCEL && a != ACT_UNHIDE_ALL) break;
        k++;
    }
    return k;
}

static float sheet_body_h(void)
{
    float h = 0;
    for (int i = 0; i < S.sheet.nbody; i++) h += S.sheet.bh[i] + 6;
    return h ? h + 18 : 0;
}

float sheet_head(void)
{
    return PAD + S.sheet.th + 24 + sheet_body_h() + (S.sheet_qr_s ? S.sheet_qr_s + 24 : 0) + (S.sheet_big ? S.sheet_big_h + 24 : 0);
}

/* List rows on screen: all of them when they fit, else as many as fit above the
 * footer in a panel no taller than the safe area. */
int sheet_rows(int nfoot)
{
    int fit = (int)((H - 2 * SAFE_Y - sheet_head() - PAD + SHEET_GAP) / (SHEET_BH + SHEET_GAP)) - nfoot;
    int nlist = S.sheet.n - nfoot;
    return nlist < fit ? nlist : fit < 1 ? 1 : fit;
}

void draw_sheet_button(const SheetItem *it, int focused, float bx, float y, float bw, float k, Uint8 ca)
{
    static const Uint8 RED[2][3] = { { RED_ON_GLASS }, { RED_ON_WHITE } };
    const float BH = SHEET_BH;
    Uint8 a = (Uint8)(ca * k);
    /* Settings rows (a value or a page): label on the left, value and › on the right */
    int row = it->val || it->chevron || it->swatches;
    float tx = row ? bx + 36 : bx + (bw - it->w) / 2, ty = y + (BH - it->h) / 2;
    Uint8 lr, lg, lb, la;
    if (focused) { /* tvOS: the focused button turns opaque white with a dark label */
        r_shadow(bx, y, bw, BH, BH / 2, 18, 8, 0.4f * a / 255);
        r_shape(bx, y, bw, BH, BH / 2, 245, 245, 247, a, 0, 1, 1);
        lr = 28, lg = 28, lb = 30;
        la = a;
    } else { /* dark under the light glass: the label reads over white tiles too */
        r_shape(bx, y, bw, BH, BH / 2, 12, 12, 16, (Uint8)(95 * a / 255), 0, 1, 1);
        r_shape(bx, y, bw, BH, BH / 2, 255, 255, 255, (Uint8)(18 * a / 255), 0, 1, 1);
        lr = lg = lb = 255;
        la = (Uint8)(TEXT_STATIC * a / 255);
    }
    if (it->destructive) lr = RED[!!focused][0], lg = RED[!!focused][1], lb = RED[!!focused][2];
    r_image(it->tex, tx, ty, (float)it->w, (float)it->h, lr, lg, lb, la, 0);
    if (it->check) { /* the choice that's on: a tick on the right, in its label's colour */
        static Tex *tick;
        static int kw, kh;
        if (!tick) tick = text_render(S.f_medium, SHEET_BUTTON_PX, "\xE2\x9C\x93", 0, &kw, &kh);
        r_image(tick, bx + bw - 36 - kw, y + (BH - kh) / 2, (float)kw, (float)kh, lr, lg, lb, la, 0);
    }
    if (!row) return;
    if (it->swatches) { /* the colour that's on ringed, in the label's colour */
        for (int i = 0; i < NTINTS; i++) {
            float cx = swatch_x(bx, bw, i), cy = y + BH / 2, d = SWATCH_D;
            const Uint8 *c = TINTS[i].rgb;
            r_shape(cx - d / 2, cy - d / 2, d, d, d / 2, c[0], c[1], c[2], a, 0, 1, 1);
            if (i == S.icon_tint) r_shape(cx - d / 2 - 6, cy - d / 2 - 6, d + 12, d + 12, d / 2 + 6, lr, lg, lb, a, 2.5f, 1, 1);
        }
        return;
    }
    float right = bx + bw - 36;
    if (it->chevron) {
        static Tex *chev;
        static int cw, ch;
        if (!chev) chev = text_render(S.f_semi, 34, "\xE2\x80\xBA", 0, &cw, &ch);
        Uint8 c = focused ? 120 : 255;
        r_image(chev, right - cw, y + (BH - ch) / 2 - 2, (float)cw, (float)ch, c, c, focused ? 126 : 255, (Uint8)(focused ? a : TEXT_TERTIARY * a / 255), 0);
        right -= cw + 14;
    }
    if (it->val) {
        Uint8 c = focused ? 88 : 255;
        r_image(it->val, right - it->vw, y + (BH - it->vh) / 2, (float)it->vw, (float)it->vh, c, c, focused ? 94 : 255,
                (Uint8)(focused ? a : TEXT_TERTIARY * a / 255), 0);
    }
}

void draw_sheet(void)
{
    if (!S.sheet.open) return;
    float t = S.sheet.t, tc = t < 0 ? 0 : t > 1 ? 1 : t;
    r_fill(0, 0, 0, (Uint8)(80 * tc));
    const float BH = SHEET_BH, BGAP = SHEET_GAP, rad = BH / 2 + PAD; /* corners concentric with the buttons */
    float big = S.sheet_big ? S.sheet_big_h + 24 : 0;
    int nfoot = sheet_footer(), nlist = S.sheet.n - nfoot, rows = sheet_rows(nfoot);
    float pw = S.sheet.wide ? 900 : SHEET_W; /* Settings: room for a label and its value */
    if (S.sheet.tw + 2 * PAD > pw) pw = S.sheet.tw + 2 * PAD;
    for (int i = 0; i < S.sheet.nbody; i++) if (S.sheet.bw[i] + 2 * PAD > pw) pw = S.sheet.bw[i] + 2 * PAD;
    float ph = sheet_head() + (rows + nfoot) * (BH + BGAP) - (S.sheet.n ? BGAP : 24) + PAD;
    float px = (W - pw) / 2, py = (H - ph) / 2;
    S.sheet.px = px; S.sheet.py = py; S.sheet.pw = pw; S.sheet.ph = ph; S.sheet.pr = rad;
    /* grows out of the button it came from on a spring, and its lensing
     * materialises as it does (Apple: glass appears by bending light) */
    float gx = S.sheet.fx + (px - S.sheet.fx) * t, gy = S.sheet.fy + (py - S.sheet.fy) * t;
    float gw = S.sheet.fw + (pw - S.sheet.fw) * t, gh = S.sheet.fh + (ph - S.sheet.fh) * t;
    float r0 = fminf(S.sheet.fw, S.sheet.fh) / 2, gr = r0 + (rad - r0) * t;
    if (gw < 1 || gh < 1) return;
    r_shadow(gx, gy, gw, gh, gr, 60, 24, 0.45f * tc);
    /* tinted dark like tvOS menus: bright tiles behind it stay quiet under the text */
    Glass g = { gx, gy, gw, gh, gr, 1, tc, -0.6f, -0.8f, 0, 1, SHEET_TINT };
    glass(&g);
    float c = (t - 0.55f) / 0.45f; /* content fades in once the panel is nearly there */
    Uint8 ca = (Uint8)(255 * (c < 0 ? 0 : c > 1 ? 1 : c));
    if (!ca) return;
    draw_text(S.sheet.title, S.sheet.tw, S.sheet.th, px + (pw - S.sheet.tw) / 2, py + PAD, (Uint8)(TEXT_STATIC * ca / 255));
    float y = py + PAD + S.sheet.th + 24;
    for (int i = 0; i < S.sheet.nbody; i++) {
        draw_text(S.sheet.body[i], S.sheet.bw[i], S.sheet.bh[i], px + (pw - S.sheet.bw[i]) / 2, y, (Uint8)(TEXT_SECONDARY * ca / 255));
        y += S.sheet.bh[i] + 6;
    }
    if (S.sheet.nbody) y += 18;
    if (S.sheet_qr_s) { /* on white, like a printed code: phones read it at any brightness */
        float q = (float)S.sheet_qr_s, qx = px + (pw - q) / 2;
        r_shape(qx, y, q, q, 24, 255, 255, 255, ca, 0, 1, 1);
        if (S.sheet_qr) r_image(S.sheet_qr, qx + 18, y + 18, q - 36, q - 36, 255, 255, 255, ca, 0);
        y += q + 24;
    }
    if (S.sheet_big) {
        draw_text(S.sheet_big, S.sheet_big_w, S.sheet_big_h, px + (pw - S.sheet_big_w) / 2, y, ca);
        y += big;
    }
    float bx = px + PAD, bw = pw - 2 * PAD, list_h = rows * (BH + BGAP);
    float more_above = fminf(fmaxf(S.sheet.scroll, 0), 1), more_below = fminf(fmaxf(nlist - rows - S.sheet.scroll, 0), 1);
    for (int i = 0; i < nlist; i++) {
        float ry = y + (i - S.sheet.scroll) * (BH + BGAP);
        /* rows sliding out of the list fade out; the rows at its edges dim while
         * more lie beyond them */
        float out = fmaxf(y - ry, ry + BH - (y + list_h - BGAP));
        float k = 1 - fmaxf(out, 0) / (BH * 0.6f);
        if (k <= 0) continue;
        float top = fminf(fmaxf((ry - y) / (BH + BGAP), 0), 1), bottom = fminf(fmaxf((y + list_h - BGAP - ry - BH) / (BH + BGAP), 0), 1);
        k *= 1 - 0.55f * more_above * (1 - top);
        k *= 1 - 0.55f * more_below * (1 - bottom);
        draw_sheet_button(&S.sheet.it[i], i == S.sheet.index, bx, ry, bw, k, ca);
    }
    y += list_h;
    for (int i = nlist; i < S.sheet.n; i++, y += BH + BGAP) draw_sheet_button(&S.sheet.it[i], i == S.sheet.index, bx, y, bw, 1, ca);
}

void draw_ghost(void)
{
    if (!S.ghost.on) return;
    float t = S.ghost.t < 0 ? 0 : S.ghost.t;
    r_fill(0, 0, 0, (Uint8)(80 * t));
    float gx = S.ghost.fx + (S.ghost.px - S.ghost.fx) * t, gy = S.ghost.fy + (S.ghost.py - S.ghost.fy) * t;
    float gw = S.ghost.fw + (S.ghost.pw - S.ghost.fw) * t, gh = S.ghost.fh + (S.ghost.ph - S.ghost.fh) * t;
    float r0 = fminf(S.ghost.fw, S.ghost.fh) / 2, gr = r0 + (S.ghost.pr - r0) * t;
    if (gw < 1 || gh < 1) return;
    r_shadow(gx, gy, gw, gh, gr, 60, 24, 0.45f * t);
    Glass g = { gx, gy, gw, gh, gr, 1, t, -0.6f, -0.8f, 0, fminf(1, t * 1.5f), SHEET_TINT };
    glass(&g);
}
