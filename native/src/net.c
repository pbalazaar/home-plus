/* Home+ native: HTTP to the service + background workers. See net.h. */
#include "net.h"
#include "luna.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_MAX_DIMENSIONS 8192 /* MAX_SIDE */
#include "stb_image.h"

#define WORKERS 3 /* the first never takes a picture: actions (a launch, a control) don't wait behind downloads */
#define MAX_PATH_LEN 512

/* What the launcher takes from the service: its answers are a few KB, and a picture is at most 8 MB
 * (the service's cap). A picture is decoded only if it's no more than MAX_SIDE a side and MAX_PIXELS
 * (TMDB's largest are 3840 x 2160): decoded, it's 4 bytes a pixel, on a TV whose memory PicCap and
 * HyperHDR share. Kept, no side is over MAX_TEX: every webOS GPU takes a texture that size, and past
 * its limit glTexImage2D fails without a word (a black picture). */
#define MAX_JSON_BODY (4 << 20)
#define MAX_IMAGE_BODY (12 << 20)
#define MAX_SIDE 8192
#define MAX_PIXELS (16L << 20)
#define MAX_TEX 4096

/* Memory: one picture is decoded at a time, and what that holds (its pixels, then its shrunk copy beside
 * them) is at most DECODE_BUDGET, or it isn't decoded; its file (up to MAX_IMAGE_BODY) and stb's own
 * scratch come on top. Decoded pictures the main thread hasn't taken yet stay within WAITING_BUDGET:
 * past it the next waits before it's decoded (one always goes). */
#define DECODE_BUDGET (64L << 20)
#define WAITING_BUDGET (16L << 20)

/* Time: a request fails when the service sends nothing for IDLE_MS, or takes longer than its deadline in
 * all, so a picture trickling in a byte at a time can't keep a worker for good. A picture's is longer than
 * the service's for its server (30 s, every redirect included), so that's the one that answers first. */
#define IDLE_MS 20000
#define DEADLINE_MS 30000
#define IMAGE_DEADLINE_MS 40000

static int service_port = SERVICE_PORT; /* (tests: HOMEPLUS_PORT, as the service's) */
static Uint32 deadline_ms = DEADLINE_MS, image_deadline_ms = IMAGE_DEADLINE_MS; /* (tests: HOMEPLUS_NET_DEADLINE_MS) */

typedef struct Job {
    JobKind kind;
    int tag, index, generation, sample, fit_w, fit_h, prio;
    char path[MAX_PATH_LEN];
    char *post;
    Uint32 queued_at;
    int fd;             /* its connection while it downloads, else -1 (under lock) */
    SDL_atomic_t cancel; /* dropped on its way (net_drop): it stops, and never answers */
    int late;           /* it went past its deadline */
    struct Job *next;
} Job;

static SDL_mutex *lock;

/* ------------------------------------------------------------------ HTTP */

/* The service answers only the launcher: each request carries the token it
 * writes into our folder when it starts (see service.js). It's read again
 * when the service has restarted and refuses the old one (403). */
static char token_path[600];
static char token[128];
static SDL_mutex *token_lock;

static void load_token(void)
{
    char buf[128] = "";
    FILE *f = token_path[0] ? fopen(token_path, "r") : NULL;
    if (f) {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        buf[strcspn(buf, "\r\n")] = 0;
    }
    SDL_LockMutex(token_lock);
    snprintf(token, sizeof token, "%s", buf);
    SDL_UnlockMutex(token_lock);
}

static void current_token(char *out, size_t cap)
{
    SDL_LockMutex(token_lock);
    snprintf(out, cap, "%s", token);
    SDL_UnlockMutex(token_lock);
}

/* a job's connection, while it has one: net_drop shuts it down to stop the job (fd -1: it's closed) */
static void job_fd(Job *j, int fd)
{
    if (!j) return;
    SDL_LockMutex(lock);
    j->fd = fd;
    SDL_UnlockMutex(lock);
}

static void hang_up(Job *j, int fd)
{
    job_fd(j, -1);
    close(fd);
}

/* j: the job it's for (its deadline, and net_drop can stop it), or NULL */
static int http_once(const char *path, const char *post_body, const char *tok, size_t max, char **body, size_t *len, Job *j)
{
    *body = NULL;
    if (len) *len = 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct timeval tv = { IDLE_MS / 1000, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(service_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    job_fd(j, fd);
    if ((j && SDL_AtomicGet(&j->cancel)) || connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        hang_up(j, fd);
        return 0;
    }
    Uint32 start = SDL_GetTicks(), heard = start, deadline = j && j->kind == JOB_IMAGE ? image_deadline_ms : deadline_ms;

    char head[MAX_PATH_LEN + 384];
    size_t blen = post_body ? strlen(post_body) : 0;
    int hn = snprintf(head, sizeof head,
                      "%s %s HTTP/1.0\r\nHost: 127.0.0.1\r\nX-Homeplus-Token: %s\r\n%s%s%zu\r\n\r\n",
                      post_body ? "POST" : "GET", path, tok,
                      post_body ? "Content-Type: application/json\r\n" : "",
                      "Content-Length: ", blen);
    /* (MSG_NOSIGNAL: sending on a connection net_drop shut, or the service closed, fails the request
     * rather than ending Home+ with a SIGPIPE) */
    if (send(fd, head, hn, MSG_NOSIGNAL) != hn || (blen && send(fd, post_body, blen, MSG_NOSIGNAL) != (ssize_t)blen)) {
        hang_up(j, fd);
        return 0;
    }

    size_t cap = 65536, n = 0;
    char *buf = malloc(cap + 1);
    for (;;) {
        if (buf && n == cap) { /* full: room for more, up to max */
            size_t more = cap * 2 < max ? cap * 2 : max;
            char *grown = more > cap ? realloc(buf, more + 1) : NULL;
            if (!grown) free(buf);
            buf = grown;
            cap = more;
        }
        /* the rest of the answer: by its deadline, and something every IDLE_MS (one poll: nothing wakes it
         * meanwhile, net_drop included, which shuts the connection) */
        Uint32 now = SDL_GetTicks();
        int left = (int)(deadline - (now - start)), quiet = (int)(IDLE_MS - (now - heard));
        if (!buf || left <= 0 || quiet <= 0) { /* more than max, no memory for it, or too slow: the request fails */
            if (buf && left <= 0 && j) j->late = 1;
            free(buf);
            hang_up(j, fd);
            return 0;
        }
        struct pollfd p = { fd, POLLIN, 0 };
        int ready = poll(&p, 1, left < quiet ? left : quiet);
        if (ready < 0 && errno != EINTR) break;
        if (ready <= 0) continue;
        ssize_t k = recv(fd, buf + n, cap - n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        n += k;
        heard = SDL_GetTicks();
    }
    hang_up(j, fd);
    if (j && SDL_AtomicGet(&j->cancel)) { /* (shut down: what came of it is no answer) */
        free(buf);
        return 0;
    }
    buf[n] = 0;

    int status = 0;
    if (sscanf(buf, "HTTP/%*s %d", &status) != 1) status = 0;
    char *sep = strstr(buf, "\r\n\r\n");
    size_t off = sep ? (size_t)(sep + 4 - buf) : n;
    size_t bn = n - off;
    memmove(buf, buf + off, bn); /* the body, in the same buffer */
    buf[bn] = 0;
    *body = buf;
    if (len) *len = bn;
    return status;
}

static int http_job(const char *path, const char *post_body, size_t max, char **body, size_t *len, Job *j)
{
    char tok[128];
    current_token(tok, sizeof tok);
    int status = http_once(path, post_body, tok, max, body, len, j);
    if (status == 403) { /* a new token: the service restarted, or our folder was reinstalled */
        free(*body);
        char *st = NULL;
        http_once("/api/status", NULL, "", MAX_JSON_BODY, &st, NULL, NULL); /* makes the service write it again */
        free(st);
        load_token();
        current_token(tok, sizeof tok);
        status = http_once(path, post_body, tok, max, body, len, j);
    }
    return status;
}

int http_request(const char *path, const char *post_body, size_t max, char **body, size_t *len)
{
    return http_job(path, post_body, max, body, len, NULL);
}

/* ------------------------------------------------------------------ icon colour */

/* If the icon's edges are one solid colour, the tile can extend it so the icon
 * reads as full-bleed. Otherwise use a dimmed average. Same idea as the web UI. */
static void sample_icon(const unsigned char *px, int w, int h, Result *r)
{
    enum { N = 48, INSET = 10 };
    int edge_n = 0;
    double em[3] = { 0 }, all[4] = { 0 };
    unsigned char edge[4 * N][3]; /* per call: two workers run this concurrently */
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            const unsigned char *p = px + ((size_t)(y * h / N) * w + (x * w / N)) * 4;
            if (p[3] < 220) continue;
            all[0] += p[0]; all[1] += p[1]; all[2] += p[2]; all[3]++;
            int ring = (x == INSET || x == N - 1 - INSET || y == INSET || y == N - 1 - INSET) &&
                       x >= INSET && x <= N - 1 - INSET && y >= INSET && y <= N - 1 - INSET;
            if (ring && edge_n < 4 * N) {
                memcpy(edge[edge_n++], p, 3);
                em[0] += p[0]; em[1] += p[1]; em[2] += p[2];
            }
        }
    }
    int ring_size = 4 * (N - 2 * INSET) - 4;
    r->sampled = 1;
    if (edge_n > ring_size * 0.85) {
        for (int c = 0; c < 3; c++) em[c] /= edge_n;
        double dev = 0;
        for (int i = 0; i < edge_n; i++)
            dev += fabs(edge[i][0] - em[0]) + fabs(edge[i][1] - em[1]) + fabs(edge[i][2] - em[2]);
        if (dev / edge_n < 30) {
            r->fullbleed = 1;
            for (int c = 0; c < 3; c++) r->rgb[c] = (unsigned char)(em[c] + 0.5);
            return;
        }
    }
    if (all[3] > 0) {
        for (int c = 0; c < 3; c++) r->rgb[c] = (unsigned char)(all[c] / all[3] * 0.6);
    } else {
        r->sampled = 0;
    }
}

/* ------------------------------------------------------------------ shrinking */

/* Adds w x one source row's colour to an output row: alpha-weighted r, g, b sums and alpha. */
static void add_row(float *acc, const float *row, int n, float w)
{
    for (int i = 0; i < n * 4; i++) acc[i] += w * row[i];
}

static void put_row(unsigned char *out, const float *acc, int n, float area)
{
    for (int i = 0; i < n; i++, acc += 4, out += 4) {
        float a = acc[3];
        for (int c = 0; c < 3; c++) out[c] = a > 0 ? (unsigned char)fminf(acc[c] / a + 0.5f, 255) : 0;
        out[3] = (unsigned char)fminf(a / area + 0.5f, 255);
    }
}

/* Shrinks RGBA pixels to nw x nh (each no larger than the source) by averaging the area of
 * the source each new pixel covers. Colour is weighted by alpha, so the transparent pixels
 * around an icon don't darken its edges. NULL when out of memory. */
static unsigned char *shrink(const unsigned char *px, int w, int h, int nw, int nh)
{
    float sx = (float)w / nw, sy = (float)h / nh;
    float *row = malloc(sizeof(float) * 4 * nw), *acc = calloc((size_t)nw * 4, sizeof(float));
    unsigned char *out = malloc((size_t)nw * nh * 4);
    if (!row || !acc || !out) {
        free(row);
        free(acc);
        free(out);
        return NULL;
    }
    int oy = 0;
    for (int y = 0; y < h && oy < nh; y++) {
        /* this source row averaged across into nw pixels */
        const unsigned char *s = px + (size_t)y * w * 4;
        for (int ox = 0; ox < nw; ox++) {
            float x0 = ox * sx, x1 = x0 + sx, r = 0, g = 0, b = 0, a = 0;
            for (int x = (int)x0; x < w && x < x1; x++) {
                float k = (fminf(x + 1, x1) - fmaxf(x, x0)) * s[x * 4 + 3];
                r += k * s[x * 4];
                g += k * s[x * 4 + 1];
                b += k * s[x * 4 + 2];
                a += k;
            }
            float *d = row + ox * 4;
            d[0] = r; d[1] = g; d[2] = b; d[3] = a;
        }
        /* ...then shared between the output rows it overlaps (at most two when shrinking) */
        float top = (float)y;
        while (oy < nh) {
            float end = (oy + 1) * sy;
            if (y + 1 < end) {
                add_row(acc, row, nw, y + 1 - top);
                break;
            }
            add_row(acc, row, nw, end - top);
            put_row(out + (size_t)oy * nw * 4, acc, nw, sx * sy);
            memset(acc, 0, sizeof(float) * 4 * nw);
            oy++;
            top = end;
            if (top >= y + 1) break;
        }
    }
    if (oy < nh) put_row(out + (size_t)oy * nw * 4, acc, nw, sx * sy); /* float rounding left the last row open */
    free(row);
    free(acc);
    return out;
}

/* ------------------------------------------------------------------ job queue */

typedef struct Done {
    Result r;
    struct Done *next;
} Done;

static SDL_cond *wake, *taken; /* taken: net_poll took a decoded picture (room for the next) */
static SDL_mutex *decoder;     /* one picture decoded at a time */
static Job *jobs_head, *jobs_tail;
static Job *active[WORKERS];   /* what each worker is doing */
static Done *done_head, *done_tail;
static size_t waiting;         /* decoded pictures in the done list, bytes (held, under lock) */
static int decoding;           /* pictures being decoded now (under lock: one, the decoder sees to it) */
static NetStats stats;         /* (under lock) */
static SDL_Thread *threads[WORKERS];
static int running;
Uint32 net_event = (Uint32)-1;

static int same_picture(const Job *a, const Job *b)
{
    return a->kind == JOB_IMAGE && b->kind == JOB_IMAGE && a->tag == b->tag && a->index == b->index && a->generation == b->generation &&
           a->sample == b->sample && a->fit_w == b->fit_w && a->fit_h == b->fit_h && !strcmp(a->path, b->path);
}

/* Queues a job. Pictures wait behind everything else (a slow download shouldn't hold up the service's
 * answers), the ones wanted soonest first, in the order they were asked for. The same picture asked for
 * again while it waits isn't queued twice: it keeps its place, or goes ahead if it's wanted sooner now. */
static void push_job(Job *j)
{
    j->fd = -1;
    j->queued_at = SDL_GetTicks();
    SDL_LockMutex(lock);
    for (Job **p = &jobs_head, *prev = NULL; *p; prev = *p, p = &(*p)->next) {
        if (!same_picture(*p, j)) continue;
        Job *had = *p;
        if (had->prio >= j->prio) { /* (it keeps its place) */
            SDL_UnlockMutex(lock);
            free(j->post);
            free(j);
            return;
        }
        *p = had->next; /* sooner now: queued again, below */
        if (jobs_tail == had) jobs_tail = prev;
        j->queued_at = had->queued_at;
        free(had->post);
        free(had);
        break;
    }
    Job **p = &jobs_head;
    if (j->kind != JOB_IMAGE) while (*p && (*p)->kind != JOB_IMAGE) p = &(*p)->next;
    else while (*p && ((*p)->kind != JOB_IMAGE || (*p)->prio >= j->prio)) p = &(*p)->next;
    j->next = *p;
    *p = j;
    if (!j->next) jobs_tail = j;
    SDL_CondBroadcast(wake); /* (all of them: the actions' worker leaves a picture to the others) */
    SDL_UnlockMutex(lock);
}

/* r goes to the main thread (net_poll), which is woken for it; 0: no memory to (it never comes) */
static int deliver(Result *r)
{
    Done *d = calloc(1, sizeof *d);
    if (!d) return 0;
    d->r = *r;
    SDL_LockMutex(lock);
    if (done_tail) done_tail->next = d; else done_head = d;
    done_tail = d;
    SDL_UnlockMutex(lock);
    /* wake the render loop, which sleeps while nothing is animating */
    SDL_Event e = { 0 };
    e.type = net_event;
    SDL_PushEvent(&e);
    return 1;
}

/* a decoded picture's room among those waiting, given back */
static void give_back(size_t held)
{
    if (!held) return;
    SDL_LockMutex(lock);
    waiting -= held;
    SDL_CondBroadcast(taken);
    SDL_UnlockMutex(lock);
}

/* A worker is done with j: its answer goes to the main thread, unless net_drop dropped it on its way. */
static void finish(int w, Job *j, Result *r)
{
    SDL_LockMutex(lock);
    active[w] = NULL;
    int dropped = SDL_AtomicGet(&j->cancel);
    if (j->kind == JOB_IMAGE) stats.cancelled += dropped, stats.late += j->late;
    SDL_UnlockMutex(lock);
    if (dropped || !deliver(r)) {
        give_back(r->held);
        r->held = 0;
        net_result_free(r);
    }
}

/* the size a picture is kept at: no larger than it's drawn (just covering fit_w x fit_h), nor than a
 * texture can be (MAX_TEX a side) */
static void kept_size(const Job *j, int w, int h, int *kw, int *kh)
{
    float s = j->fit_w > 0 ? fmaxf((float)j->fit_w / w, (float)j->fit_h / h) : 1;
    *kw = w, *kh = h;
    if (s < 1) {
        *kw = (int)lroundf(w * s), *kh = (int)lroundf(h * s);
        if (*kw < j->fit_w) *kw = j->fit_w;
        if (*kh < j->fit_h) *kh = j->fit_h;
    }
    if (*kw > MAX_TEX || *kh > MAX_TEX) {
        float k = fminf((float)MAX_TEX / *kw, (float)MAX_TEX / *kh);
        *kw = (int)lroundf(*kw * k), *kh = (int)lroundf(*kh * k);
        if (*kw < 1) *kw = 1;
        if (*kh < 1) *kh = 1;
    }
}

static void decode_pixels(const Job *j, Result *r, const unsigned char *body, size_t len);

/* A picture into r->rgba, shrunk to what it's drawn at; left NULL (the picture fails: the glow, or
 * none) when it doesn't decode, is too big to (MAX_SIDE, MAX_PIXELS, DECODE_BUDGET), or is dropped
 * while it waits for room among the decoded pictures waiting. */
static void decode_image(Job *j, Result *r, const unsigned char *body, size_t len)
{
    int w = 0, h = 0, n, kw, kh;
    if (!stbi_info_from_memory(body, (int)len, &w, &h, &n)) return; /* (not a picture) */
    if (w > MAX_SIDE || h > MAX_SIDE || (long)w * h > MAX_PIXELS) {
        SDL_LockMutex(lock);
        stats.refused++;
        SDL_UnlockMutex(lock);
        return;
    }
    kept_size(j, w, h, &kw, &kh);
    size_t kept = (size_t)kw * kh * 4, need = (size_t)w * h * 4 + (kw != w || kh != h ? kept : 0);
    SDL_LockMutex(lock);
    if (need > DECODE_BUDGET) {
        stats.refused++;
        SDL_UnlockMutex(lock);
        return;
    }
    while (running && !SDL_AtomicGet(&j->cancel) && waiting && waiting + kept > WAITING_BUDGET) SDL_CondWait(taken, lock);
    if (!running || SDL_AtomicGet(&j->cancel)) {
        SDL_UnlockMutex(lock);
        return;
    }
    waiting += kept; /* (held for it now: given back if it fails) */
    if (waiting > stats.waiting_peak) stats.waiting_peak = waiting;
    SDL_UnlockMutex(lock);

    SDL_LockMutex(decoder);
    SDL_LockMutex(lock);
    if (++decoding > stats.decoding) stats.decoding = decoding;
    SDL_UnlockMutex(lock);
    Uint32 t0 = SDL_GetTicks();
    decode_pixels(j, r, body, len);
    Uint32 took = SDL_GetTicks() - t0;
    SDL_LockMutex(lock);
    decoding--;
    if (r->rgba) {
        r->held = kept;
        stats.pictures++;
        if (need > stats.decode_peak) stats.decode_peak = need;
        if (took > stats.decode_ms) stats.decode_ms = (float)took;
    } else {
        waiting -= kept;
        SDL_CondBroadcast(taken);
    }
    SDL_UnlockMutex(lock);
    SDL_UnlockMutex(decoder);
}

/* (decode_image's work, one picture at a time) */
static void decode_pixels(const Job *j, Result *r, const unsigned char *body, size_t len)
{
    int n;
    r->rgba = stbi_load_from_memory(body, (int)len, &r->w, &r->h, &n, 4);
    if (!r->rgba) return;
    if (j->sample) sample_icon(r->rgba, r->w, r->h, r);
    /* bigger than it's ever drawn: shrink to just cover the fit box */
    float s = j->fit_w > 0 ? fmaxf((float)j->fit_w / r->w, (float)j->fit_h / r->h) : 1;
    if (s < 1) {
        int nw = (int)lroundf(r->w * s), nh = (int)lroundf(r->h * s);
        if (nw < j->fit_w) nw = j->fit_w;
        if (nh < j->fit_h) nh = j->fit_h;
        unsigned char *small = shrink(r->rgba, r->w, r->h, nw, nh);
        if (small) {
            stbi_image_free(r->rgba);
            r->rgba = small; /* malloc'd like stb's own buffers: net_result_free frees either */
            r->w = nw;
            r->h = nh;
        }
    }
    /* still too long for a texture (a 10000 x 200 banner covers the box as it is): shrunk to fit MAX_TEX */
    if (r->w > MAX_TEX || r->h > MAX_TEX) {
        float k = fminf((float)MAX_TEX / r->w, (float)MAX_TEX / r->h);
        int nw = (int)lroundf(r->w * k), nh = (int)lroundf(r->h * k);
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
        unsigned char *small = shrink(r->rgba, r->w, r->h, nw, nh);
        stbi_image_free(r->rgba); /* (no memory to shrink it: no picture, rather than a black one) */
        r->rgba = small;
        r->w = nw;
        r->h = nh;
    }
}

/* j's work, its answer into *res */
static void run_job(Job *j, Result *res)
{
    Result r = { 0 };
    r.kind = j->kind;
    r.tag = j->tag;
    r.index = j->index;
    r.generation = j->generation;

    if (j->kind == JOB_LUNA) {
        /* the bus directly, as our app id; luna-send only where that's unavailable */
        char *reply = NULL;
        int rc = luna_call(j->path, j->post ? j->post : "{}", 20000, &reply);
        if (rc >= 0) {
            r.body = reply ? reply : strdup("");
            r.status = rc == 1 ? 200 : 0;
            *res = r;
            return;
        }
        char cmd[MAX_PATH_LEN + 600];
        snprintf(cmd, sizeof cmd, "luna-send -n 1 -w 20000 '%s' '%s' 2>&1", j->path, j->post ? j->post : "{}");
        FILE *p = popen(cmd, "r");
        size_t cap = 4096, n = 0;
        char *out = malloc(cap + 1);
        if (p) {
            size_t k;
            while (n < cap && (k = fread(out + n, 1, cap - n, p)) > 0) n += k;
            pclose(p);
        }
        out[n] = 0;
        luna_diag("luna-send %s -> %.300s", j->path, out);
        r.body = out;
        r.status = strstr(out, "\"returnValue\":true") || strstr(out, "\"returnValue\": true") ? 200 : 0;
        *res = r;
        return;
    }
    if (j->kind == JOB_WAKE) {
        /* any call starts a webOS service; ping it on the bus, else with luna-send */
        int rc = luna_call("luna://" SERVICE_ID "/ping", "{}", 8000, NULL);
        if (rc < 0) rc = system("luna-send -n 1 -w 5000 luna://" SERVICE_ID "/ping '{}' >/dev/null 2>&1") == 0;
        r.status = rc == 1 ? 200 : 0;
        *res = r;
        return;
    }

    char *body = NULL;
    size_t len = 0;
    int image = j->kind == JOB_IMAGE;
    r.status = http_job(j->path, j->kind == JOB_POST ? (j->post ? j->post : "{}") : NULL, image ? MAX_IMAGE_BODY : MAX_JSON_BODY, &body, &len, j);
    if (image) {
        if (r.status == 200 && body && len && !SDL_AtomicGet(&j->cancel)) decode_image(j, &r, (unsigned char *)body, len);
        free(body);
    } else {
        r.body = body;
    }
    *res = r;
}

static int worker(void *arg)
{
    int w = (int)(intptr_t)arg, actions = w == 0; /* pictures wait behind everything else in the queue: one at its head means only pictures */
    for (;;) {
        SDL_LockMutex(lock);
        while (running && (!jobs_head || (actions && jobs_head->kind == JOB_IMAGE))) SDL_CondWait(wake, lock);
        if (!running) {
            SDL_UnlockMutex(lock);
            return 0;
        }
        Job *j = jobs_head;
        jobs_head = j->next;
        if (!jobs_head) jobs_tail = NULL;
        active[w] = j;
        float waited = (float)(SDL_GetTicks() - j->queued_at);
        float *most = j->kind == JOB_IMAGE ? &stats.picture_wait_ms : &stats.action_wait_ms;
        if (waited > *most) *most = waited;
        SDL_UnlockMutex(lock);
        Result r;
        run_job(j, &r);
        finish(w, j, &r);
        free(j->post);
        free(j);
    }
}

void net_start(void)
{
    net_event = SDL_RegisterEvents(1);
    luna_setup();
    const char *port = getenv("HOMEPLUS_PORT"), *deadline = getenv("HOMEPLUS_NET_DEADLINE_MS");
    if (port && atoi(port) > 0) service_port = atoi(port);
    if (deadline && atoi(deadline) > 0) deadline_ms = image_deadline_ms = (Uint32)atoi(deadline);
    lock = SDL_CreateMutex();
    decoder = SDL_CreateMutex();
    wake = SDL_CreateCond();
    taken = SDL_CreateCond();
    /* the token sits next to the binary on the TV; HOMEPLUS_TOKEN_FILE for the desktop preview */
    const char *env = getenv("HOMEPLUS_TOKEN_FILE");
    char *base = env ? NULL : SDL_GetBasePath();
    snprintf(token_path, sizeof token_path, "%s%s", env ? env : base ? base : "./", env ? "" : "api-token");
    SDL_free(base);
    token_lock = SDL_CreateMutex();
    load_token();
    running = 1;
    for (int i = 0; i < WORKERS; i++) threads[i] = SDL_CreateThread(worker, "homeplus-net", (void *)(intptr_t)i);
}

/* The service's events (the remote's Settings key while Home+ is in front): a request it answers
 * when one happens, or after 15 s, kept open by a thread of its own. Each answer arrives in
 * net_poll as a JSON result with this tag. */
static int events_tag;

static int events_loop(void *arg)
{
    (void)arg;
    while (running) {
        Result r = { 0 };
        r.kind = JOB_JSON;
        r.tag = events_tag;
        r.status = http_request("/api/events", NULL, MAX_JSON_BODY, &r.body, NULL);
        if (r.status == 200 && r.body && strstr(r.body, "\"events\":[\"") && deliver(&r)) continue;
        free(r.body);
        if (r.status != 200) SDL_Delay(2000); /* the service isn't up yet */
    }
    return 0;
}

void net_events(int tag)
{
    if (events_tag) return;
    events_tag = tag;
    SDL_DetachThread(SDL_CreateThread(events_loop, "homeplus-events", NULL)); /* it may be waiting at exit */
}

void net_stop(void)
{
    SDL_LockMutex(lock);
    running = 0;
    SDL_CondBroadcast(wake);
    SDL_CondBroadcast(taken);
    for (int i = 0; i < WORKERS; i++)
        if (active[i] && active[i]->fd >= 0) shutdown(active[i]->fd, SHUT_RDWR); /* (a picture downloading) */
    SDL_UnlockMutex(lock);
    for (int i = 0; i < WORKERS; i++) SDL_WaitThread(threads[i], NULL);
}

static Job *new_job(JobKind kind, const char *path, int tag, int index, int generation)
{
    Job *j = calloc(1, sizeof *j);
    j->kind = kind;
    j->tag = tag;
    j->index = index;
    j->generation = generation;
    if (path) snprintf(j->path, sizeof j->path, "%s", path);
    return j;
}

void net_get_json(const char *path, int tag, int index, int generation)
{
    push_job(new_job(JOB_JSON, path, tag, index, generation));
}

void net_post(const char *path, const char *body, int tag)
{
    Job *j = new_job(JOB_POST, path, tag, 0, 0);
    j->post = body ? strdup(body) : NULL;
    push_job(j);
}

void net_get_image(const char *path, int tag, int index, int generation, int sample, int fit_w, int fit_h, int prio)
{
    Job *j = new_job(JOB_IMAGE, path, tag, index, generation);
    j->sample = sample;
    j->fit_w = fit_w;
    j->fit_h = fit_h;
    j->prio = prio;
    push_job(j);
}

void net_bump(int tag, int index, int generation)
{
    SDL_LockMutex(lock);
    Job **first = &jobs_head, *prev = NULL; /* first: where the pictures start */
    while (*first && (*first)->kind != JOB_IMAGE) first = &(*first)->next;
    for (Job **p = first; *p; prev = *p, p = &(*p)->next) {
        Job *j = *p;
        if (j->kind != JOB_IMAGE || j->tag != tag || j->index != index || j->generation != generation) continue;
        j->prio = PRIO_NOW; /* (what's asked for now from here on goes after it) */
        if (p != first) {
            *p = j->next;
            if (jobs_tail == j) jobs_tail = prev;
            j->next = *first;
            *first = j;
        }
        break;
    }
    SDL_UnlockMutex(lock);
}

int net_drop(int tag, int index, int generation)
{
    int n = 0;
    SDL_LockMutex(lock);
    for (Job **p = &jobs_head, *prev = NULL; *p;) {
        Job *j = *p;
        if (j->kind == JOB_IMAGE && j->tag == tag && (index < 0 || j->index == index) && j->generation == generation) {
            *p = j->next;
            if (jobs_tail == j) jobs_tail = prev;
            free(j->post);
            free(j);
            n++;
        } else {
            prev = j;
            p = &j->next;
        }
    }
    /* ...and on their way: the download stops (its connection shut, so the service sees it go), and what
     * came of it is never decoded nor answered (finish) */
    for (int i = 0; i < WORKERS; i++) {
        Job *j = active[i];
        if (!j || j->kind != JOB_IMAGE || j->tag != tag || (index >= 0 && j->index != index) || j->generation != generation) continue;
        if (SDL_AtomicGet(&j->cancel)) continue;
        SDL_AtomicSet(&j->cancel, 1);
        if (j->fd >= 0) shutdown(j->fd, SHUT_RDWR);
        SDL_CondBroadcast(taken); /* (one waiting for room gives up) */
        n++;
    }
    SDL_UnlockMutex(lock);
    return n;
}

void net_luna(const char *uri, const char *payload, int tag)
{
    Job *j = new_job(JOB_LUNA, uri, tag, 0, 0);
    j->post = payload ? strdup(payload) : NULL;
    push_job(j);
}

void net_wake_service(void)
{
    push_job(new_job(JOB_WAKE, NULL, 0, 0, 0));
}

int net_poll(Result *out)
{
    SDL_LockMutex(lock);
    Done *d = done_head;
    if (d) {
        done_head = d->next;
        if (!done_head) done_tail = NULL;
        if (d->r.held) { /* taken: room for the next picture */
            waiting -= d->r.held;
            SDL_CondBroadcast(taken);
        }
    }
    SDL_UnlockMutex(lock);
    if (!d) return 0;
    *out = d->r;
    out->held = 0;
    free(d);
    return 1;
}

void net_result_free(Result *r)
{
    free(r->body);
    if (r->rgba) stbi_image_free(r->rgba);
    r->body = NULL;
    r->rgba = NULL;
}

void net_stats(NetStats *out)
{
    memset(out, 0, sizeof *out);
    if (!lock) return; /* (not started yet) */
    SDL_LockMutex(lock);
    *out = stats;
    memset(&stats, 0, sizeof stats);
    SDL_UnlockMutex(lock);
}
