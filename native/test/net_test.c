/* Home+ native: net.c against a stand-in service. An action gets through while pictures hang; pictures
 * dropped on their way hang up; answers and pictures fail at their deadline; pictures are decoded one at
 * a time, within their allowance, and the decoded ones waiting stay within theirs; the ones wanted
 * soonest go first, and one asked for twice is asked for once; and pictures that aren't pictures.
 * `make -C native test` builds and runs it (tools/test.sh native), with test/make-fixtures.py's pictures. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net.h"

#define DEADLINE_MS 2000 /* HOMEPLUS_NET_DEADLINE_MS for these tests (on the TV: 30 s, a picture 40) */
#define W_FIT 1920       /* what the Top Shelf's pictures are asked for at */
#define H_FIT 1080

static const char *fixtures;
static int failures;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            failures++;                                     \
            printf("  FAIL (line %d): ", __LINE__);         \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

/* ------------------------------------------------------------------ the stand-in service */

static SDL_mutex *slock;
static char seen[64][256]; /* the paths asked for, in order */
static int nseen, hung_up; /* hung_up: requests left hanging that the launcher closed */

static void note(const char *path)
{
    SDL_LockMutex(slock);
    if (nseen < 64) snprintf(seen[nseen++], sizeof seen[0], "%s", path);
    SDL_UnlockMutex(slock);
}

static char *slurp(const char *name, size_t *len)
{
    char file[512];
    snprintf(file, sizeof file, "%s/%s", fixtures, name);
    FILE *f = fopen(file, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(n > 0 ? (size_t)n : 1);
    *len = buf ? fread(buf, 1, (size_t)n, f) : 0;
    fclose(f);
    return buf;
}

static void send_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t k = send(fd, p, n, MSG_NOSIGNAL);
        if (k <= 0) return;
        p += k, n -= (size_t)k;
    }
}

/* one request: /hang never answers (until the launcher hangs up), /trickle sends a byte every 100 ms,
 * /img/<file> sends a picture, /slow/<file> the same 200 ms later, and anything else is {"ok":true} */
static int serve(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char req[2048] = "", path[256] = "";
    size_t n = 0;
    while (n < sizeof req - 1 && !strstr(req, "\r\n\r\n")) {
        ssize_t k = recv(fd, req + n, sizeof req - 1 - n, 0);
        if (k <= 0) break;
        req[n += (size_t)k] = 0;
    }
    sscanf(req, "%*s %255s", path);
    note(path);
    if (!strcmp(path, "/hang")) {
        char c;
        while (recv(fd, &c, 1, 0) > 0) {
        }
        SDL_LockMutex(slock);
        hung_up++;
        SDL_UnlockMutex(slock);
    } else if (!strcmp(path, "/trickle")) {
        send_all(fd, "HTTP/1.0 200 OK\r\n\r\n", 19);
        for (int i = 0; i < 300 && send(fd, "x", 1, MSG_NOSIGNAL) == 1; i++) SDL_Delay(100);
    } else if (!strncmp(path, "/img/", 5) || !strncmp(path, "/slow/", 6)) {
        if (path[1] == 's') SDL_Delay(200);
        size_t len = 0;
        char *body = slurp(strchr(path + 1, '/') + 1, &len), head[128];
        if (body) {
            int hn = snprintf(head, sizeof head, "HTTP/1.0 200 OK\r\nContent-Length: %zu\r\n\r\n", len);
            send_all(fd, head, (size_t)hn);
            send_all(fd, body, len);
            free(body);
        } else {
            send_all(fd, "HTTP/1.0 404 Not Found\r\n\r\n", 26);
        }
    } else {
        const char *ok = "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n\r\n{\"ok\":true}";
        send_all(fd, ok, strlen(ok));
    }
    close(fd);
    return 0;
}

static int listener;

static int accept_loop(void *arg)
{
    (void)arg;
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) return 0;
        SDL_DetachThread(SDL_CreateThread(serve, "serve", (void *)(intptr_t)fd));
    }
}

static int start_service(void)
{
    listener = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (bind(listener, (struct sockaddr *)&a, sizeof a) || listen(listener, 64) || getsockname(listener, (struct sockaddr *)&a, &al)) return 0;
    SDL_DetachThread(SDL_CreateThread(accept_loop, "accept", NULL));
    return ntohs(a.sin_port);
}

/* ------------------------------------------------------------------ answers */

static Result got[256];
static int ngot;

static void pump(void)
{
    Result r;
    while (net_poll(&r)) {
        if (ngot < 256) got[ngot++] = r;
        else net_result_free(&r);
    }
}

/* the next answer tagged tag, waiting up to ms for it (others that come meanwhile are kept) */
static int take(int tag, int ms, Result *out)
{
    Uint32 end = SDL_GetTicks() + (Uint32)ms;
    for (;;) {
        pump();
        for (int i = 0; i < ngot; i++)
            if (got[i].tag == tag) {
                *out = got[i];
                got[i] = got[--ngot];
                return 1;
            }
        if (SDL_GetTicks() >= end) return 0;
        SDL_Delay(5);
    }
}

static int seen_at(const char *path, int from)
{
    SDL_LockMutex(slock);
    int at = -1;
    for (int i = from; i < nseen && at < 0; i++)
        if (!strcmp(seen[i], path)) at = i;
    SDL_UnlockMutex(slock);
    return at;
}

/* ------------------------------------------------------------------ tests */

static void actions_get_through(void)
{
    puts("an action gets through while pictures hang; dropped, they hang up");
    for (int i = 0; i < 3; i++) net_get_image("/hang", 10, i, 1, 0, 0, 0, PRIO_NOW);
    SDL_Delay(300); /* (both picture workers are waiting on one) */
    Uint32 t0 = SDL_GetTicks();
    net_post("/api/launch", "{\"id\":\"netflix\"}", 11);
    Result r;
    int answered = take(11, 1000, &r);
    CHECK(answered && r.status == 200, "the launch wasn't answered while pictures hang");
    CHECK(SDL_GetTicks() - t0 < 500, "the launch took %u ms", SDL_GetTicks() - t0);
    if (answered) net_result_free(&r);
    int before = hung_up, n = net_drop(10, -1, 1);
    CHECK(n == 3, "net_drop dropped %d, not 3 (2 on their way, 1 waiting)", n);
    SDL_Delay(400);
    CHECK(hung_up - before == 2, "the service saw %d of the 2 on their way hang up", hung_up - before);
    CHECK(!take(10, 300, &r), "a dropped picture answered");
}

static void deadlines(void)
{
    puts("a picture trickling in, and an answer that never comes, fail at their deadline");
    NetStats s;
    net_stats(&s);
    Uint32 t0 = SDL_GetTicks();
    net_get_image("/trickle", 20, 0, 1, 0, 0, 0, PRIO_NOW);
    net_get_json("/hang", 21, 0, 0);
    Result r;
    int failed = take(20, DEADLINE_MS + 2000, &r);
    Uint32 took = SDL_GetTicks() - t0;
    CHECK(failed && r.status == 0 && !r.rgba, "the trickling picture didn't fail");
    CHECK(took >= DEADLINE_MS - 100 && took < DEADLINE_MS + 1000, "it failed after %u ms (its deadline: %d)", took, DEADLINE_MS);
    if (failed) net_result_free(&r);
    failed = take(21, 1500, &r);
    CHECK(failed && r.status == 0, "the answer that never came didn't fail");
    if (failed) net_result_free(&r);
    net_stats(&s);
    CHECK(s.late == 1, "%d pictures counted too slow, not 1", s.late);
}

static void one_decode_at_a_time(void)
{
    puts("pictures are decoded one at a time (8 MB ones, two of which the waiting allowance lets through)");
    NetStats s;
    net_stats(&s);
    for (int i = 0; i < 6; i++) net_get_image("/img/hd.png", 30, i, 1, 0, 0, 0, PRIO_NOW);
    int n = 0;
    Result r;
    for (int i = 0; i < 6; i++)
        if (take(30, 8000, &r)) {
            n += r.rgba && r.w == 1920 && r.h == 1080;
            net_result_free(&r);
        }
    CHECK(n == 6, "%d of 6 decoded", n);
    net_stats(&s);
    CHECK(s.pictures == 6 && s.decoding == 1, "%d decoded, %d at once at most", s.pictures, s.decoding);
}

static void waiting_one_always_goes(void)
{
    puts("a picture bigger than the waiting allowance still goes, when none are waiting");
    for (int i = 0; i < 2; i++) net_get_image("/img/big.png", 35, i, 1, 0, 0, 0, PRIO_NOW);
    int n = 0;
    Result r;
    for (int i = 0; i < 2; i++)
        if (take(35, 8000, &r)) {
            n += r.rgba && r.w == 2048;
            net_result_free(&r);
        }
    CHECK(n == 2, "%d of 2 decoded", n);
}

static void decode_allowance(void)
{
    puts("a picture past the decode allowance isn't decoded; TMDB's largest is, kept at what it's drawn at");
    NetStats s;
    net_stats(&s);
    net_get_image("/img/huge.png", 40, 0, 1, 0, W_FIT, H_FIT, PRIO_NOW);
    net_get_image("/img/uhd.png", 40, 1, 1, 0, W_FIT, H_FIT, PRIO_NOW);
    int huge = 0, uhd = 0;
    Result r;
    for (int i = 0; i < 2; i++)
        if (take(40, 10000, &r)) {
            if (r.index == 0) huge = r.status == 200 && !r.rgba ? 1 : -1;
            if (r.index == 1) uhd = r.rgba && r.w == W_FIT && r.h == H_FIT ? 1 : -1;
            net_result_free(&r);
        }
    CHECK(huge == 1, "the 4000 x 4000 picture (and its 1920 x 1920 copy) wasn't refused");
    CHECK(uhd == 1, "the 3840 x 2160 picture wasn't decoded to 1920 x 1080");
    net_stats(&s);
    CHECK(s.refused == 1 && s.pictures == 1, "%d refused, %d decoded", s.refused, s.pictures);
}

static void waiting_allowance(void)
{
    puts("decoded pictures waiting to be taken stay within their allowance");
    NetStats s;
    net_stats(&s);
    for (int i = 0; i < 5; i++) net_get_image("/img/hd.png", 50, i, 1, 0, 0, 0, PRIO_NOW);
    SDL_Delay(1500); /* (none taken meanwhile) */
    net_stats(&s);
    CHECK(s.pictures == 2, "%d decoded before any were taken (room for 2 of 8 MB)", s.pictures);
    CHECK(s.waiting_peak <= (16 << 20), "%.1f MB waiting", s.waiting_peak / 1048576.0);
    int n = 0;
    Result r;
    for (int i = 0; i < 5; i++)
        if (take(50, 8000, &r)) {
            n += r.rgba != NULL;
            net_result_free(&r);
        }
    CHECK(n == 5, "%d of 5 came once taken", n);
}

static void order(void)
{
    puts("pictures wanted soonest go first; one asked for twice is asked for once; a bumped one goes ahead");
    for (int i = 0; i < 2; i++) net_get_image("/hang", 60, i, 1, 0, 0, 0, PRIO_NOW);
    SDL_Delay(300);
    int from = nseen;
    net_get_image("/slow/a.png", 61, 0, 1, 0, 0, 0, PRIO_LATER);
    net_get_image("/slow/b.png", 61, 1, 1, 0, 0, 0, PRIO_LATER);
    net_get_image("/slow/c.png", 61, 2, 1, 0, 0, 0, PRIO_NOW);
    net_get_image("/slow/d.png", 61, 3, 1, 0, 0, 0, PRIO_SOON);
    net_get_image("/slow/a.png", 61, 0, 1, 0, 0, 0, PRIO_LATER); /* again */
    net_get_image("/slow/x.png", 61, 4, 1, 0, 0, 0, PRIO_LATER);
    net_bump(61, 4, 1);
    net_drop(60, 0, 1); /* one worker free: they go one at a time, in order */
    int n = 0;
    Result r;
    for (int i = 0; i < 5; i++)
        if (take(61, 3000, &r)) {
            n++;
            net_result_free(&r);
        }
    CHECK(n == 5 && !take(61, 400, &r), "%d answers, not 5", n);
    int x = seen_at("/slow/x.png", from), c = seen_at("/slow/c.png", from), d = seen_at("/slow/d.png", from);
    int a = seen_at("/slow/a.png", from), b = seen_at("/slow/b.png", from);
    CHECK(x >= 0 && x < c && c < d && d < a && a < b, "asked for in the order x %d, c %d, d %d, a %d, b %d", x, c, d, a, b);
    CHECK(seen_at("/slow/a.png", a + 1) < 0, "a was asked for twice");
    net_drop(60, 1, 1);
}

static void drop_answered(void)
{
    puts("a picture answered but not yet taken is dropped too, its room among those waiting given back");
    NetStats s, t;
    net_stats(&t); /* (the counts from before) */
    int decoded = 0;
    net_get_image("/img/hd.png", 90, 0, 1, 0, 0, 0, PRIO_NOW);
    for (int i = 0; i < 300 && !decoded; i++) { /* (decoded: it's answered at once after) */
        SDL_Delay(10);
        net_stats(&t);
        decoded += t.pictures;
    }
    CHECK(decoded == 1, "the picture to drop wasn't decoded");
    SDL_Delay(50);
    int n = net_drop(90, -1, 1);
    CHECK(n == 1, "net_drop dropped %d, not the 1 answered", n);
    Result r;
    CHECK(!take(90, 300, &r), "the dropped picture was still taken");
    for (int i = 0; i < 2; i++) net_get_image("/img/hd.png", 91, i, 1, 0, 0, 0, PRIO_NOW);
    SDL_Delay(1500); /* (none taken meanwhile: two of 8 MB fit in 16, beside nothing) */
    net_stats(&s);
    CHECK(s.pictures == 2, "%d of 2 decoded with none taken: the dropped one's room wasn't given back", s.pictures);
    for (int i = 0; i < 2; i++)
        if (take(91, 3000, &r)) net_result_free(&r);
}

static void not_pictures(void)
{
    puts("what isn't a picture, is half of one or has no pixels gives none");
    net_get_image("/img/garbage.bin", 70, 0, 1, 0, 0, 0, PRIO_NOW);
    net_get_image("/img/truncated.png", 70, 1, 1, 0, 0, 0, PRIO_NOW);
    net_get_image("/img/missing.png", 70, 2, 1, 0, 0, 0, PRIO_NOW);
    net_get_image("/img/empty.gif", 70, 3, 1, 1, 0, 0, PRIO_NOW); /* (as an icon: sampled, had it pixels) */
    int n = 0;
    Result r;
    for (int i = 0; i < 4; i++)
        if (take(70, 3000, &r)) {
            n += !r.rgba;
            CHECK(r.index != 2 || r.status == 404, "a missing picture's status is %d", r.status);
            net_result_free(&r);
        }
    CHECK(n == 4, "%d of 4 gave no picture", n);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: net-test <fixtures dir>\n");
        return 2;
    }
    fixtures = argv[1];
    signal(SIGPIPE, SIG_IGN);
    if (SDL_Init(SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    slock = SDL_CreateMutex();
    int port = start_service();
    if (!port) {
        fprintf(stderr, "no port for the stand-in service\n");
        return 2;
    }
    char p[16], d[16];
    snprintf(p, sizeof p, "%d", port);
    snprintf(d, sizeof d, "%d", DEADLINE_MS);
    setenv("HOMEPLUS_PORT", p, 1);
    setenv("HOMEPLUS_NET_DEADLINE_MS", d, 1);
    setenv("HOMEPLUS_TOKEN_FILE", "/nonexistent/api-token", 1);
    net_start();
    actions_get_through();
    deadlines();
    one_decode_at_a_time();
    waiting_one_always_goes();
    decode_allowance();
    waiting_allowance();
    order();
    drop_answered();
    not_pictures();
    net_stop();
    printf(failures ? "net: %d failed\n" : "net: all passed\n", failures);
    return failures ? 1 : 0;
}
