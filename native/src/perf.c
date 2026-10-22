/* Home+ native: frame timing, to see how smoothly it runs on the TV. Off unless /tmp/homeplus-perf exists (on the
 * TV the one in Home+'s jail, /var/palm/jail/com.github.pbalazaar.homeplus/tmp) or HOMEPLUS_PERF is set, looked for
 * when it starts, when it comes back and at each rest. On, each run of frames, from the first after a rest to the
 * next rest, is a line in the log (`perf:`): how many frames came and how evenly (the gaps between them, how many
 * missed a refresh), where a frame's time went (update(), drawing, the swap, the CPU time it took, the GPU's own
 * time where its driver says, the loop between frames), the first frame's dt, and how soon a key's work was on
 * screen. With /tmp/homeplus-perf-gpu too (or HOMEPLUS_PERF=gpu) it waits for the GPU after each part of a frame,
 * which slows it down but says what each part costs the GPU (`perf gpu:`). The slowest of the service's
 * answers handled between a run's frames is in its line too, with its tag (home.h's TAG_*), and how many
 * of its frames drew each backdrop (the rest found it unchanged); what pictures cost since the last rest is
 * a line of its own (`perf: pictures:`, net.c's counts), and so is what the GPU holds for it as it hides and
 * when, hidden a while, it lets go of its render targets (`perf: memory`). Each return to the front is a line of its
 * own (`perf: back:`): when its window events came, when come_back() ran, the first frame, and the first frames
 * with the Top Shelf complete and settled (shelf_complete()), with the first event's time on the wall clock,
 * which the service's log and a script on the TV read too. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "home.h"

enum { PERF_MAX = 2400 }; /* 40 s at 60 Hz: a longer run is logged in parts */

static struct {
    int on, probe, gpu, n, ngpu, lost, nkeys, answer_tag;
    double tick;                       /* ms a performance-counter tick */
    float refresh;                     /* ms between the screen's refreshes */
    Uint64 begin, updated, drawn, last, probed; /* this frame's marks; the end of the run's last frame; the last probe */
    double cpu0;                       /* the thread's CPU time when this frame began */
    float dt0;                         /* the run's first dt, ms */
    float answer;                      /* the slowest answer handled between the run's frames, ms */
    unsigned seen;                     /* what was on screen during the run (SEEN) */
    unsigned backdrops[2];             /* backdrops drawn before the run */
    Uint32 key_at;                     /* a key's SDL timestamp, until a frame shows its work */
    float part[PROBE_N];               /* probe: the run's ms in each part, summed */
    int parts[PROBE_N];
    float gap[PERF_MAX], upd[PERF_MAX], draw[PERF_MAX], swap[PERF_MAX], cpu[PERF_MAX], loop[PERF_MAX], gpu_ms[PERF_MAX], key[64];
} P;

static const char *const SEEN[] = { "cc", "sheet", "recents", "scroll", "fade", "toast" };
static const char *const PART[PROBE_N] = { "art", "screen", "scene", "backdrop", "overlays" };

/* a return to the front, from its first window event: ms on the wall clock (0: not yet) */
static struct {
    int on, calls;
    double event, back, first, page, settled;
    char events[160], shown[32]; /* the events, ms after the first; what the first frame's Top Shelf showed */
} B;

static double thread_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static double wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void back_line(void)
{
    char first[32] = "", page[32] = "", settled[32] = "";
    if (B.first) snprintf(first, sizeof first, "+%.0f", B.first - B.event);
    if (B.page) snprintf(page, sizeof page, "+%.0f", B.page - B.event);
    if (B.settled) snprintf(settled, sizeof settled, "+%.0f", B.settled - B.event);
    log_remote("perf: back: come_back +%.0f ms (%d calls), first frame %s (%s), Top Shelf complete %s, settled %s; "
               "events %s; first event at %.0f", B.back ? B.back - B.event : -1, B.calls, first[0] ? first : "none",
               B.shown, page[0] ? page : "never", settled[0] ? settled : "never", B.events, B.event);
    B.on = 0;
}

/* a window or app event: one that brings Home+ back while it's hidden starts a return */
void perf_window(const SDL_Event *e)
{
    if (!P.on) return;
    const char *name = e->type == SDL_APP_DIDENTERFOREGROUND ? "foreground" : e->type == SDL_APP_DIDENTERBACKGROUND ? "background"
                     : e->type == SDL_APP_WILLENTERFOREGROUND ? "willforeground" : e->type == SDL_APP_WILLENTERBACKGROUND ? "willbackground"
                     : e->type != SDL_WINDOWEVENT ? NULL : e->window.event == SDL_WINDOWEVENT_SHOWN ? "shown"
                     : e->window.event == SDL_WINDOWEVENT_HIDDEN ? "hidden" : e->window.event == SDL_WINDOWEVENT_EXPOSED ? "exposed"
                     : e->window.event == SDL_WINDOWEVENT_FOCUS_GAINED ? "focus" : e->window.event == SDL_WINDOWEVENT_FOCUS_LOST ? "unfocus"
                     : e->window.event == SDL_WINDOWEVENT_RESTORED ? "restored" : e->window.event == SDL_WINDOWEVENT_MINIMIZED ? "minimized"
                     : e->window.event == SDL_WINDOWEVENT_MAXIMIZED ? "maximized" : NULL;
    if (!name) return;
    double at = wall_ms() - (double)(SDL_GetTicks() - e->common.timestamp); /* (SDL stamped it with its ticks) */
    int back = e->type == SDL_APP_DIDENTERFOREGROUND || (e->type == SDL_WINDOWEVENT && (e->window.event == SDL_WINDOWEVENT_SHOWN ||
               e->window.event == SDL_WINDOWEVENT_FOCUS_GAINED || e->window.event == SDL_WINDOWEVENT_RESTORED));
    if (B.on && at - B.event > 5000) back_line(); /* (one that never settled) */
    if (!B.on && back && S.offscreen) {
        memset(&B, 0, sizeof B);
        B.on = 1;
        B.event = at;
    }
    if (!B.on) return;
    char one[32];
    snprintf(one, sizeof one, "%s%s+%.0f", B.events[0] ? " " : "", name, at - B.event);
    append(B.events, sizeof B.events, one);
}

void perf_back(void)
{
    if (!B.on) return;
    if (!B.calls++) B.back = wall_ms();
}

void perf_check(void)
{
    const char *env = getenv("HOMEPLUS_PERF");
    int on = env || access("/tmp/homeplus-perf", F_OK) == 0;
    int probe = on && ((env && !strcmp(env, "gpu")) || access("/tmp/homeplus-perf-gpu", F_OK) == 0);
    if (on != P.on || probe != P.probe) P.n = 0;
    if (on && !P.on) { /* (pictures from before it was on aren't this run's) */
        NetStats none;
        net_stats(&none);
    }
    P.on = on, P.probe = probe;
    if (!P.on || P.tick) return;
    P.tick = 1000.0 / (double)SDL_GetPerformanceFrequency();
    SDL_DisplayMode m;
    P.refresh = SDL_GetCurrentDisplayMode(0, &m) == 0 && m.refresh_rate > 0 ? 1000.0f / m.refresh_rate : 1000 / 60.0f;
    P.gpu = r_gpu_timer();
    log_remote("perf: on: %s, %s, a refresh every %.2f ms, GPU times %s", r_gl_info(0), r_gl_info(1), P.refresh,
               P.gpu ? "from the driver" : "unavailable");
}

void perf_key(Uint32 timestamp)
{
    if (P.on && !P.key_at) P.key_at = timestamp ? timestamp : SDL_GetTicks();
}

Uint64 perf_clock(void) { return P.on ? SDL_GetPerformanceCounter() : 0; }

void perf_answer(int tag, Uint64 since)
{
    if (!P.on || !P.n) return;
    float ms = (float)((SDL_GetPerformanceCounter() - since) * P.tick);
    if (ms > P.answer) P.answer = ms, P.answer_tag = tag;
}

void perf_begin(float dt)
{
    if (!P.on) return;
    P.begin = SDL_GetPerformanceCounter();
    P.cpu0 = thread_ms();
    if (!P.n) {
        P.dt0 = dt * 1000;
        P.backdrops[0] = r_backdrops_drawn(0), P.backdrops[1] = r_backdrops_drawn(1);
    }
}

void perf_updated(void)
{
    if (!P.on) return;
    P.updated = P.probed = SDL_GetPerformanceCounter();
    r_gpu_begin();
}

void perf_probe(int part)
{
    if (!P.probe) return;
    r_finish();
    Uint64 now = SDL_GetPerformanceCounter();
    P.part[part] += (float)((now - P.probed) * P.tick);
    P.parts[part]++;
    P.probed = now;
}

void perf_drawn(void)
{
    if (!P.on) return;
    r_gpu_end();
    P.drawn = SDL_GetPerformanceCounter();
}

static void gpu_in(long us)
{
    if (us == -2) P.lost++;
    else if (P.ngpu < PERF_MAX) P.gpu_ms[P.ngpu++] = us / 1000.0f;
}

void perf_end(void)
{
    if (!P.on) return;
    if (P.n == PERF_MAX) perf_rest();
    Uint64 end = SDL_GetPerformanceCounter();
    int i = P.n++;
    P.upd[i] = (float)((P.updated - P.begin) * P.tick);
    P.draw[i] = (float)((P.drawn - P.updated) * P.tick);
    P.swap[i] = (float)((end - P.drawn) * P.tick);
    P.cpu[i] = (float)(thread_ms() - P.cpu0);
    P.gap[i] = i ? (float)((end - P.last) * P.tick) : 0;
    P.loop[i] = i ? (float)((P.begin - P.last) * P.tick) : 0;
    P.last = end;
    P.seen |= (S.cc.t > 0.004f) << 0 | (S.sheet.open || S.ghost.on) << 1 | (S.recents.t > 0.004f) << 2 |
              (S.scrolled > 0.004f && S.scrolled < 0.996f) << 3 | (S.pic_a < 1) << 4 | (S.toast_t > 0.01f) << 5;
    if (P.key_at) {
        if (P.nkeys < (int)(sizeof P.key / sizeof *P.key)) P.key[P.nkeys++] = (float)(SDL_GetTicks() - P.key_at);
        P.key_at = 0;
    }
    for (long us; P.gpu && (us = r_gpu_result(0)) != -1;) gpu_in(us);
    if (B.on) {
        double now = wall_ms();
        int done = shelf_complete();
        if (!B.first) {
            B.first = now;
            snprintf(B.shown, sizeof B.shown, "%s", done == 2 ? "settled" : done ? "complete, fading in" : S.skel_a > 0.004f ? "a skeleton"
                     : S.page_app != S.want_app || S.page_rail != S.want_rail ? "the page before" : "loading");
        }
        if (done && !B.page) B.page = now;
        if (done == 2 && !B.settled) B.settled = now;
        if (B.settled || now - B.event > 5000) back_line();
    }
}

static int by_value(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* average, median, 95th percentile and most of n values (sorts them) */
static void stats(float *v, int n, float out[4])
{
    memset(out, 0, 4 * sizeof *out);
    if (n <= 0) return;
    double sum = 0;
    for (int i = 0; i < n; i++) sum += v[i];
    qsort(v, (size_t)n, sizeof *v, by_value);
    out[0] = (float)(sum / n);
    out[1] = v[n / 2];
    out[2] = v[(n * 95) / 100 < n - 1 ? (n * 95) / 100 : n - 1];
    out[3] = v[n - 1];
}

/* what the launcher holds on the GPU, when it hides and when it lets go of its render targets */
void perf_memory(const char *when)
{
    if (!P.on) return;
    int n;
    size_t tex = r_texture_bytes(&n);
    log_remote("perf: memory (%s): %d textures, %.1f MB; render targets %.1f MB (4 bytes a pixel: the driver adds its own)",
               when, n, tex / 1048576.0, r_target_bytes() / 1048576.0);
}

/* what pictures cost since the last rest (net.c): a line of its own when any came, or were dropped */
static void pictures_line(void)
{
    NetStats n;
    net_stats(&n);
    if (!n.pictures && !n.cancelled && !n.refused && !n.late) return;
    log_remote("perf: pictures: %d decoded (%d at once at most, the slowest %.0f ms, the most one held %.1f MB), %d dropped "
               "on their way, %d too big, %d too slow; decoded and waiting %.1f MB at most; the longest wait in the queue: "
               "a picture %.0f ms, an answer %.0f ms", n.pictures, n.decoding, n.decode_ms, n.decode_peak / 1048576.0,
               n.cancelled, n.refused, n.late, n.waiting_peak / 1048576.0, n.picture_wait_ms, n.action_wait_ms);
}

void perf_rest(void)
{
    if (B.on && wall_ms() - B.event > 5000) back_line();
    if (P.on) pictures_line();
    if (!P.on || !P.n) {
        perf_check();
        return;
    }
    for (long us; P.gpu && (us = r_gpu_result(1)) != -1;) gpu_in(us); /* the run's last frames, from the GPU */
    if (P.n >= 3) {
        /* the gaps between frames, in refreshes: how many came on time, early, a little late or a refresh late */
        static const float edge[] = { 0.75f, 0.9f, 1.1f, 1.25f, 1.5f, 2.5f };
        int late = 0, hist[7] = { 0 };
        float total = 0, g[4], u[4], d[4], s[4], c[4], l[4], q[4], k[4];
        for (int i = 1; i < P.n; i++) {
            int b = 0;
            while (b < 6 && P.gap[i] >= edge[b] * P.refresh) b++;
            hist[b]++;
            total += P.gap[i], late += P.gap[i] > 1.5f * P.refresh;
        }
        stats(P.gap + 1, P.n - 1, g);
        stats(P.upd, P.n, u);
        stats(P.draw, P.n, d);
        stats(P.swap, P.n, s);
        stats(P.cpu, P.n, c);
        stats(P.loop + 1, P.n - 1, l);
        stats(P.gpu_ms, P.ngpu, q);
        stats(P.key, P.nkeys, k);
        char seen[64] = "", gpu[64] = "", keys[64] = "", answer[64] = "";
        for (int i = 0; i < (int)(sizeof SEEN / sizeof *SEEN); i++) {
            if (!(P.seen >> i & 1)) continue;
            if (seen[0]) append(seen, sizeof seen, " ");
            append(seen, sizeof seen, SEEN[i]);
        }
        if (P.ngpu && q[3] > 0) snprintf(gpu, sizeof gpu, ", gpu %.2f/%.2f/%.2f%s", q[0], q[2], q[3], P.lost ? " (some lost)" : "");
        if (P.ngpu >= 3 && q[3] <= 0) { /* (the C3's Mali has the extension, but times every frame at 0) */
            P.gpu = 0;
            log_remote("perf: GPU times from the driver are all 0: none from now on (the probe says what parts cost)");
        }
        if (P.nkeys) snprintf(keys, sizeof keys, "; keys on screen in %.0f/%.0f ms (%d)", k[0], k[3], P.nkeys);
        if (P.answer > 0) snprintf(answer, sizeof answer, "; slowest answer %.1f ms (tag %d)", P.answer, P.answer_tag);
        log_remote("perf%s: %d frames in %.0f ms, %.1f fps, gaps %.1f median %.1f p95 %.1f max, %d late, in refreshes "
                   "<.75:%d <.9:%d ~1:%d <1.25:%d <1.5:%d <2.5:%d more:%d; ms avg/p95/max: update %.2f/%.2f/%.2f, "
                   "draw %.2f/%.2f/%.2f, swap %.2f/%.2f/%.2f, cpu %.2f/%.2f/%.2f%s, between %.2f/%.2f/%.2f; first dt "
                   "%.0f ms%s%s; backdrops drawn %u and %u [%s]",
                   P.probe ? " (gpu probe)" : "", P.n, total, total > 0 ? (P.n - 1) * 1000 / total : 0, g[1], g[2], g[3],
                   late, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], u[0], u[2], u[3], d[0], d[2], d[3],
                   s[0], s[2], s[3], c[0], c[2], c[3], gpu, l[0], l[2], l[3], P.dt0, keys, answer,
                   r_backdrops_drawn(0) - P.backdrops[0], r_backdrops_drawn(1) - P.backdrops[1], seen[0] ? seen : "home");
        if (P.probe) {
            char line[300] = "";
            for (int i = 0; i < PROBE_N; i++) {
                char one[48];
                if (!P.parts[i]) continue;
                snprintf(one, sizeof one, "%s%s %.2f", line[0] ? ", " : "", PART[i], P.part[i] / P.parts[i]);
                append(line, sizeof line, one);
            }
            log_remote("perf gpu: ms a frame: %s [%s]", line, seen[0] ? seen : "home");
        }
    }
    P.n = P.ngpu = P.lost = P.nkeys = 0;
    P.seen = 0;
    P.answer = 0;
    memset(P.part, 0, sizeof P.part);
    memset(P.parts, 0, sizeof P.parts);
    perf_check();
}
