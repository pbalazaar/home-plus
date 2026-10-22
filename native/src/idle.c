/* Home+ native: idle: sleeping until something happens, OLED care (dim, then black), and going offscreen. */
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

/* LG's libSDL2 is the Wayland build; the vendored generic headers only show
 * SDL_SysWMinfo's Wayland members when asked */
#define SDL_VIDEO_DRIVER_WAYLAND 1
#include <SDL_syswm.h>

#include "home.h"

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

void start_idle_wait(void)
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
int wait_event(SDL_Event *e, int timeout_ms, int drew)
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

int next_wake_ms(void)
{
    Uint32 now = SDL_GetTicks();
    Uint32 dim_ms = S.idle_dim_ms ? S.idle_dim_ms : IDLE_DIM_MS, off_ms = S.idle_off_ms ? S.idle_off_ms : IDLE_OFF_MS;
    int visible = !S.offscreen && S.idle_stage < 2;
    Uint32 turn = visible ? S.rail_next : 0; /* (0: it isn't turning) */
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

/* Back from an app opened from Home+, the home screen starts at the top (come_back). That's made ready while the
 * app is in front: the Top Shelf's page there is asked for and its pictures loaded, and once they're in it's
 * the page on show (the page left lets go of its pictures), so the first frame back is the home screen as it
 * stays. When Home+ goes, and as the service's answers come while it's hidden. */
void ready_home(void)
{
    go_home();
    if (S.focus < COLS) S.shelf_cur = S.focus;
    S.rail_next = 0; /* (the carousel turns a whole turn after it's back) */
    shelf_load();
    shelf_jump();
    release_images();
}

/* Another app is in front (LG's SDL may say so as HIDDEN, MINIMIZED or
 * DIDENTERBACKGROUND): stop drawing, release the audio and the big pictures. */
void go_offscreen(void)
{
    if (S.offscreen) return;
    S.offscreen = 1;
    S.hidden_at = SDL_GetTicks();
    S.keys_until = 0;
    sw_reset(); /* an app opened from it is in front now */
    S.launching = 0;
    snd_suspend();
    if (S.away) {
        ready_home();
    } else {
        release_images();
        shelf_load(); /* (hidden, it lets go of the pictures beside the page it comes back to, even on their way) */
    }
    perf_memory("hidden");
}

/* Hidden for a while, not an app just opened and closed again: the render targets go too (~6 MB, what glass
 * refracts and its blur), and the first frame back makes them again as it draws its backdrops. */
#define TARGETS_KEPT_MS 30000

void hidden_idle(Uint32 now)
{
    static Uint32 kept; /* (the preview's scene of it: HOMEPLUS_TARGETS_KEPT_MS) */
    if (!kept) kept = getenv("HOMEPLUS_TARGETS_KEPT_MS") ? (Uint32)atoi(getenv("HOMEPLUS_TARGETS_KEPT_MS")) : TARGETS_KEPT_MS;
    if (!S.offscreen || now - S.hidden_at < kept) return;
    if (r_targets_release()) perf_memory("hidden a while, render targets let go");
}

void come_back(void)
{
    if (S.offscreen) { /* the first frame back is the screen at rest (nothing catches up on screen), coming into focus */
        S.settle = 1;
        S.back_at = S.reduce_motion ? 0 : SDL_GetTicks() | 1; /* (Reduce Motion: there at once, as a sheet appears in place) */
    }
    if (S.away) go_home(); /* back from another app (or the Home button): start at the top */
    S.away = 0;
    if (S.asked) open_asked(S.asked); /* ...unless the Control Center over that app asked for something */
    S.asked = 0;
    S.offscreen = 0;
    S.last_input = SDL_GetTicks();
    S.idle_stage = 0;
    perf_check();
    perf_back();
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
