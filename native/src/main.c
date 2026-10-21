/* Home+ native — a tvOS-style home screen for webOS, drawn with SDL2 + GLES2.
 *
 * Rendered on the GPU by our own GLES2 renderer (render.c): Liquid Glass panels,
 * rounded shapes and text, a few dozen quads a frame. Animations run at the
 * display refresh rate, and when nothing moves the loop sleeps.
 *
 * Data comes from the Home+ service's local API (see service/service.js). */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

/* LG's libSDL2 is the Wayland build; the vendored generic headers only show
 * SDL_SysWMinfo's Wayland members when asked */
#define SDL_VIDEO_DRIVER_WAYLAND 1
#include <SDL_syswm.h>

#include "home.h"

State S;

void request_data(void)
{
    if (!S.layout_loaded) net_get_json("/api/layout", TAG_LAYOUT, 0, 0);
    net_get_json("/api/apps", TAG_APPS, 0, 0);
    rail_request(SRC_STREMIO);
    if (want_source() > SRC_STREMIO) rail_request(want_source());
    net_get_json("/api/system", TAG_SYSTEM, 0, 0);
    net_get_json("/api/home", TAG_HOME, 0, 0);
    net_events(TAG_EVENTS); /* the Settings key, through the service (once) */
    net_get_json("/api/prefs", TAG_PREFS, 0, 0);
}

/* what the Control Center over another app can ask Home+ to open */
enum { OPEN_SETTINGS = 1, OPEN_RECENTS, OPEN_ACCESSIBILITY };
static void open_asked(int what)
{
    if (what == OPEN_SETTINGS) open_settings_sheet();
    else if (what == OPEN_RECENTS) sw_open();
    else if (what == OPEN_ACCESSIBILITY) settings_page(PAGE_ACCESS, 0);
}

/* ------------------------------------------------------------------ results */

static void handle_result(Result *r)
{
    switch (r->tag) {
    case TAG_WAKE:
        break;
    case TAG_LAYOUT:
        if (r->status == 200) parse_layout(r->body);
        break;
    case TAG_STATUS:
        on_status(r);
        break;
    case TAG_ELEVATE:
        on_elevate(r);
        break;
    case TAG_RESTART:
        break; /* the service exits; setup_retry wakes it and checks again */
    case TAG_UNINSTALL:
        if (r->status == 200) {
            sheet_open("Home+ Is Being Removed");
            sheet_body("Press Home to go back to LG\xE2\x80\x99s home screen.");
            S.sheet.locked = 1;
        } else {
            S.setup = SETUP_DONE;
            sheet_close();
            toast("Couldn\xE2\x80\x99t remove Home+", 4000);
        }
        break;
    case TAG_APPS:
        if (r->status == 200 && parse_apps(r->body)) {
            S.service_ok = 1;
            S.wake_tries = 0; /* (the next time it fails, it gets its tries again) */
        } else {
            S.service_ok = 0;
            /* service not up yet (or no apps in its answer): start it, then retry */
            if (S.wake_tries++ < 10) {
                net_wake_service();
                S.next_retry = SDL_GetTicks() + 2000;
            }
            if (S.wake_tries > 10 && !S.napps && S.setup == SETUP_DONE) { /* tried: not placeholders for good, but why and Try Again */
                S.toast_until = 0;
                setup_set(SETUP_FAILED);
            } else {
                toast(S.wake_tries < 10 ? "Starting the Home+ service\xE2\x80\xA6" : "The Home+ service isn\xE2\x80\x99t responding. Restart the TV, or reinstall Home+.", 600000);
            }
        }
        if (S.service_ok && S.toast_until > SDL_GetTicks() + 100000) S.toast_until = 0;
        break;
    case TAG_STREMIO:
        if (r->status == 200) parse_continue(r->body, r->index > SRC_NONE && r->index < SRC_COUNT ? r->index : SRC_STREMIO);
        else if (r->index == S.rail_req) S.rail_req = SRC_NONE;
        break;
    case TAG_CONTROL:
        if (r->status == 200) {
            parse_control(r->body);
        } else if (S.cc.ran >= 0 && S.cc.ran < S.cc.n) { /* a control the TV refused shakes, says so, and shows how things are */
            S.cc.it[S.cc.ran].shake = SDL_GetTicks();
            cc_problem("The TV didn\xE2\x80\x99t do that");
            net_get_json("/api/control", TAG_CONTROL, 0, 0);
        } else if (r->status) {
            toast("The TV didn\xE2\x80\x99t do that", 3000);
        }
        S.cc.ran = -1;
        break;
    case TAG_FEATURED:
        S.feat_req = -1;
        if (r->generation == S.generation && r->index >= 0 && r->index < S.napps) {
            if (r->status == 200) parse_featured(r->body, &S.apps[r->index]);
            else S.apps[r->index].feat = 3; /* not this time */
        }
        break;
    case TAG_SYSTEM:
        if (r->status == 200) parse_system(r->body);
        break;
    case TAG_RUNNING:
        if (r->status == 200) sw_fill(r->body);
        else if (S.recents.loading) S.recents.loading = 0, S.recents.failed = 1;
        break;
    case TAG_DELETED: /* its tile went when you chose Delete: if the TV said no, it comes back */
        if (r->status != 200) {
            cJSON *o = r->body ? cJSON_Parse(r->body) : NULL;
            char msg[300];
            snprintf(msg, sizeof msg, "Couldn\xE2\x80\x99t delete it: %s", o && jstr(o, "errorText")[0] ? jstr(o, "errorText") : "the TV didn\xE2\x80\x99t answer");
            cJSON_Delete(o);
            for (int i = 0; i < S.napps; i++) S.apps[i].deleted = 0;
            rebuild_items();
            toast(msg, 4000);
        } else {
            net_get_json("/api/apps", TAG_APPS, 0, 0); /* the list without it, once webOS has caught up */
        }
        break;
    case TAG_CLOSED:
        if (r->status != 200) toast("Couldn\xE2\x80\x99t close that app", 2500);
        break;
    case TAG_EVENTS:
        if (r->body && strstr(r->body, "\"controlCenter\"")) settings_key();
        if (r->body && strstr(r->body, "\"inputs\"")) input_key();
        if (r->body) { /* asked for from the Control Center over another app, which brings Home+ to the front */
            int want = strstr(r->body, "\"settings\"") ? OPEN_SETTINGS : strstr(r->body, "\"recents\"") ? OPEN_RECENTS
                     : strstr(r->body, "\"accessibility\"") ? OPEN_ACCESSIBILITY : 0;
            if (want && S.away) S.asked = want; /* coming back starts at the top: opened after that */
            else if (want) open_asked(want);
        }
        break;
    case TAG_HOME:
    case TAG_HOME_SET:
    case TAG_KEY_SET:
        if (r->status == 200) {
            cJSON *o = cJSON_Parse(r->body);
            if (o) {
                snprintf(S.home_mode, sizeof S.home_mode, "%s", jstr(o, "mode"));
                S.noflash_active = cJSON_IsTrue(cJSON_GetObjectItem(o, "noFlashActive"));
                S.noflash_available = cJSON_IsTrue(cJSON_GetObjectItem(o, "noFlashAvailable"));
                cJSON *sk = cJSON_GetObjectItem(o, "settingsKey");
                S.settings_key = cJSON_IsTrue(cJSON_GetObjectItem(sk, "on"));
                S.settings_key_available = cJSON_IsTrue(cJSON_GetObjectItem(sk, "available"));
                S.input_key = cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(o, "inputKey"), "on"));
                S.piccap = cJSON_IsTrue(cJSON_GetObjectItem(o, "piccap"));
                if (!strcmp(S.home_mode, "redirect") && !S.noflash_active && r->tag == TAG_HOME_SET)
                    toast("No-flash mode isn\xE2\x80\x99t available on this TV; using standard mode", 5000);
                cJSON_Delete(o);
                if (r->tag == TAG_KEY_SET && r->kind == JOB_JSON && S.sheet.open && S.sheet.page == PAGE_HOME) {
                    int index = S.sheet.index, parent_index = S.sheet.parent_index; /* the buttons as they are */
                    settings_page(PAGE_HOME, index);
                    S.sheet.parent_index = parent_index;
                }
            }
        } else if (r->tag == TAG_KEY_SET && r->kind == JOB_POST) {
            /* the Settings or Input button wasn't changed (LG Input Hook's file isn't JSON, say): why, then how they are */
            cJSON *o = r->body ? cJSON_Parse(r->body) : NULL;
            char msg[300];
            snprintf(msg, sizeof msg, "Couldn\xE2\x80\x99t change the button: %s", o && jstr(o, "errorText")[0] ? jstr(o, "errorText") : "the service didn\xE2\x80\x99t answer");
            cJSON_Delete(o);
            toast(msg, 5000);
            net_get_json("/api/home", TAG_KEY_SET, 0, 0);
        }
        break;
    case TAG_PREFS:
        if (r->status == 200) {
            cJSON *o = cJSON_Parse(r->body);
            S.sounds_off = o && cJSON_IsFalse(cJSON_GetObjectItem(o, "sounds"));
            snd_enable(!S.sounds_off);
            S.reduce_motion = o && cJSON_IsTrue(cJSON_GetObjectItem(o, "reduceMotion"));
            S.reduce_transparency = o && cJSON_IsTrue(cJSON_GetObjectItem(o, "reduceTransparency"));
            S.cc_blur_video = o && cJSON_IsTrue(cJSON_GetObjectItem(o, "ccBlurVideo"));
            double dm = jnum(o, "dimMinutes"), om = jnum(o, "offMinutes"); /* -1: never */
            S.idle_dim_ms = dm > 0 ? (Uint32)(dm * 60000) : dm < 0 ? IDLE_NEVER : 0;
            S.idle_off_ms = om > 0 ? (Uint32)(om * 60000) : om < 0 ? IDLE_NEVER : 0;
            int c24 = o && cJSON_IsTrue(cJSON_GetObjectItem(o, "clock24"));
            if (c24 != S.clock24) S.clock24 = c24, S.clock_min = -1;
            S.orbit_off = o && cJSON_IsFalse(cJSON_GetObjectItem(o, "pixelShift"));
            S.vivid_covers = o && cJSON_IsFalse(cJSON_GetObjectItem(o, "calmColours"));
            S.shelf_still = o && cJSON_IsFalse(cJSON_GetObjectItem(o, "shelfTurns"));
            const char *look = o ? jstr(o, "iconLook") : "";
            S.icon_look = !strcmp(look, "dark") ? LOOK_DARK : !strcmp(look, "clear") ? LOOK_CLEAR : !strcmp(look, "tinted") ? LOOK_TINTED : LOOK_DEFAULT;
            const char *tint = o ? jstr(o, "iconTint") : "";
            S.icon_tint = TINT_INDIGO;
            for (int i = 0; i < NTINTS; i++)
                if (!strcasecmp(tint, TINTS[i].name)) S.icon_tint = i;
            cJSON_Delete(o);
        }
        break;
    case TAG_MEDIA_LINK:
    case TAG_MEDIA_STATUS: /* Stremio, Jellyfin and Plex sign-in */
        on_media_signin(r);
        break;
    case TAG_TRAKT_LINK:
    case TAG_TRAKT_STATUS:
        on_trakt_signin(r);
        break;
    case TAG_ICON:
        if (r->generation == S.generation && r->index < S.napps) {
            App *a = &S.apps[r->index];
            if (r->rgba) {
                make_logo(a, r->rgba, r->w, r->h); /* already shrunk to ICON_FIT while decoding */
                a->icon_state = 2;
            } else {
                a->icon_state = 3;
            }
        }
        break;
    case TAG_ART:
        if (r->generation == S.generation && r->index < S.napps) {
            App *a = &S.apps[r->index];
            a->art_tex = r->rgba ? r_tex(r->rgba, r->w, r->h) : NULL;
            a->art_state = r->rgba ? 2 : 3; /* on show already: it fades in over the glow */
        }
        break;
    case TAG_QR:
        if (r->generation == S.qr_gen && S.sheet.open && S.sheet_qr_s && r->rgba) {
            tex_free(&S.sheet_qr);
            S.sheet_qr = r_tex(r->rgba, r->w, r->h);
        }
        break;
    case TAG_RAIL_LOGO:
    case TAG_RAIL_BG:
        if (r->generation == S.rail_generation && r->index < S.nrail) {
            RailItem *it = &S.rail[r->index];
            Tex *t = r->rgba ? r_tex(r->rgba, r->w, r->h) : NULL;
            if (r->tag == TAG_RAIL_LOGO) {
                tex_free(&it->logo_tex);
                it->logo_tex = t, it->logo_state = t ? 2 : 3, it->lw = r->w, it->lh = r->h;
            } else {
                tex_free(&it->bg_tex);
                it->bg_tex = t, it->bg_state = t ? 2 : 3;
            }
        }
        break;
    case TAG_LAUNCH:
        if (r->status != 200 || (r->body && strstr(r->body, "\"returnValue\":false"))) {
            S.away = 0;
            S.launching = 0;
            if (S.recents.zooming) sw_reset();
            toast("Couldn\xE2\x80\x99t open that app", 3500);
        }
        break;
    }
}

/* the clock capsule: the time, then the circle; its right edge lines up with the dock's */
static float top_w(void) { return S.clock_w + 24 + 14 + TOP_DOT + 5; }
static float top_x(void) { return RIGHT - top_w(); }

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
static int top_down_tile(void) { return dock_tile(S.top_col); }

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

static void move_focus(int dx, int dy)
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

static void go_home(void)
{
    sheet_close();
    sw_reset();
    if (S.moving) { S.moving = 0; save_layout(); }
    S.zone = 0;
    S.focus = 0;
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

static void keys_take_over(void)
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
static int pointer_moved(int mx, int my)
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

static void on_key_down(SDL_KeyboardEvent *e)
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
static void ok_release(void)
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

static void on_key_up(SDL_KeyboardEvent *e)
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
static int sheet_button_at(int mx, int my, int *outside)
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
static int pointer_at(int mx, int my, int apply)
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

/* ------------------------------------------------------------------ animation */

/* OLED protection: after a while without input the screen dims (all but the
 * focused tile, like tvOS), then fades to black and stops drawing. */

static float special_s, special_v; /* focus animation of the "Hidden" tile */

static void update_clock(void)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    int m = t->tm_hour * 60 + t->tm_min;
    if (m == S.clock_min && S.clock_tex) return;
    char buf[16];
    int h12 = t->tm_hour % 12 ? t->tm_hour % 12 : 12;
    if (S.clock24) snprintf(buf, sizeof buf, "%02d:%02d", t->tm_hour, t->tm_min);
    else snprintf(buf, sizeof buf, "%d:%02d %s", h12, t->tm_min, t->tm_hour < 12 ? "AM" : "PM");
    tex_free(&S.clock_tex);
    S.clock_tex = text_render(S.f_semi, HEADING_PX, buf, 0, &S.clock_w, &S.clock_h);
    S.clock_min = m;
}

/* Advances every animated value once per frame. Drawing only reads them: the
 * scene is drawn twice, into the glass backdrop and then to the screen. */
static int update(float dt)
{
    int anim = 0;
    update_clock();
    int row = S.focus / COLS;
    approach(&S.scrolled, row > 0 ? 1.0f : 0.0f, 10, dt, &anim);
    S.scroll_target = row == 0 ? 0 : 380 - (GRID_TOP + row * ROW_H);
    if (row > 0) {
        /* don't scroll past the point where the last row (and its label) sits at the
         * bottom of the safe area: a short last stretch would otherwise stop mid-screen */
        int rows = (S.nitems + COLS - 1) / COLS;
        float lowest = (H - SAFE_Y) - (GRID_TOP + (rows - 1) * ROW_H + TILE_H + 60);
        if (S.scroll_target < lowest) S.scroll_target = lowest;
    }
    spring(&S.scroll, &S.scroll_v, S.scroll_target, 0.5f, 1.0f, dt, &anim);

    /* the Top Shelf belongs to the last top-row app focused, also while it shows
     * Continue Watching: scrolled below the dock, its picture stays behind, blurred */
    if (S.focus < COLS) S.shelf_cur = S.focus;
    /* the chevron shows while Up from the focused app goes into its Continue Watching */
    int chev = S.zone == ZONE_GRID && S.focus < COLS && rail_active() && !S.sheet.open && !S.recents.open && !S.moving;
    approach(&S.chevron_a, chev ? 1.0f : 0.0f, 8, dt, &anim);
    int want = want_source();
    if (want && want != SRC_FEATURED && want != S.rail_source && want != S.rail_req && S.service_ok) rail_request(want);
    App *sa = shelf_app();
    if (sa && !media_source(sa) && S.tmdb_configured && S.service_ok && S.feat_req < 0 &&
        (sa->feat == 0 || (sa->feat == 2 && !rail_for(sa))))
        featured_request(sa); /* a streaming service? (the service keeps its list) */
    if (rail_active()) {
        Uint32 now = SDL_GetTicks();
        int nx = (S.rail_index + 1) % S.nrail;
        /* it turns every 10 s while its app has focus, nothing covers it and the screen is awake
         * (unless Settings says it stays still) */
        int turning = !S.shelf_still && S.zone == ZONE_GRID && S.nrail > 1 && !S.sheet.open && !S.recents.open && !S.moving &&
                      S.idle_stage == 0;
        if (!turning) S.rail_next = 0;
        else if (!S.rail_next) S.rail_next = now + ROTATE_MS;
        else if (now >= S.rail_next && S.rail[nx].bg_state != 1 && S.rail[nx].logo_state != 1) { /* the next is ready */
            rail_focus(nx);
            nx = (S.rail_index + 1) % S.nrail;
            S.rail_next = now + ROTATE_MS;
        }
        /* pictures: the selected title's first, then both of its neighbours, loaded ahead so
         * turning or stepping through never waits; and those still on show. The rest are let
         * go (~8 MB each on this GPU), with their requests that haven't started. */
        int pv = (S.rail_index + S.nrail - 1) % S.nrail, order[3] = { S.rail_index, nx, pv };
        RailItem *sel = &S.rail[S.rail_index];
        if (sel->logo_state == 1) net_bump(TAG_RAIL_LOGO, S.rail_index, S.rail_generation); /* asked for as a neighbour */
        if (sel->bg_state == 1) net_bump(TAG_RAIL_BG, S.rail_index, S.rail_generation);
        for (int k = 0; k < 3; k++) {
            RailItem *it = &S.rail[order[k]];
            if (it->bg[0] && it->bg_state == 0) {
                it->bg_state = 1;
                net_get_image(it->bg, TAG_RAIL_BG, order[k], S.rail_generation, 0, W, H);
            }
            if (it->logo[0] && it->logo_state == 0) {
                it->logo_state = 1;
                net_get_image(it->logo, TAG_RAIL_LOGO, order[k], S.rail_generation, 0, 640, 240);
            }
        }
        for (int i = 0; i < S.nrail; i++) {
            if (i == S.rail_index || i == nx || i == pv || i == S.page_rail || i == S.prev_rail || pic_rail(i)) continue;
            RailItem *it = &S.rail[i];
            if (it->bg_state == 1 && net_drop(TAG_RAIL_BG, i, S.rail_generation)) it->bg_state = 0;
            if (it->logo_state == 1 && net_drop(TAG_RAIL_LOGO, i, S.rail_generation)) it->logo_state = 0;
            if (it->bg_tex) tex_free(&it->bg_tex), it->bg_state = 0;
            if (it->logo_tex) tex_free(&it->logo_tex), it->logo_state = 0;
            tex_free(&it->title_tex);
            tex_free(&it->meta_tex);
            it->texts_made = 0;
        }
    } else {
        App *ca = item_app(S.shelf_cur);
        if (ca && ca->art[0] && ca->art_state == 0 && !rail_for(ca) && !carousel_coming(ca)) { /* its own page */
            ca->art_state = 1;
            net_get_image(ca->art, TAG_ART, S.items[S.shelf_cur], S.generation, 0, W, H);
        }
    }
    update_page(dt, &anim);
    if (!S.reduce_motion && !S.offscreen && apps_loading()) anim = 1; /* placeholders shimmer */
    if (!S.reduce_motion && !S.offscreen) /* so do tiles while their icons load */
        for (int i = 0; i < S.nitems && !anim; i++) {
            App *a = item_app(i);
            if (a && a->icon_state == 1) anim = 1;
        }
    /* while OK is down the focused thing sinks a little (1.06), then pops back */
    int pressed = S.ok_down && !S.ok_long && !S.sheet.open && !S.recents.open;
    float on = pressed ? 0.6f : 1.0f, resp = pressed ? 0.12f : 0.35f, damp = pressed ? 1.0f : 0.7f;
    for (int i = 0; i < S.nitems; i++) {
        App *a = item_app(i);
        int f = i == S.focus && S.zone == ZONE_GRID;
        spring(a ? &a->s : &special_s, a ? &a->sv : &special_v, f ? on : 0.0f, f ? resp : 0.35f, f ? damp : 0.7f, dt, &anim);
    }
    for (int i = 0; i < TOP_COUNT; i++) {
        int f = S.zone == ZONE_TOP && S.top_index == i && !S.cc.open; /* open, focus is in the Control Center */
        spring(&S.top_s[i], &S.top_v[i], f ? on : 0.0f, f ? resp : 0.35f, f ? damp : 0.7f, dt, &anim);
    }
    spring(&S.cc.t, &S.cc.tv, S.cc.open ? 1.0f : 0.0f, S.reduce_motion ? 0.15f : 0.32f, 0.9f, dt, &anim);
    if (S.cc.marquee && S.cc.open) anim = 1; /* a long name is scrolling */
    Uint32 tick = SDL_GetTicks();
    for (int i = 0; i < S.cc.n; i++) {
        CCItem *c = &S.cc.it[i];
        int f = S.cc.open && S.cc.index == i && !S.cc.list;
        spring(&c->s, &c->sv, f ? on : 0.0f, f ? resp : 0.35f, f ? damp : 0.7f, dt, &anim);
        if ((c->shake && tick - c->shake < 450) || (c->flash && tick - c->flash < 1800)) anim = 1; /* a shake; a value showing */
    }
    spring(&S.cc.plus_s, &S.cc.plus_v, S.cc.editing && S.cc.index == S.cc.n ? on : 0.0f, 0.35f, 0.7f, dt, &anim);
    spring(&S.cc.reset_s, &S.cc.reset_v, S.cc.editing && S.cc.index == S.cc.n + 1 ? on : 0.0f, 0.35f, 0.7f, dt, &anim);
    spring(&S.cc.list_t, &S.cc.list_v, S.cc.list ? 1.0f : 0.0f, S.reduce_motion ? 0.15f : 0.3f, 0.9f, dt, &anim);
    if (S.cc.open && S.cc.editing && !S.reduce_motion) anim = 1; /* they wobble */
    if (pressed) anim = 1; /* the hold bar fills */
    if (S.launching) {
        Uint32 held = SDL_GetTicks() - S.launch_at;
        if (held < 2500) anim = 1;
        if (held > 10000) {
            char msg[200];
            App *la = item_app(S.launch_item);
            snprintf(msg, sizeof msg, "%s is taking a while to open", la ? la->title : "The app");
            toast(msg, 3500);
            S.launching = 0;
        }
    }
    if (S.moving && !S.reduce_motion) anim = 1; /* wobble */
    if (S.sheet.open) {
        spring(&S.sheet.t, &S.sheet.tv, 1, 0.42f, 0.82f, dt, &anim);
        int nfoot = sheet_footer(), nlist = S.sheet.n - nfoot, rows = sheet_rows(nfoot);
        if (S.sheet.index < nlist) S.sheet.first = keep_in_view(S.sheet.first, S.sheet.index, rows, nlist, rows > 4);
        if (S.sheet.t < 0.05f) S.sheet.scroll = (float)S.sheet.first; /* opens already in place */
        spring(&S.sheet.scroll, &S.sheet.scroll_v, (float)S.sheet.first, 0.5f, 1.0f, dt, &anim);
    }
    sw_update(dt, &anim);
    if (S.ghost.on) {
        spring(&S.ghost.t, &S.ghost.tv, 0, 0.32f, 1.0f, dt, &anim);
        if (S.ghost.t <= 0.002f) S.ghost.on = 0;
    }
    spring(&S.toast_t, &S.toast_v, toast_on() ? 1.0f : 0.0f, 0.4f, 0.85f, dt, &anim);

    /* when focus lands the element sways for a moment and its highlight follows
     * a moving light (from the pointer if it moved lately), then it settles */
    int layer = S.recents.open ? 2 : S.sheet.open ? 1 : 0; /* home, a sheet, the App Switcher */
    int key = layer == 2 ? 2000000 + S.recents.index * 2 + S.recents.close_all
            : layer == 1 ? 1000000 + S.sheet.index
                         : S.zone * 100000 + S.focus * 1000 + S.top_index * 100 + S.rail_index;
    if (key != S.focus_key) {
        /* a tick when a key moved focus: not when a layer opens or closes, nor
         * when the pointer or a reset moves it */
        if (S.tick_armed && layer == S.focus_key / 1000000) snd_play(SND_TICK);
        S.focus_key = key;
        S.sway_t = 0;
    }
    S.tick_armed = 0;
    float bx = -0.6f, by = -0.8f;
    if (S.ptr_at && SDL_GetTicks() - S.ptr_at < 3000) {
        bx = S.ptr_x;
        by = S.ptr_y;
    }
    float amp = 0;
    if (S.reduce_motion) S.sway_t = 1.6f; /* no sway, the light stays put */
    if (S.sway_t < 1.6f) {
        S.sway_t += dt;
        amp = 1 - S.sway_t / 1.6f;
        amp *= amp;
        anim = 1;
    }
    float ang = 0.6f * amp * sinf(S.sway_t * 2 * (float)M_PI / 1.1f);
    S.light_x = bx * cosf(ang) - by * sinf(ang);
    S.light_y = bx * sinf(ang) + by * cosf(ang);

    float dim = S.idle_stage >= 1 ? 1.0f : 0.0f, black = S.idle_stage >= 2 ? 1.0f : 0.0f;
    approach(&S.dim, dim, dim > S.dim ? 2.5f : 10.0f, dt, &anim);
    approach(&S.black, black, black > S.black ? 2.5f : 10.0f, dt, &anim);
    return anim;
}

/* An app's tile drawn large: the Top Shelf's big tile and the App Switcher's
 * cards. Its logo grows with it (up to 3.5x); a tile without one shows its name. */
void draw_big_tile(App *a, float x, float y, float w, float h, float radius, Uint8 alpha, int shadow, int backdrop)
{
    float k = fminf(fminf(w / TILE_W, h / TILE_H), 3.5f);
    if (shadow) r_shadow(x, y, w, h, radius, 40, 20, 0.45f * alpha / 255);
    if (draw_face(a, x, y, w, h, radius, k, alpha, 0, backdrop)) return;
    ensure_big(a);
    if (!a->big) return;
    float f = fminf(fminf(1, w * 0.8f / a->bw), k / 2.5f), bw = a->bw * f, bh = a->bh * f;
    r_image(a->big, x + (w - bw) / 2, y + (h - bh) / 2, bw, bh, 255, 255, 255, alpha, 0);
}

/* The clock capsule, like tvOS 26's: the time, and at its end a circle (the Control
 * Center's glyph, where tvOS shows the profile picture). The whole capsule is the
 * button: focused, it lifts and turns into a white platter. With the Control Center
 * open, it's tvOS's header: the capsule's glass goes, the time stays on its own, and
 * the circle grows into the selected tab, solid white with the glyph dark. */
static void draw_clock(Uint8 alpha, int backdrop)
{
    if (!alpha) return;
    float k = S.top_s[TOP_SETTINGS], g = fminf(fmaxf(k, 0), 1), sc = 1 + 0.08f * k;
    float w0 = top_w(), pw = w0 * sc, ph = BAR * sc, cx = top_x() + w0 / 2, cy = TOP_Y + BAR / 2.0f;
    if (k > 0.01f) {
        float dx, dy;
        sway(&dx, &dy);
        cx += dx * k;
        cy += dy * k;
    }
    float x = cx - pw / 2, y = cy - ph / 2, o = fminf(fmaxf(S.cc.t, 0), 1) * (1 - g);
    if (k > 0.01f && !backdrop) r_shadow(x, y, pw, ph, ph / 2, 16, 6, 0.35f * k * alpha / 255.0f);
    panel(backdrop, x, y, pw, ph, ph / 2, 1, k, alpha / 255.0f * (1 - o));
    if (g > 0.01f) /* focused: an opaque white platter with dark contents, like the sheet buttons */
        r_shape(x, y, pw, ph, ph / 2, 245, 245, 247, (Uint8)(alpha * g), 0, 1, 1);
    Uint8 tc = (Uint8)(255 - 227 * g); /* the time stays its own size, so it stays sharp */
    r_image(S.clock_tex, x + 24 * sc, cy - S.clock_h / 2.0f, S.clock_w, S.clock_h, tc, tc, (Uint8)(255 - 225 * g),
            (Uint8)(alpha * (TEXT_STATIC + (255 - TEXT_STATIC) * g) / 255), 0);
    float dc = x + pw - 5 * sc - TOP_DOT * sc / 2; /* the circle's centre: it grows around it */
    float d = (TOP_DOT + (BAR + 2 - TOP_DOT) * o) * sc, ox = dc - d / 2, oy = cy - d / 2;
    if (o > 0.01f && !backdrop) r_shadow(ox, oy, d, d, d / 2, 14, 5, 0.3f * o * alpha / 255.0f);
    r_shape(ox, oy, d, d, d / 2, 255, 255, 255, (Uint8)(alpha * (0.16f * (1 - g) * (1 - o) + 0.97f * o)), 0, 1, 1);
    if (g > 0.01f) r_shape(ox, oy, d, d, d / 2, 0, 0, 0, (Uint8)(alpha * 0.07f * g), 0, 1, 1);
    float is = S.top_w[TOP_SETTINGS] * sc * (1 + 0.2f * o), gk = fmaxf(g, o);
    Uint8 gc = (Uint8)(255 - 227 * gk);
    r_image(S.top_tex[TOP_SETTINGS], ox + (d - is) / 2, oy + (d - is) / 2, is, is, gc, gc, (Uint8)(255 - 225 * gk),
            (Uint8)(alpha * (0.85f + 0.15f * gk)), 0);
}

static void draw_tile(int i, int backdrop)
{
    App *a = item_app(i);
    float bx = LEFT + (i % COLS) * (TILE_W + GAP_X);
    float by = GRID_TOP + (i / COLS) * ROW_H + S.scroll;
    if (by > H + 40 || by + ROW_H < -40) return;
    int focused = i == S.focus && S.zone == ZONE_GRID;
    float k = a ? a->s : special_s;
    float sc = 1 + (FOCUS_SCALE - 1) * k;
    int launching = S.launching && i == S.launch_item;
    if (launching) { /* opening: breathes a little below its focus size for 2 s, then holds */
        float lt = (SDL_GetTicks() - S.launch_at) / 1000.0f;
        k = 1;
        sc = lt < 2 ? FOCUS_SCALE - 0.015f + 0.015f * cosf(lt * 2 * (float)M_PI * 1.25f) : FOCUS_SCALE;
    }
    float w = TILE_W * sc, h = TILE_H * sc, rad = TILE_R * sc;
    float x = bx - (w - TILE_W) / 2, y = by - (h - TILE_H) / 2 - 6 * k;
    if (S.moving && focused && !S.reduce_motion) x += sinf(SDL_GetTicks() / 40.0f) * 2.0f; /* gentle wobble while moving */
    if (k > 0.01f) {
        float dx, dy;
        sway(&dx, &dy);
        x += dx * k;
        y += dy * k;
    }
    if (k > 0.01f && !backdrop) r_shadow(x, y, w, h, rad, 28, 16, 0.5f * k);
    if (!a) {
        static Tex *ht;
        static int hw, hh;
        r_shape(x, y, w, h, rad, 70, 70, 82, 34, 0, 1, 1);
        if (!ht) ht = text_render(S.f_semi, 28, "\xE2\x80\xA2 \xE2\x80\xA2 \xE2\x80\xA2   Hidden", 0, &hw, &hh);
        draw_text(ht, hw, hh, x + (w - hw) / 2, y + (h - hh) / 2, 205);
    } else if (draw_face(a, x, y, w, h, rad, sc, 255, k, backdrop)) { /* opaque, the icon's own colour (or a look's) */
    } else if (a->icon_state == 1) { /* its icon is on its way: a placeholder */
        float bw = w * 0.46f, bh = h * 0.15f;
        draw_skeleton(x + (w - bw) / 2, y + (h - bh) / 2, bw, bh, bh / 2, 1);
    } else { /* no icon: its name, kept inside the tile */
        ensure_label(a);
        if (a->label) {
            float k = fminf(1, (w - 32) / a->lw), lw = a->lw * k, lh = a->lh * k;
            r_image(a->label, x + (w - lw) / 2, y + (h - lh) / 2, lw, lh, 255, 255, 255, 230, 0);
        }
    }
    if (S.moving && focused) r_shape(x, y, w, h, rad, 255, 255, 255, 200, 2.0f, 1, 1); /* the tile you're moving */
    if (S.zone == ZONE_TOP && i == top_down_tile() && !backdrop) /* where Down from the top bar lands */
        r_shape(x, y, w, h, rad, 255, 255, 255, 128, 2.0f, 1, 1);
    if (focused && S.ok_down && !S.ok_long && !S.sheet.open && !S.recents.open && !S.moving && !backdrop) {
        float p = (SDL_GetTicks() - S.ok_down - 250) / (float)(LONG_PRESS_MS - 250);
        if (p > 0) { /* holding OK: a bar fills until the options open */
            float hw = w * 0.5f, hx = x + (w - hw) / 2, hy = y + h + 10;
            r_shape(hx, hy, hw, 4, 2, 255, 255, 255, 60, 0, 1, 1);
            r_shape(hx, hy, hw * fminf(p, 1), 4, 2, 255, 255, 255, 230, 0, 1, 1);
        }
    }
    if (a && k > 0.01f && !backdrop && (i >= COLS || launching)) { /* like tvOS, the top row has no names */
        ensure_label(a);
        float ly = i < COLS ? DOCK_Y + S.scroll + DOCK_H + 6 : by + TILE_H + 26; /* "Opening…" under the dock */
        Tex *lt = launching && S.launch_tex ? S.launch_tex : a->label;
        int lw = lt == a->label ? a->lw : S.launch_w, lh = lt == a->label ? a->lh : S.launch_h;
        draw_text(lt, lw, lh, bx + (TILE_W - lw) / 2, ly, (Uint8)(TEXT_STATIC * fminf(k, 1)));
    }
}

/* Everything below the overlays. backdrop: the copy the glass refracts. */
static void draw_scene(int backdrop)
{
    Uint8 shelf_a = (Uint8)(255 * (1 - S.scrolled));
    /* The backdrop (what glass refracts, and what's blurred below the dock) keeps the
     * picture at full strength and never holds its own blur: blurring last frame's blur
     * smeared it wider every frame. */
    draw_shelf_art(backdrop ? 255 : shelf_a, backdrop);
    if (S.scrolled > 0.004f && !backdrop) { /* below the dock the picture stays, blurred and dimmed, so the apps stand out */
        r_backdrop_image(1, (Uint8)(255 * S.scrolled));
        r_fill(0, 0, 0, (Uint8)(110 * S.scrolled));
    }
    /* the shelf's content leaves faster than the grid arrives, drifting up behind it */
    r_translate(0, 0.35f * S.scroll);
    draw_shelf_content((Uint8)(255 * fmaxf(0, 1 - 2.5f * S.scrolled)), backdrop);
    r_translate(0, 0);
    /* Scrolling down, the dock's glass grows to the screen's edges and frosts over: it
     * becomes the blurred background the grid sits on, crossfading into it at the end.
     * (Its stand-in in the backdrop stays out meanwhile: blurred, it made a white band.) */
    float dock_y = DOCK_Y + S.scroll, dock_a = 1 - S.scrolled, t = smooth01(0, 1, S.scrolled);
    if (t < 0.999f) {
        float gx = DOCK_X * (1 - t), gy = dock_y * (1 - t), gw = W - 2 * gx, gh = dock_y + DOCK_H + (H - dock_y - DOCK_H) * t - gy;
        float rad = (TILE_R + PAD) * (1 - t), ga = t < 0.75f ? 1 : 1 - (t - 0.75f) / 0.25f;
        if (!backdrop && t <= 0.004f) r_shadow(gx, gy, gw, gh, rad, 40, 14, 0.3f); /* only at rest: it'd be a big moving blur */
        /* clear while it's the dock (its icons are opaque), frosted as soon as it moves: growing
         * towards the whole screen, the clear glass's sharp, colour-split edge (four texture
         * reads a pixel against one) dropped frames on the TV */
        if (!backdrop || t <= 0.004f) panel(backdrop, gx, gy, gw, gh, rad, smooth01(0, 0.15f, t), 0, ga);
    }
    if (!backdrop && S.chevron_a > 0.004f && dock_a > 0.004f) { /* tvOS's chevron, 30 px above the dock */
        float cs = 56, cx = (W - cs) / 2, cy = dock_y - 30 - cs / 2;
        Uint8 ca = (Uint8)(S.chevron_a * dock_a * 255);
        r_image(S.chevron, cx, cy + 2, cs, cs, 0, 0, 0, (Uint8)(ca * 0.35f), 0); /* a soft shadow, for bright pictures */
        r_image(S.chevron, cx, cy, cs, cs, 255, 255, 255, (Uint8)(ca * 0.7f), 0);
    }
    int dimmed = !backdrop && S.dim > 0.004f, top = S.zone == ZONE_TOP;
    if (!(dimmed && top)) draw_clock((Uint8)(255 * (1 - S.scrolled)), backdrop);
    if (apps_loading()) /* the apps are on their way: placeholders in the dock */
        for (int c = 0; c < COLS; c++) draw_skeleton(LEFT + c * (TILE_W + GAP_X), GRID_TOP + S.scroll, TILE_W, TILE_H, TILE_R, 1);
    for (int i = 0; i < S.nitems; i++)
        if (i != S.focus) draw_tile(i, backdrop);
    if (dimmed) r_fill(0, 0, 0, (Uint8)(S.dim * 115)); /* OLED: idle dims all but what has focus */
    if (S.launching && !backdrop) { /* the rest steps back while the app opens */
        float d = (SDL_GetTicks() - S.launch_at - 250) / 200.0f;
        if (d > 0) r_fill(0, 0, 0, (Uint8)(77 * fminf(d, 1)));
    }
    if (S.focus < S.nitems) draw_tile(S.focus, backdrop);
    if (dimmed && top) draw_clock((Uint8)(255 * (1 - S.scrolled)), backdrop);
    if (S.moving) { /* how to finish, in a capsule at the top while an app is being moved */
        static Tex *hint;
        static int hw, hh;
        if (!hint) hint = text_render(S.f_medium, HINT_PX, "Arrows move it  \xC2\xB7  OK puts it down", 0, &hw, &hh);
        float pw = hw + 56, px = (W - pw) / 2;
        panel(backdrop, px, SAFE_Y, pw, BAR, BAR / 2, 1, 0, 1);
        draw_text(hint, hw, hh, px + 28, SAFE_Y + (BAR - hh) / 2.0f, TEXT_STATIC);
    }
}

static void draw_toast(void)
{
    float t = S.toast_t < 0 ? 0 : S.toast_t > 1 ? 1 : S.toast_t;
    if (!S.toast_tex || t < 0.01f) return;
    float tw = S.toast_w + 72, th = S.toast_h + 40, tx = (W - tw) / 2, ty = H - SAFE_Y - th + (1 - S.toast_t) * 40;
    r_shadow(tx, ty, tw, th, th / 2, 30, 12, 0.4f * t);
    Glass g = { tx, ty, tw, th, th / 2, 1, t, -0.6f, -0.8f, 0, t, 0, 0, 0, 0 };
    glass(&g);
    draw_text(S.toast_tex, S.toast_w, S.toast_h, (W - S.toast_w) / 2, ty + 20, (Uint8)(245 * t));
}

extern int homeplus_screenshot(const char *path);

static int render(float dt, const char *shot)
{
    int anim = update(dt);
    /* the Top Shelf picture alone: what the dock and top bar refract (the icons
     * sit on the glass), and the blurred background below the dock */
    r_backdrop_begin();
    draw_shelf_art(255, 1);
    r_backdrop_end();

    r_frame_begin(8, 8, 11);
    draw_scene(0);
    if (S.sheet.open || S.ghost.on || S.toast_t > 0.01f || S.recents.t > 0.004f || S.cc.t > 0.004f) { /* overlays refract the whole scene */
        r_backdrop_begin();
        draw_scene(1);
        if (cc_dim() > 0.002f) r_fill(0, 0, 0, (Uint8)(255 * cc_dim())); /* (what the glass refracts steps back too) */
        r_backdrop_end();
    }
    draw_switcher();
    draw_control_center();
    draw_ghost();
    draw_sheet();
    draw_toast();
    if (S.black > 0.004f) r_fill(0, 0, 0, (Uint8)(S.black * 255)); /* OLED: idle fades to black */
    if (shot) homeplus_screenshot(shot);
    r_present();
    return anim;
}

/* ------------------------------------------------------------------ setup */

static int load_assets(void)
{
    char *base = SDL_GetBasePath();
    char path[1024];
    snprintf(path, sizeof path, "%sassets/fonts/Inter-Medium.ttf", base ? base : "./");
    S.f_medium = font_load(path);
    snprintf(path, sizeof path, "%sassets/fonts/Inter-SemiBold.ttf", base ? base : "./");
    S.f_semi = font_load(path);
    snprintf(path, sizeof path, "%sassets/fonts/Inter-Bold.ttf", base ? base : "./");
    S.f_bold = font_load(path);
    SDL_free(base);
    if (!S.f_medium || !S.f_semi || !S.f_bold) {
        fprintf(stderr, "homeplus: fonts missing\n");
        return 0;
    }
    S.scrim_v = gfx_gradient(1, 0.0f, 0.6f);
    S.scrim_h = gfx_gradient(0, 0.72f, 0.0f); /* text over daylight artwork stays readable */
    S.scrim_top = gfx_gradient(1, 0.45f, 0.0f);
    S.heading_tex = text_render(S.f_semi, HEADING_PX, "Continue Watching \xC2\xB7 Stremio", 0, &S.hw, &S.hh);
    S.rail_hint = text_render(S.f_medium, HINT_PX, "OK opens Stremio", 0, &S.rail_hint_w, &S.rail_hint_h);
    static const char *const top_names[TOP_COUNT] = { "Control Center" };
    for (int i = 0; i < TOP_COUNT; i++) {
        char hint[96];
        snprintf(hint, sizeof hint, "OK opens %s  \xC2\xB7  Down returns to your apps", top_names[i]);
        S.top_hint[i] = text_render(S.f_medium, HINT_PX, hint, 0, &S.top_hw[i], &S.top_hh[i]);
    }
    S.sub_tex = text_render(S.f_medium, HINT_PX, "OK opens  \xC2\xB7  Hold for options", 0, &S.sw, &S.sh);
    S.sub_first_tex = text_render(S.f_medium, HINT_PX, "OK opens  \xC2\xB7  Hold for options  \xC2\xB7  Back for Recents", 0,
                                  &S.sfw, &S.sfh); /* Back on the first app opens Recents */
    S.top_tex[TOP_SETTINGS] = gfx_icon(ICON_CONTROLS, 64); /* the Control Center's glyph: drawn at 30 px, 64 stays sharp */
    S.cc.picked = S.cc.ran = -1; /* the controls come from the service (their glyphs are made when first drawn) */
    S.chevron = gfx_icon(ICON_CHEVRON, 64);
    S.top_w[TOP_SETTINGS] = S.top_h[TOP_SETTINGS] = 30;
    return 1;
}

/* ------------------------------------------------------------------ idle wait */

/* LG's SDL 2.0.14 has no real blocking wait: SDL_WaitEventTimeout checks for
 * events every 10 ms, 100 wake-ups a second even while hidden. On the TV the
 * loop sleeps in poll() on the Wayland socket and on a pipe that queued events
 * (network results) write to. While hidden it still pumps SDL every 100 ms:
 * LG's SDL finishes SAM's relaunch, i.e. the Home button, in SDL_PumpEvents. */
#define HIDDEN_PUMP_MS 100

static int wake_pipe[2] = { -1, -1 };

/* the Wayland socket SDL reads events from, or -1 (not Wayland, e.g. the desktop preview) */
static int wayland_fd(void)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(S.win, &info) || info.subsystem != SDL_SYSWM_WAYLAND) return -1;
    void *lib = SDL_LoadObject("libwayland-client.so.0"); /* SDL has it loaded already */
    int (*get_fd)(struct wl_display *) = lib ? (int (*)(struct wl_display *))SDL_LoadFunction(lib, "wl_display_get_fd") : NULL;
    return get_fd ? get_fd(info.info.wl.display) : -1;
}

/* runs on whichever thread queues an SDL event and wakes the loop out of poll() */
static int SDLCALL wake_loop(void *userdata, SDL_Event *e)
{
    char c = 1;
    ssize_t n = write(wake_pipe[1], &c, 1); /* fails only when full: the loop is awake then */
    (void)userdata;
    (void)e;
    (void)n;
    return 0;
}

static void start_idle_wait(void)
{
    S.wl_fd = wayland_fd();
    if (S.wl_fd < 0 || pipe(wake_pipe) != 0) {
        S.wl_fd = -1;
        return;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(wake_pipe[i], F_SETFL, O_NONBLOCK);
        fcntl(wake_pipe[i], F_SETFD, FD_CLOEXEC); /* not inherited by luna-send */
    }
    SDL_AddEventWatch(wake_loop, NULL);
}

/* drew: a frame went out since the last wait (drawing can leave events read off
 * the socket but not yet handed to SDL) */
static int wait_event(SDL_Event *e, int timeout_ms, int drew)
{
    if (S.wl_fd < 0) return SDL_WaitEventTimeout(e, timeout_ms < 0 ? -1 : timeout_ms > 0 ? timeout_ms : 1);
    if (drew && SDL_PollEvent(e)) return 1;
    struct pollfd fds[2] = { { S.wl_fd, POLLIN, 0 }, { wake_pipe[0], POLLIN, 0 } };
    poll(fds, 2, timeout_ms);
    char buf[64];
    while (read(wake_pipe[0], buf, sizeof buf) > 0) {
    }
    return SDL_PollEvent(e);
}

/* How long the loop may sleep: until what's due next (a retry, a Trakt check, a
 * long press, the toast ending, the next OLED stage, closing the audio, the
 * next minute for the clock and pixel orbit while something is on screen), or
 * the hidden pump. -1 = until something happens. */
/* when the next OLED idle stage is due (0: none will come) */
static Uint32 idle_due(Uint32 dim_ms, Uint32 off_ms)
{
    Uint32 next = S.idle_stage == 0 && dim_ms != IDLE_NEVER ? dim_ms : off_ms;
    if (S.idle_stage == 0 && off_ms != IDLE_NEVER && off_ms < next) next = off_ms;
    return next == IDLE_NEVER ? 0 : S.last_input + next;
}

static int next_wake_ms(void)
{
    Uint32 now = SDL_GetTicks();
    Uint32 dim_ms = S.idle_dim_ms ? S.idle_dim_ms : IDLE_DIM_MS, off_ms = S.idle_off_ms ? S.idle_off_ms : IDLE_OFF_MS;
    int visible = !S.offscreen && S.idle_stage < 2;
    Uint32 turn = visible && S.rail_next ? (S.rail_next - 2500 > now ? S.rail_next - 2500 : S.rail_next) : 0;
    int waiting = visible && (S.want_app != S.page_app || S.want_rail != S.page_rail); /* the Top Shelf's next page */
    Uint32 page = waiting ? S.want_at + (now - S.want_at < SKEL_MS ? SKEL_MS : PAGE_WAIT_MS) : 0;
    /* the Top Shelf turns, and shows its next page, in a frame: one due before the last frame was that
     * frame's to do, and isn't waited for again (a turn waiting for its next picture happens when the
     * picture comes, which wakes the loop) */
    if (turn && (Sint32)(turn - S.drawn_at) <= 0) turn = 0;
    if (page && (Sint32)(page - S.drawn_at) <= 0) page = 0;
    Uint32 due[11] = { S.setup_retry, S.next_retry, S.trakt_poll, S.media_poll, S.ok_down && !S.ok_long ? S.ok_down + LONG_PRESS_MS : 0, turn,
                      S.toast_tex && S.toast_until > now ? S.toast_until : 0,
                      S.offscreen || S.idle_stage >= 2 ? 0 : idle_due(dim_ms, off_ms),
                      snd_active() ? now + 1000 : 0, visible ? now + (60000 - now % 60000) : 0, page };
    int wait = -1;
    for (int i = 0; i < 11; i++) {
        if (!due[i]) continue;
        int d = (Sint32)(due[i] - now) > 0 ? (int)(due[i] - now) : 0;
        if (wait < 0 || d < wait) wait = d;
    }
    if (visible) { /* the clock's next minute */
        time_t tt = time(NULL);
        int d = (60 - (int)(tt % 60)) * 1000 + 5;
        if (wait < 0 || d < wait) wait = d;
    }
    if (S.offscreen && (wait < 0 || wait > HIDDEN_PUMP_MS)) wait = HIDDEN_PUMP_MS;
    return wait;
}

/* Another app is in front: free the big pictures except the one on show (they
 * come back on demand; ~8 MB each on this GPU). */
static void release_images(void)
{
    App *keep = item_app(S.shelf_cur);
    for (int i = 0; i < S.napps; i++) {
        App *a = &S.apps[i];
        int shown = (S.pic_under.kind == PIC_ART && S.pic_under.i == i) || (S.pic_top.kind == PIC_ART && S.pic_top.i == i);
        if (a == keep || shown || !a->art_tex) continue;
        tex_free(&a->art_tex);
        a->art_state = 0;
    }
    for (int i = 0; i < S.nrail; i++) {
        RailItem *it = &S.rail[i];
        if (i == S.rail_index || i == S.page_rail || pic_rail(i)) continue;
        if (it->bg_tex) tex_free(&it->bg_tex), it->bg_state = 0;
        if (it->logo_tex) tex_free(&it->logo_tex), it->logo_state = 0;
    }
}

/* Another app is in front (LG's SDL may say so as HIDDEN, MINIMIZED or
 * DIDENTERBACKGROUND): stop drawing, release the audio and the big pictures. */
static void go_offscreen(void)
{
    if (S.offscreen) return;
    S.offscreen = 1;
    S.keys_until = 0;
    sw_reset(); /* an app opened from it is in front now */
    S.launching = 0;
    snd_suspend();
    release_images();
}

static void come_back(void)
{
    if (S.away) go_home(); /* back from another app (or the Home button): start at the top */
    S.away = 0;
    if (S.asked) open_asked(S.asked); /* ...unless the Control Center over that app asked for something */
    S.asked = 0;
    S.offscreen = 0;
    S.last_input = SDL_GetTicks();
    S.idle_stage = 0;
    if (S.setup == SETUP_DONE) request_data();
    /* only a setup that stopped on an error starts over; one in progress carries on */
    else if (S.setup == SETUP_NO_HBC || S.setup == SETUP_DENIED || S.setup == SETUP_FAILED || S.setup == SETUP_OLD) setup_retry_now();
}

/* Any input resets the OLED idle timer. Returns 1 when it only woke a dark
 * screen, so that press does nothing else (key: its release is dropped too). */
int wake(int key)
{
    int dark = S.black > 0.01f;
    S.last_input = SDL_GetTicks();
    S.idle_stage = 0;
    if (dark && key) S.swallow_up = 1;
    return dark;
}

/* window -> logical coordinates (the GL drawable may not be 1920x1080) */
static void to_logical(int *x, int *y)
{
    int ww, wh;
    SDL_GetWindowSize(S.win, &ww, &wh);
    if (ww > 0 && wh > 0) {
        *x = *x * W / ww;
        *y = *y * H / wh;
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    /* LG's SDL: let the app handle Back/Exit itself instead of the system closing it. */
    SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_BACK", "true");
    SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_EXIT", "true");
    SDL_SetHint("SDL_WEBOS_CURSOR_SLEEP_TIME", "5000");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "homeplus: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    r_gl_attributes();
    S.win = SDL_CreateWindow("Home+", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, W, H,
                             SDL_WINDOW_OPENGL | (getenv("HOMEPLUS_SCREENSHOT") ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP) | SDL_WINDOW_SHOWN);
    if (!S.win) {
        fprintf(stderr, "homeplus: window: %s\n", SDL_GetError());
        return 1;
    }
    if (!r_init(S.win, W, H)) {
        fprintf(stderr, "homeplus: renderer init failed\n");
        return 1;
    }
    if (!load_assets()) return 1;
    snd_init();

    net_start();
    S.ptr_ax = S.ptr_ay = -1;
    start_idle_wait();
    {
        int dw, dh, ww, wh;
        SDL_GL_GetDrawableSize(S.win, &dw, &dh);
        SDL_GetWindowSize(S.win, &ww, &wh);
        log_remote("native up: renderer=gles2 drawable=%dx%d window=%dx%d idle=%s", dw, dh, ww, wh, S.wl_fd >= 0 ? "poll" : "sdl");
    }
    S.shelf_cur = S.page_app = S.prev_app = S.want_app = -1;
    S.page_rail = S.prev_rail = S.want_rail = -2;
    S.feat_req = -1;
    S.last_input = SDL_GetTicks();
    S.orbit_k = -1;
    setup_check(); /* loads the data once the service is up and elevated */

    const char *shot = getenv("HOMEPLUS_SCREENSHOT"); /* host preview: render then save a PNG */
    /* tests: run as on the TV for this long, then say how often the loop woke and drew in its last 10 s */
    Uint32 rest_ms = getenv("HOMEPLUS_REST_REPORT_MS") ? (Uint32)atoi(getenv("HOMEPLUS_REST_REPORT_MS")) : 0;
    unsigned rest_wakes = 0, rest_frames = 0;
    Uint32 shot_at = shot ? SDL_GetTicks() + (Uint32)atoi(getenv("HOMEPLUS_SCREENSHOT_MS") ? getenv("HOMEPLUS_SCREENSHOT_MS") : "2500") : 0;
    extern void homeplus_preview_input(Uint32 elapsed);

    int running = 1, animating = 1, drew = 1;
    Uint32 last = SDL_GetTicks(), start = last;
    while (running) {
        SDL_Event e;
        /* sleep while idle or hidden; wake on input, a queued event or what's due next */
        int got;
        if (shot || (animating && !S.offscreen)) got = SDL_PollEvent(&e);
        else if ((Sint32)(S.keys_until - SDL_GetTicks()) > 0) got = SDL_WaitEventTimeout(&e, 10);
        else got = wait_event(&e, next_wake_ms(), drew);
        drew = 0;
        int dirty = got;
        while (got) {
            switch (e.type) {
            case SDL_QUIT: running = 0; break;
            case SDL_KEYDOWN:
                if (e.key.keysym.sym == WEBOS_KEY_CURSOR_HIDE) { /* the pointer fell asleep: not input */
                    keys_take_over();
                    S.cursor_up = 0;
                    break;
                }
                S.keys_until = SDL_GetTicks() + 600;
                if (wake(1)) break;
                on_key_down(&e.key);
                break;
            case SDL_KEYUP:
                if (S.swallow_up) {
                    S.swallow_up = 0;
                    break;
                }
                on_key_up(&e.key);
                break;
            case SDL_MOUSEMOTION:
                /* the Magic Remote pointer: what it points at gets focus */
                if (wake(0)) break;
                {
                    int mx = e.motion.x, my = e.motion.y;
                    to_logical(&mx, &my);
                    if (pointer_moved(mx, my) && !S.moving && !S.ok_down) pointer_at(mx, my, 1);
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (wake(0)) { S.ptr_press = 0; break; }
                if (e.button.button == SDL_BUTTON_LEFT && !S.ok_down) {
                    int mx = e.button.x, my = e.button.y, outside = 0;
                    to_logical(&mx, &my);
                    S.click_x = mx;
                    if (!pointer_moved(mx, my)) { /* the keys are in charge: it's their OK */
                        S.click_x = -1;
                        S.ptr_press = 1;
                        S.ptr_dismiss = 0;
                        S.ok_down = SDL_GetTicks();
                        S.ok_long = 0;
                        break;
                    }
                    S.ptr_press = pointer_at(mx, my, 1);
                    /* a click beside a sheet closes it, as Back would */
                    S.ptr_dismiss = !S.ptr_press && S.sheet.open && !S.sheet.locked && !S.recents.open &&
                                    (sheet_button_at(mx, my, &outside), outside);
                    if (!S.ptr_press && S.cc.open && !S.sheet.open) S.ptr_dismiss = 1; /* and beside the Control Center */
                    if (S.ptr_press) { /* held, it opens options like a held OK */
                        S.ok_down = SDL_GetTicks();
                        S.ok_long = 0;
                    }
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button != SDL_BUTTON_LEFT) break;
                if (S.ptr_press && S.ok_down) ok_release();
                else if (S.ptr_dismiss && S.sheet.open) sheet_dismiss();
                else if (S.ptr_dismiss && S.cc.open) cc_close();
                S.ptr_press = S.ptr_dismiss = 0;
                break;
            case SDL_MOUSEWHEEL:
                if (wake(0)) break;
                if (S.recents.open) sw_key(e.wheel.y < 0 ? SDLK_RIGHT : SDLK_LEFT);
                else if (S.sheet.open) {
                    if (e.wheel.y < 0 && S.sheet.index < S.sheet.n - 1) S.sheet.index++;
                    else if (e.wheel.y > 0 && S.sheet.index > 0) S.sheet.index--;
                } else move_focus(0, e.wheel.y < 0 ? 1 : -1);
                break;
            case SDL_APP_DIDENTERBACKGROUND:
                go_offscreen();
                break;
            case SDL_APP_DIDENTERFOREGROUND:
                come_back();
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || e.window.event == SDL_WINDOWEVENT_SHOWN ||
                    e.window.event == SDL_WINDOWEVENT_RESTORED)
                    come_back();
                else if (e.window.event == SDL_WINDOWEVENT_HIDDEN || e.window.event == SDL_WINDOWEVENT_MINIMIZED)
                    go_offscreen();
                break;
            default:
                if (e.type == net_event) {
                    Result r;
                    while (net_poll(&r)) {
                        handle_result(&r);
                        net_result_free(&r);
                    }
                }
            }
            got = SDL_PollEvent(&e);
        }

        Uint32 now = SDL_GetTicks();
        if (rest_ms && now - start >= rest_ms - 10000) rest_wakes++;
        if (rest_ms && now - start >= rest_ms) {
            printf("at rest: %.1f wakes/s, %.1f frames/s\n", rest_wakes / 10.0, rest_frames / 10.0);
            fflush(stdout);
            running = 0;
        }
        snd_idle();
        if (S.ok_down && !S.ok_long && now - S.ok_down >= LONG_PRESS_MS) {
            S.ok_long = 1;
            if (!S.sheet.open && !S.recents.open && !S.moving && S.zone == 0) {
                snd_play(SND_SELECT);
                open_app_sheet();
            } else if (S.cc.open && !S.sheet.open) {
                snd_play(SND_SELECT);
                cc_long_press();
            }
            dirty = 1;
        }
        if (S.trakt_poll && now >= S.trakt_poll) {
            S.trakt_poll = 0;
            net_get_json("/api/trakt/status", TAG_TRAKT_STATUS, 0, 0);
        }
        if (S.media_poll && now >= S.media_poll) {
            char path[48];
            S.media_poll = 0;
            snprintf(path, sizeof path, "/api/%s/status", SRC_ID[S.media_poll_src]);
            net_get_json(path, TAG_MEDIA_STATUS, 0, 0);
        }
        if (S.setup_retry && now >= S.setup_retry) {
            S.setup_retry = 0;
            net_wake_service();
            setup_check();
        }
        if (S.next_retry && now >= S.next_retry) {
            S.next_retry = 0;
            net_get_json("/api/apps", TAG_APPS, 0, 0);
            rail_request(SRC_STREMIO);
            if (!S.layout_loaded) net_get_json("/api/layout", TAG_LAYOUT, 0, 0);
        }
        if (shot || rest_ms) homeplus_preview_input(now - start);
        if (S.toast_tex && now < S.toast_until + 100) dirty = 1;
        {
            time_t tt = time(NULL);
            struct tm *lt = localtime(&tt);
            if (lt->tm_hour * 60 + lt->tm_min != S.clock_min) dirty = 1;
        }
        /* OLED: idle stages, and a 1 px step of the pixel orbit every minute (within +-4 px) */
        {
            Uint32 idle = now - S.last_input;
            Uint32 dim_ms = S.idle_dim_ms ? S.idle_dim_ms : IDLE_DIM_MS, off_ms = S.idle_off_ms ? S.idle_off_ms : IDLE_OFF_MS;
            int stage = off_ms != IDLE_NEVER && idle >= off_ms ? 2 : dim_ms != IDLE_NEVER && idle >= dim_ms ? 1 : 0;
            if (stage != S.idle_stage && !shot) {
                S.idle_stage = stage;
                dirty = 1;
            }
            int k = (int)(now / 60000);
            if (k != S.orbit_k) {
                S.orbit_k = k;
                int still = shot || S.orbit_off; /* Settings -> Display & OLED -> Pixel Shift */
                r_set_offset(still ? 0 : roundf(4 * sinf(k * 0.61f)), still ? 0 : roundf(4 * sinf(k * 0.37f + 1.0f)));
                dirty = 1;
            }
        }
        /* woken for what a frame does (the Top Shelf turning, its next page): draw one */
        if (!dirty && !animating && next_wake_ms() == 0) dirty = 1;
        /* nothing to draw while hidden behind another app or once the screen is black */
        if ((S.offscreen || (S.idle_stage == 2 && S.black >= 0.999f)) && !shot) dirty = animating = 0;

        if (dirty || animating || shot) {
            float dt = (now - last) / 1000.0f;
            if (dt > 0.1f) dt = 0.1f;
            int take = shot && now >= shot_at;
            animating = render(dt, take ? shot : NULL);
            drew = 1;
            S.drawn_at = now;
            if (rest_ms && now - start >= rest_ms - 10000) rest_frames++;
            if (take) running = 0;
        }
        last = now;
    }
    net_stop();
    r_shutdown();
    SDL_Quit();
    return 0;
}
