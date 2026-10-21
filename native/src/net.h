/* Home+ native: talking to the Home+ service, off the render thread.
 *
 * The service (SERVICE_ID, shipped in the launcher's package) listens on 127.0.0.1:47823.
 * Jobs are queued from the main thread; worker threads do the HTTP request
 * (and image decoding and shrinking) and post results back, which the main
 * thread drains once per frame. */
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
} Result;

/* SDL event type pushed whenever a result is ready (valid after net_start). */
extern Uint32 net_event;

void net_start(void);
void net_stop(void);

void net_get_json(const char *path, int tag, int index, int generation);
void net_post(const char *path, const char *body, int tag);
/* fit_w x fit_h: the largest the image is ever drawn (scaled to cover it). Bigger images are
 * shrunk to that when decoded, since GPU memory, not the file, is what they cost. 0 = as is. */
void net_get_image(const char *path, int tag, int index, int generation, int sample, int fit_w, int fit_h);
/* Forgets image requests that haven't started: tag's for index (any index when < 0) and
 * generation. Returns how many were dropped (they never answer). */
int net_drop(int tag, int index, int generation);
/* Moves a picture that's still waiting ahead of the other pictures: it's wanted now. */
void net_bump(int tag, int index, int generation);
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

#endif
