/* Home+ native: input: the remote's keys, the Magic Remote's pointer, and where focus goes. */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>

#include "home.h"

/* the clock capsule: the time, then the circle; its right edge lines up with the dock's */
float top_w(void) { return S.clock_w + 24 + 14 + TOP_DOT + 5; }
float top_x(void) { return RIGHT - top_w(); }

/* Up into the top bar remembers the column it came from, for Down */
void enter_top(int col)
{
    S.zone = ZONE_TOP;
    S.top_index = TOP_SETTINGS;
    S.top_col = col < 0 ? 0 : col >= COLS ? COLS - 1 : col;
}

/* the dock tile in a column, or the last tile when the row is shorter */
static int dock_tile(int col)
{
    return col < S.nitems ? col : S.nitems > 0 ? S.nitems - 1 : 0;
}

/* Where Down from the top bar lands: the dock tile in the column it was entered from */
int top_down_tile(void) { return dock_tile(S.top_col); }

/* rect of the focused element: where a sheet grows from */
void focus_rect(float *x, float *y, float *w, float *h)
{
    if (S.zone == ZONE_TOP) {
        *x = top_x();
        *y = TOP_Y;
        *w = top_w();
        *h = BAR;
    } else if (S.zone == ZONE_GRID && S.focus < S.nitems) {
        *x = LEFT + (S.focus % COLS) * (TILE_W + GAP_X);
        *y = GRID_TOP + (S.focus / COLS) * ROW_H + S.scroll;
        *w = TILE_W;
        *h = TILE_H;
    } else {
        *w = 240;
        *h = 80;
        *x = (W - *w) / 2;
        *y = (H - *h) / 2;
    }
}

static void activate(void)
{
    if (S.zone == ZONE_TOP) {
        cc_open();
        return;
    }
    if (S.zone == 1) {
        App *a = item_app(S.focus);
        if (a && S.rail_index < S.nrail) {
            /* LG's apps can't be sent to a title: open the app and say where the title is */
            char hint[300];
            const char *name = S.rail[S.rail_index].name;
            if (S.rail_source == SRC_FEATURED && S.rail_trending) { /* not the app's: just open it */
                launch(a->id, NULL);
                return;
            }
            if (S.rail_source == SRC_FEATURED) {
                snprintf(hint, sizeof hint, "Search for \xE2\x80\x9C%s\xE2\x80\x9D in %s", name, a->title);
                launch(a->id, hint);
                snprintf(hint, sizeof hint, "Opening %s. Search for \xE2\x80\x9C%s\xE2\x80\x9D.", a->title, name);
            } else {
                snprintf(hint, sizeof hint, "\xE2\x80\x9C%s\xE2\x80\x9D is in %s\xE2\x80\x99s Continue Watching", name, a->title);
                launch(a->id, hint);
                snprintf(hint, sizeof hint, "Opening %s. Look for \xE2\x80\x9C%s\xE2\x80\x9D in Continue Watching.", a->title, name);
            }
            toast(hint, 6000);
        }
        return;
    }
    if (S.moving) {
        S.moving = 0;
        save_layout();
        return;
    }
    if (S.focus < S.nitems && S.items[S.focus] < 0) {
        open_hidden_sheet();
        return;
    }
    App *a = item_app(S.focus);
    if (!a || (S.launching && SDL_GetTicks() - S.launch_at < 3000)) return; /* a second OK while it opens */
    launch(a->id, NULL);
    /* the tile breathes and says so until the app is in front (webOS takes 1-5 s) */
    char msg[200];
    snprintf(msg, sizeof msg, "Opening %s\xE2\x80\xA6", a->title);
    tex_free(&S.launch_tex);
    S.launch_tex = text_render(S.f_medium, 26, msg, TILE_W + 60, &S.launch_w, &S.launch_h);
    S.launching = 1;
    S.launch_item = S.focus;
    S.launch_at = SDL_GetTicks();
}

void move_focus(int dx, int dy)
{
    if (S.zone == ZONE_TOP) {
        if (dy > 0) { /* down: back to the app it came from */
            S.zone = ZONE_GRID;
            S.focus = top_down_tile();
        }
        return;
    }
    if (S.zone == ZONE_RAIL) { /* stepping through the carousel's titles */
        if (dy > 0) S.zone = ZONE_GRID; /* back to its app */
        else if (dy < 0) enter_top(S.focus % COLS);
        else if (dx && S.rail_index + dx >= 0 && S.rail_index + dx < S.nrail) rail_focus(S.rail_index + dx);
        return;
    }
    int n = S.nitems, i = S.focus, next = i;
    if (dx) {
        next = i + dx;
        if (next < 0 || next >= n || next / COLS != i / COLS) return;
    }
    if (dy > 0) {
        next = i + COLS;
        if (next >= n) {
            if ((n - 1) / COLS > i / COLS) next = n - 1;
            else return;
        }
    }
    if (dy < 0) {
        if (i < COLS) {
            /* up from the top row: into its Top Shelf carousel (the chevron), else the clock */
            if (!S.moving) {
                if (rail_active()) S.zone = ZONE_RAIL;
                else enter_top(i);
            }
            return;
        }
        next = i - COLS;
    }
    if (S.moving) {
        if (S.items[next] >= 0) move_item_to(next);
        return;
    }
    S.focus = next;
}

/* LG's SDL 2.0.14 reports remote keys with the *scancode* number in keysym.sym
 * (Right arrow arrives as sym=79 = SDL_SCANCODE_RIGHT). Map those back to keycodes;
 * real keycodes (from a USB keyboard or the desktop build) pass through, and so do
 * the colour keys, which have no SDL keycode. */
static SDL_Keycode remote_key(const SDL_Keysym *ks)
{
    switch (ks->sym) {
    case SDL_SCANCODE_RIGHT: return SDLK_RIGHT;
    case SDL_SCANCODE_LEFT: return SDLK_LEFT;
    case SDL_SCANCODE_DOWN: return SDLK_DOWN;
    case SDL_SCANCODE_UP: return SDLK_UP;
    case SDL_SCANCODE_RETURN: return SDLK_RETURN;
    case SDL_SCANCODE_KP_ENTER: return SDLK_KP_ENTER;
    case SDL_SCANCODE_ESCAPE: return SDLK_ESCAPE;
    case SDL_SCANCODE_BACKSPACE: return SDLK_BACKSPACE;
    case SDL_SCANCODE_AC_BACK: return SDLK_AC_BACK;
    case WEBOS_KEY_BACK: return SDLK_AC_BACK;
    default: return ks->sym;
    }
}

int is_back(SDL_Keycode k)
{
    return k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE;
}

/* keys Home+ acts on; anything else gets logged so it can be mapped */
static int is_mapped(SDL_Keycode k)
{
    switch (k) {
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_LEFT: case SDLK_RIGHT: case SDLK_UP: case SDLK_DOWN:
    case WEBOS_KEY_RED: case WEBOS_KEY_GREEN: case WEBOS_KEY_YELLOW: case WEBOS_KEY_BLUE:
        return 1;
    default:
        return is_back(k);
    }
}

#define PTR_TAKE_PX 40 /* pointer travel (logical px) that hands control back to it */
static int ptr_lx = -1, ptr_ly = -1; /* the pointer's last position */

void keys_take_over(void)
{
    S.ptr_mode = 0;
    S.ptr_ax = ptr_lx;
    S.ptr_ay = ptr_ly;
}

/* An arrow key hides the Magic Remote's pointer; moving the remote brings it back. LG's
 * SDL has SDL_webOSCursorVisibility (its SDL_webOS.h): looked up at run time, since the
 * vendored 2.0.14 headers and desktop SDL don't have it. */
static void hide_pointer(void)
{
    static SDL_bool (*visibility)(SDL_bool);
    static int looked;
    if (!looked) {
        void *self = dlopen(NULL, RTLD_LAZY);
        looked = 1;
        if (self) *(void **)&visibility = dlsym(self, "SDL_webOSCursorVisibility");
    }
    if (visibility && S.cursor_up) visibility(SDL_FALSE);
    S.cursor_up = 0;
}

/* the pointer moved to mx,my: 1 when it's (now) the one in charge */
int pointer_moved(int mx, int my)
{
    ptr_lx = mx;
    ptr_ly = my;
    S.cursor_up = 1;
    if (S.ptr_mode) return 1;
    if (S.ptr_ax < 0) { /* first sighting since the keys took over: just remember it */
        S.ptr_ax = mx;
        S.ptr_ay = my;
        return 0;
    }
    int dx = mx - S.ptr_ax, dy = my - S.ptr_ay;
    if (dx * dx + dy * dy < PTR_TAKE_PX * PTR_TAKE_PX) return 0; /* a hand's tremor */
    S.ptr_mode = 1;
    return 1;
}

void on_key_down(SDL_KeyboardEvent *e)
{
    SDL_Keycode k = remote_key(&e->keysym);
    if (k == WEBOS_KEY_CURSOR_SHOW || k == WEBOS_KEY_CURSOR_HIDE) {
        S.cursor_up = k == WEBOS_KEY_CURSOR_SHOW;
        return;
    }
    if (k != SDLK_RETURN && k != SDLK_KP_ENTER) keys_take_over();
    if (k == SDLK_LEFT || k == SDLK_RIGHT || k == SDLK_UP || k == SDLK_DOWN) {
        S.tick_armed = 1;
        hide_pointer();
    }
    if (S.logged_keys < 40 && !is_mapped(k)) {
        S.logged_keys++;
        S.last_unknown_key = (int)k;
        log_remote("unknown key sym=%d scancode=%d mod=%d", (int)k, (int)e->keysym.scancode, (int)e->keysym.mod);
    }
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
        if (!e->repeat && !S.ok_down) {
            S.ok_down = SDL_GetTicks();
            S.ok_long = 0;
        }
        return;
    }
    if (S.recents.open) {
        sw_key(k);
        return;
    }
    if (S.sheet.open) {
        if (k == SDLK_UP && S.sheet.index > 0) S.sheet.index--;
        else if (k == SDLK_DOWN && S.sheet.index < S.sheet.n - 1) S.sheet.index++;
        else if ((k == SDLK_LEFT || k == SDLK_RIGHT) && S.sheet.it[S.sheet.index].swatches) {
            int i = S.icon_tint + (k == SDLK_RIGHT ? 1 : -1);
            if (i >= 0 && i < NTINTS) icon_tint_set(i), snd_play(SND_TICK);
        } else if (is_back(k) && !S.sheet.locked) {
            if (S.sheet.page == PAGE_KEYS) keys_close();
            if (S.sheet.parent) settings_page(S.sheet.parent, S.sheet.parent_index);
            else sheet_dismiss();
        }
        return;
    }
    if (S.cc.open) {
        if (S.cc.list) cc_list_key(k);
        else if (k == SDLK_LEFT) cc_move(-1, 0);
        else if (k == SDLK_RIGHT) cc_move(1, 0);
        else if (k == SDLK_UP) cc_move(0, -1);
        else if (k == SDLK_DOWN) cc_move(0, 1);
        else if (is_back(k)) {
            if (S.cc.picked >= 0) { /* put it down */
                S.cc.picked = -1;
                cc_save();
            } else if (S.cc.editing) { /* done (every change is already saved) */
                S.cc.editing = 0;
                cc_pack();
                if (S.cc.index >= S.cc.n) S.cc.index = S.cc.n ? S.cc.n - 1 : 0;
            } else {
                cc_close();
            }
        }
        return;
    }
    switch (k) {
    case SDLK_LEFT: move_focus(-1, 0); break;
    case SDLK_RIGHT: move_focus(1, 0); break;
    case SDLK_UP: move_focus(0, -1); break;
    case SDLK_DOWN: move_focus(0, 1); break;
    /* colour keys: Recents, Settings, the focused app's options, move it (again: done) */
    case WEBOS_KEY_RED: if (!S.moving) sw_open(); break;
    case WEBOS_KEY_GREEN: if (!S.moving) open_settings_sheet(); break;
    case WEBOS_KEY_YELLOW: if (!S.moving) open_app_sheet(); break;
    case WEBOS_KEY_BLUE:
        if (S.moving) { S.moving = 0; save_layout(); }
        else if (S.zone == ZONE_GRID && item_app(S.focus)) S.moving = 1;
        break;
    default:
        if (is_back(k)) {
            if (S.zone != ZONE_GRID) S.zone = ZONE_GRID;
            else if (S.moving) { S.moving = 0; save_layout(); }
            else if (S.focus) S.focus = 0;
            else sw_open(); /* already home: Back shows what's open */
        }
    }
}

/* OK let go (the remote's key, or the pointer's click): select what has focus. */
void ok_release(void)
{
    int was_long = S.ok_long;
    S.ok_down = 0;
    if (was_long) return;
    snd_play(SND_SELECT);
    if (S.recents.open) {
        sw_ok();
    } else if (S.sheet.open) {
        if (!S.sheet.n) return; /* "Setting up…": nothing to press */
        SheetItem it = S.sheet.it[S.sheet.index];
        S.at_page = S.sheet.page; /* a Settings action rebuilds its page in place */
        S.at_index = S.sheet.index;
        S.at_parent_index = S.sheet.parent_index;
        if (!S.at_page || it.act == ACT_CANCEL || it.act == ACT_LAUNCH_ID) sheet_dismiss(); /* else its page is rebuilt */
        if (it.act == ACT_LAUNCH_ID) launch(it.id, NULL);
        else run_action(it.act, it.arg, it.id);
    } else if (S.cc.open) {
        cc_activate();
    } else {
        activate();
    }
}

void on_key_up(SDL_KeyboardEvent *e)
{
    SDL_Keycode k = remote_key(&e->keysym);
    if ((k != SDLK_RETURN && k != SDLK_KP_ENTER) || !S.ok_down) return;
    ok_release();
}

static int hit_item(int mx, int my)
{
    for (int i = 0; i < S.nitems; i++) {
        float x = LEFT + (i % COLS) * (TILE_W + GAP_X);
        float y = GRID_TOP + (i / COLS) * ROW_H + S.scroll;
        if (mx >= x && mx < x + TILE_W && my >= y && my < y + TILE_H) return i;
    }
    return -1;
}


/* A sheet's button under the pointer (-1: none); *outside: not on the panel at all.
 * Mirrors draw_sheet()'s layout, from where the panel was last drawn. */
int sheet_button_at(int mx, int my, int *outside)
{
    *outside = mx < S.sheet.px || mx > S.sheet.px + S.sheet.pw || my < S.sheet.py || my > S.sheet.py + S.sheet.ph;
    if (*outside || !S.sheet.n) return -1;
    int nfoot = sheet_footer(), nlist = S.sheet.n - nfoot, rows = sheet_rows(nfoot);
    float step = SHEET_BH + SHEET_GAP, y0 = S.sheet.py + sheet_head(), list_h = rows * step;
    if (mx < S.sheet.px + PAD || mx > S.sheet.px + S.sheet.pw - PAD) return -1;
    if (my >= y0 && my < y0 + list_h) {
        float r = (my - y0) / step + S.sheet.scroll;
        int i = (int)floorf(r);
        if (r - i <= SHEET_BH / step && i >= 0 && i < nlist) return i;
        return -1;
    }
    for (int k = 0; k < nfoot; k++) {
        float fy = y0 + list_h + k * step;
        if (my >= fy && my < fy + SHEET_BH) return nlist + k;
    }
    return -1;
}

/* What's under the pointer; with apply, it gets focus (as if the remote moved
 * there). Returns 1 when it's something OK would act on. */
int pointer_at(int mx, int my, int apply)
{
    if (S.recents.open) {
        if (S.recents.zooming || !S.recents.n) return 0;
        float bx = (W - 360) / 2.0f, by = H - SAFE_Y - SHEET_BH; /* Close All */
        if (mx >= bx && mx < bx + 360 && my >= by && my < by + SHEET_BH) {
            if (apply) S.recents.close_all = 1;
            return 1;
        }
        for (int i = 0; i < S.recents.n; i++) {
            float cx = W / 2.0f + S.recents.c[i].x * SW_PITCH;
            if (fabsf(mx - cx) < SW_W * 0.42f && fabsf((float)(my - SW_CY)) < SW_H * 0.42f) {
                if (apply) S.recents.index = i, S.recents.close_all = 0;
                return 1;
            }
        }
        return 0;
    }
    if (S.sheet.open) {
        int outside, i = sheet_button_at(mx, my, &outside);
        if (i >= 0 && apply) S.sheet.index = i;
        return i >= 0;
    }
    if (S.cc.open) {
        for (int i = 0; i < S.cc.n + (S.cc.editing ? 2 : 0) && !S.cc.list && S.cc.picked < 0; i++) {
            float x, y, w, h;
            if (!cc_rect(i, &x, &y, &w, &h)) continue;
            if (mx >= x && mx < x + w && my >= y && my < y + h) {
                if (apply && S.cc.index != i) S.cc.index = i, cc_problem(NULL);
                return 1;
            }
        }
        return 0;
    }
    if (S.moving) return 1; /* OK anywhere puts the moved app down */
    if (S.scrolled < 0.5f) { /* the top bar and Continue Watching are only there at the top */
        if (mx >= top_x() && mx < top_x() + top_w() && my >= TOP_Y && my < TOP_Y + BAR) {
            if (apply && S.zone != ZONE_TOP) enter_top(S.focus % COLS);
            return 1;
        }
    }
    int h = hit_item(mx, my);
    if (h < 0) return 0;
    if (apply) {
        S.focus = h;
        S.zone = ZONE_GRID;
        /* the light comes from where the pointer is, relative to the tile */
        float dx = mx - (LEFT + (h % COLS) * (TILE_W + GAP_X) + TILE_W / 2.0f);
        float dy = my - (GRID_TOP + (h / COLS) * ROW_H + S.scroll + TILE_H / 2.0f);
        float l = sqrtf(dx * dx + dy * dy);
        if (l > 8) {
            S.ptr_x = dx / l;
            S.ptr_y = dy / l;
            S.ptr_at = SDL_GetTicks();
        }
    }
    return 1;
}

/* window -> logical coordinates (the GL drawable may not be 1920x1080) */
void to_logical(int *x, int *y)
{
    int ww, wh;
    SDL_GetWindowSize(S.win, &ww, &wh);
    if (ww > 0 && wh > 0) {
        *x = *x * W / ww;
        *y = *y * H / wh;
    }
}
