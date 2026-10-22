/* Home+ native — a tvOS-style home screen for webOS, drawn with SDL2 + GLES2.
 *
 * Rendered on the GPU by our own GLES2 renderer (render.c): Liquid Glass panels,
 * rounded shapes and text, a few dozen quads a frame. Animations run at the
 * display refresh rate, and when nothing moves the loop sleeps.
 *
 * Data comes from the Home+ service's local API (see service/service.js).
 *
 * This file starts it and runs the frame loop: update() advances every animated value, OLED care's
 * stages and the pixel orbit included. The rest is a file per feature; home.h lists them. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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
    /* the Control Center's controls, quietly (1), so their glyphs are made at rest (cc_warm), not as it opens */
    if (!S.cc.n) net_get_json("/api/control", TAG_CONTROL, 1, 0);
}

void open_asked(int what)
{
    if (what == OPEN_SETTINGS) open_settings_sheet();
    else if (what == OPEN_RECENTS) sw_open();
    else if (what == OPEN_ACCESSIBILITY) settings_page(PAGE_ACCESS, 0);
}

/* Advances every animated value once per frame. Drawing only reads them: the
 * scene is drawn twice, into the glass backdrop and then to the screen. */
int update(float dt)
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
        spring(a ? &a->s : &S.special_s, a ? &a->sv : &S.special_v, f ? on : 0.0f, f ? resp : 0.35f, f ? damp : 0.7f, dt, &anim);
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
     * a moving light (from the pointer if it moved lately), then it settles. The
     * carousel's titles count as focus only while you step through them: the Top
     * Shelf turning by itself moves nothing */
    int layer = S.recents.open ? 2 : S.sheet.open ? 1 : 0; /* home, a sheet, the App Switcher */
    int key = layer == 2 ? 2000000 + S.recents.index * 2 + S.recents.close_all
            : layer == 1 ? 1000000 + S.sheet.index
                         : S.zone * 100000 + S.focus * 1000 + S.top_index * 100 + (S.zone == ZONE_RAIL ? S.rail_index : 0);
    if (key != S.focus_key) {
        /* a tick when a key moved focus: not when a layer opens or closes, nor
         * when the pointer or a reset moves it */
        if (S.tick_armed && layer == S.focus_key / 1000000) snd_play(SND_TICK);
        S.focus_key = key;
        /* only focus landing on the home screen sways it: moving through a sheet or Recents swayed the tile
         * under them, and redrew and blurred the scene behind them for 1.6 s a move */
        if (!layer) S.sway_t = 0;
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

/* How far a frame moves animations on: the whole refreshes since the last frame (up to 0.1 s). With vsync
 * each frame is on screen for whole refreshes however late the loop woke to draw it, so measured time
 * made even motion uneven (the C3's frames came 12 to 23 ms apart at a steady 60 Hz). A refresh is the
 * middle of the last 15 gaps between animating frames: the screen's own, which isn't always the one SDL
 * says (after 50 Hz video the C3's screen stays at 50 Hz and SDL still says 60), or the pace frames come at.
 * After a rest it's one: the time the loop slept isn't motion (it was 100 ms, a jump at the start of every
 * animation). */
static float frame_dt(int resting)
{
    enum { N = 15 };
    static Uint64 last;
    static double tick, refresh, gaps[N];
    static int next;
    if (!tick) {
        SDL_DisplayMode m;
        tick = 1.0 / (double)SDL_GetPerformanceFrequency();
        refresh = SDL_GetCurrentDisplayMode(0, &m) == 0 && m.refresh_rate >= 24 ? 1.0 / m.refresh_rate : 1 / 60.0;
        for (int i = 0; i < N; i++) gaps[i] = refresh;
    }
    Uint64 now = SDL_GetPerformanceCounter();
    double gap = (now - last) * tick, n = 1;
    if (!resting && last) {
        double s[N];
        gaps[next] = gap;
        next = (next + 1) % N;
        for (int i = 0; i < N; i++) { /* sorted, for the middle one: an odd long or short gap doesn't move it */
            int j = i;
            for (; j > 0 && s[j - 1] > gaps[i]; j--) s[j] = s[j - 1];
            s[j] = gaps[i];
        }
        refresh = s[N / 2];
        n = floor(gap / refresh + 0.5);
    }
    last = now;
    double dt = refresh * (n < 1 ? 1 : n);
    return (float)(dt > 0.1 ? 0.1 : dt);
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
    perf_check();

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
    /* tests: run as on the TV for this long, then say how often the loop woke and drew in its last 10 s,
     * and how much of that time went on drawing (all of it while it never stops, at any frame rate) */
    Uint32 rest_ms = getenv("HOMEPLUS_REST_REPORT_MS") ? (Uint32)atoi(getenv("HOMEPLUS_REST_REPORT_MS")) : 0;
    unsigned rest_wakes = 0, rest_frames = 0, rest_drawing_ms = 0;
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
                perf_key(e.key.timestamp);
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
                        Uint64 t0 = perf_clock();
                        handle_result(&r);
                        perf_answer(r.tag, t0);
                        net_result_free(&r);
                    }
                }
            }
            got = SDL_PollEvent(&e);
        }

        Uint32 now = SDL_GetTicks();
        if (rest_ms && now - start >= rest_ms - 10000) rest_wakes++;
        if (rest_ms && now - start >= rest_ms) {
            printf("at rest: %.1f wakes/s, %.1f frames/s, drawing %u%% of the time\n", rest_wakes / 10.0, rest_frames / 10.0,
                   rest_drawing_ms / 100);
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
            /* (screenshots are taken at a time: there it's the time that went by) */
            float dt = shot ? fminf((now - last) / 1000.0f, 0.1f) : frame_dt(!animating);
            int take = shot && now >= shot_at;
            animating = render(dt, take ? shot : NULL);
            if (!animating) { /* at rest */
                perf_rest();
                cc_warm();
            }
            drew = 1;
            S.drawn_at = now;
            if (rest_ms && now - start >= rest_ms - 10000) rest_frames++, rest_drawing_ms += SDL_GetTicks() - now;
            if (take) running = 0;
        }
        last = now;
    }
    perf_rest();
    net_stop();
    r_shutdown();
    SDL_Quit();
    return 0;
}
