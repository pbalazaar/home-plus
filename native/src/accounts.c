/* Home+ native: signing in to Stremio, Jellyfin, Plex and Trakt, with a code or a QR code. */
#include <stdio.h>

#include "home.h"

/* A sign-in's sheet is done: back to the welcome step it came from, or closed */
static void signin_close(void)
{
    if (S.sheet.parent == PAGE_WELCOME) settings_page(PAGE_WELCOME, S.sheet.parent_index);
    else sheet_dismiss();
}

/* Stremio, Jellyfin and Plex sign-in: the code to show, and whether it was approved */
void on_media_signin(Result *r)
{
    int src = S.media_poll_src;
    cJSON *o = r->status == 200 ? cJSON_Parse(r->body) : NULL;
    const char *err = o ? jstr(o, "error") : "";
    cJSON *pend = o ? cJSON_GetObjectItem(o, "pending") : NULL;
    int code_shown = S.sheet.open && S.sheet.page == PAGE_SIGNIN;
    if (o && cJSON_IsTrue(cJSON_GetObjectItem(o, "connected"))) {
        char msg[64];
        snprintf(msg, sizeof msg, "%s connected", SRC_NAME[src]);
        S.media_poll = 0;
        S.src_connected[src] = 1;
        if (code_shown) signin_close();
        toast(msg, 3000);
        if (want_source() == src) rail_request(src);
    } else if (cJSON_IsObject(pend)) {
        if (r->tag == TAG_MEDIA_LINK) { /* the code, big, with where to enter it */
            char title[64];
            snprintf(title, sizeof title, "Connect %s", SRC_NAME[src]);
            sheet_open(title);
            S.toast_until = SDL_GetTicks(); /* "Asking for a code…": the sheet takes over */
            const char *qr = jstr(pend, "qr");
            if (src == SRC_JELLYFIN) {
                sheet_body("In any Jellyfin app you\xE2\x80\x99re signed in to, open\n"
                           "Settings \xE2\x86\x92 Quick Connect and enter this code:");
            } else {
                char body[200];
                snprintf(body, sizeof body, "Scan this with your phone, or go to %s\nand enter the code below.", jstr(pend, "url"));
                sheet_body(body);
            }
            if (qr[0]) { /* room for it now; the picture follows */
                S.qr_gen++;
                S.sheet_qr_s = 360;
                net_get_image(qr, TAG_QR, 0, S.qr_gen, 0, 0, 0);
            }
            tex_free(&S.sheet_big);
            S.sheet_big = text_render(S.f_bold, 96, jstr(pend, "code"), 0, &S.sheet_big_w, &S.sheet_big_h);
            sheet_add("Cancel", ACT_UP, 0, 0);
            S.sheet.page = PAGE_SIGNIN;
            if (S.at_page == PAGE_ACCOUNTS || S.at_page == PAGE_WELCOME) /* Cancel goes back there; from an app's options it closes */
                S.sheet.parent = S.at_page, S.sheet.parent_index = S.at_index;
        }
        S.media_poll = SDL_GetTicks() + 3000;
    } else {
        S.media_poll = 0;
        if (code_shown) signin_close();
        if (err[0]) toast(err, 7000);
        else if (r->tag == TAG_MEDIA_LINK) toast("Couldn\xE2\x80\x99t start the sign-in. Try again.", 5000);
        else if (code_shown) toast("The code expired. Try again.", 5000);
    }
    cJSON_Delete(o);
}

/* Trakt sign-in, the same way */
void on_trakt_signin(Result *r)
{
    cJSON *o = r->status == 200 ? cJSON_Parse(r->body) : NULL;
    if (!o) {
        if (r->tag == TAG_TRAKT_LINK) toast("Trakt isn\xE2\x80\x99t set up yet. The README\xE2\x80\x99s Trakt section says how.", 7000);
        S.trakt_poll = 0;
        return;
    }
    int connected = cJSON_IsTrue(cJSON_GetObjectItem(o, "connected"));
    cJSON *pend = cJSON_GetObjectItem(o, "pending");
    if (connected) {
        S.trakt_poll = 0;
        S.trakt_connected = 1;
        if (S.sheet.open && S.sheet_big) signin_close();
        toast("Trakt connected", 3000);
        rail_request(SRC_STREMIO);
    } else if (cJSON_IsObject(pend)) {
        if (r->tag == TAG_TRAKT_LINK) {
            /* show the code big, like tvOS sign-in screens */
            char title[160];
            snprintf(title, sizeof title, "On your phone, go to %s and enter", jstr(pend, "url"));
            sheet_open(title);
            tex_free(&S.sheet_big);
            S.sheet_big = text_render(S.f_bold, 96, jstr(pend, "code"), 0, &S.sheet_big_w, &S.sheet_big_h);
            if (S.at_page == PAGE_WELCOME) { /* Cancel and Back go back to the welcome step */
                sheet_add("Cancel", ACT_UP, 0, 0);
                S.sheet.page = PAGE_SIGNIN, S.sheet.parent = PAGE_WELCOME, S.sheet.parent_index = S.at_index;
            } else {
                sheet_add("Cancel", ACT_CANCEL, 0, 0);
            }
        }
        S.trakt_poll = SDL_GetTicks() + 3000;
    } else {
        S.trakt_poll = 0;
        if (r->tag == TAG_TRAKT_LINK) toast("Trakt sign-in didn\xE2\x80\x99t start. Check the Client ID/Secret (see README).", 6000);
    }
    cJSON_Delete(o);
}

int source_installed(int src)
{
    for (int i = 0; i < S.napps; i++)
        if (media_source(&S.apps[i]) == src) return 1;
    return 0;
}

int stremio_installed(void) { return source_installed(SRC_STREMIO); }
