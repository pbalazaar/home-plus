/* Home+ native: talking to the Home+ service, off the render thread.
 *
 * The service (SERVICE_ID, shipped in the launcher's package) listens on 127.0.0.1:47823.
 * Jobs are queued from the main thread; worker threads do the HTTP request
 * (and image decoding and shrinking) and post results back, which the main
 * thread drains once per frame. A request fails after 20 s with nothing from the service, or
 * when it takes longer than its deadline (30 s; a picture 40). Pictures are decoded one at a time
 * within a memory allowance, and the decoded ones waiting to be taken are kept within another. */
#ifndef HOMEPLUS_NET_H
#define HOMEPLUS_NET_H

#include <SDL.h>
#include <stddef.h>

#define SERVICE_PORT 47823
#define APP_ID "com.github.pbalazaar.homeplus"
#define SERVICE_ID APP_ID ".service"

/* Blocking request. Returns the HTTP status (0 on connection failure, or an answer of more than
 * max bytes); *body is malloc'd and NUL-terminated (caller frees). post_body NULL means GET. */
int http_request(const char *path, const char *post_body, size_t max, char **body, size_t *len);

typedef enum { JOB_JSON, JOB_IMAGE, JOB_POST, JOB_WAKE, JOB_LUNA } JobKind;

typedef struct {
    JobKind kind;
    int tag;        /* caller-defined: which request this answers */
    int index;      /* caller-defined: e.g. which app */
    int generation; /* lets the caller drop answers to stale requests */
    int status;     /* HTTP status, 0 = unreachable */
    char *body;     /* JOB_JSON / JOB_POST: response text */
    unsigned char *rgba; /* JOB_IMAGE: decoded pixels */
    int w, h;
    /* JOB_IMAGE with sample: edge colour of the icon (see net.c) */
    int sampled, fullbleed;
    unsigned char rgb[3];
    size_t held; /* (net.c: what it counts against the decoded pictures waiting) */
    void *prep;  /* JOB_IMAGE: what its tag's net_prepare made of it, or NULL */
} Result;

/* How soon a picture is wanted: the ones on show (or about to be) first, then the ones next to
 * them, then the rest; in the order they were asked for within each. */
enum { PRIO_LATER, PRIO_SOON, PRIO_NOW };

/* SDL event type pushed whenever a result is ready (valid after net_start). */
extern Uint32 net_event;

void net_start(void);
void net_stop(void);

void net_get_json(const char *path, int tag, int index, int generation);
void net_post(const char *path, const char *body, int tag);
/* fit_w x fit_h: the largest the image is ever drawn (scaled to cover it). Bigger images are
 * shrunk to that when decoded, since GPU memory, not the file, is what they cost. 0 = as is.
 * prio: PRIO_*. The same picture asked for again while it waits is asked for once. */
void net_get_image(const char *path, int tag, int index, int generation, int sample, int fit_w, int fit_h, int prio);
/* Forgets image requests: tag's for index (any index when < 0) and generation, waiting, on their
 * way (those stop downloading, and aren't decoded), or answered but not yet taken. Returns how many
 * were dropped (they never answer). */
int net_drop(int tag, int index, int generation);
/* Moves a picture that's still waiting ahead of the other pictures: it's wanted now. */
void net_bump(int tag, int index, int generation);
/* What a worker does with tag's pictures once they're decoded, before they're answered, so the main
 * thread needn't: prepare(r) may set r->prep, one malloc'd block (net_result_free frees it). It must
 * touch neither the GPU nor the launcher's state. Call before asking for those pictures. */
void net_prepare(int tag, void (*prepare)(Result *r));
/* Starts the service via luna-send (the HTTP server only runs while it's alive). */
void net_wake_service(void);
/* One luna-send call; the result's body is luna-send's output and status is 200
 * when it reported returnValue true. uri and payload must not contain single quotes. */
void net_luna(const char *uri, const char *payload, int tag);

/* Listens for the service's events from now on: each arrives as a JSON result with this tag. */
void net_events(int tag);

/* Pops one finished result; returns 0 when none are waiting. */
int net_poll(Result *out);
void net_result_free(Result *r);

/* What the pictures cost since the last call (which starts them again), for perf.c. */
typedef struct {
    int pictures, cancelled, refused, late; /* decoded; dropped on their way; too big to decode; past their deadline */
    int decoding;                           /* the most decoded at once */
    float decode_ms, picture_wait_ms, action_wait_ms; /* the longest decode; the longest wait in the queue */
    size_t decode_peak, waiting_peak;       /* the most one decode held; the most decoded pictures waiting */
} NetStats;
void net_stats(NetStats *out);

#endif
