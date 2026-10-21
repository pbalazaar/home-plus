/* Home+ native: first run: the service has to be running and elevated (root) before Home+ can list apps. */
#include <stdio.h>
#include <string.h>

#include "home.h"

/* Control Center: TV settings + accounts */
/* ------------------------------------------------------------------ first run */

void setup_check(void) { net_get_json("/api/status", TAG_STATUS, 0, 0); }

/* The screen for the current setup step: plain words, no SSH or service names. */
static void setup_screen(void)
{
    const char *title, *body;
    switch (S.setup) {
    case SETUP_NO_HBC:
        title = "Homebrew Channel Needed";
        body = "Home+ runs on rooted LG TVs with the Homebrew Channel installed.\nInstall it, then try again.";
        break;
    case SETUP_DENIED:
        title = "Couldn\xE2\x80\x99t Get Permission";
        body = "The Homebrew Channel didn\xE2\x80\x99t give Home+ the access it needs.\nCheck that your TV is rooted and the Homebrew Channel is up to date.";
        break;
    case SETUP_FAILED:
        title = "Home+ Couldn\xE2\x80\x99t Start";
        body = "Part of Home+ that runs in the background didn\xE2\x80\x99t respond.\nRestarting the TV usually fixes this.";
        break;
    case SETUP_OLD:
        title = "Uninstall the Old Home+";
        body = "This Home+ has a new app ID, and the one it replaces is still installed.\n"
               "Uninstall the old one, com.github.pmiraka.homeplus, then try again.\n"
               "Not with its Remove Home+: that deletes your settings and sign-ins.";
        break;
    case SETUP_REMOVING:
        title = "Removing Home+\xE2\x80\xA6";
        body = "Turning off the Home button takeover and clearing Home+\xE2\x80\x99s settings.";
        break;
    default:
        title = "Setting Up Home+";
        body = "Getting Home+ ready. The first time takes a few seconds.";
    }
    sheet_open(title);
    sheet_body(body);
    S.sheet.locked = 1;
    if (S.setup == SETUP_NO_HBC || S.setup == SETUP_DENIED || S.setup == SETUP_FAILED || S.setup == SETUP_OLD)
        sheet_add("Try Again", ACT_SETUP_RETRY, 0, 0);
}

void setup_set(int state)
{
    int changed = state != S.setup;
    S.setup = state;
    if (state == SETUP_DONE) {
        if (S.sheet.open && S.sheet.locked) sheet_dismiss();
        return;
    }
    /* "Setting up" steps share one screen: only redraw it when the words change */
    int working = state == SETUP_STARTING || state == SETUP_ELEVATING || state == SETUP_RESTARTING;
    if (changed && !(working && S.sheet.open && S.sheet.locked && S.sheet.n == 0)) setup_screen();
}

static void ask_home_screen(void)
{
    sheet_open("Use Home+ as Your Home Screen?");
    sheet_body("The Home button will open Home+ instead of LG\xE2\x80\x99s home screen.\nYou can change this any time in Settings.");
    sheet_add("Use Home+", ACT_HOME_YES, 0, 0);
    sheet_add("Not Now", ACT_HOME_NO, 0, 0);
}

void set_home_mode(const char *mode, int asked)
{
    char body[64];
    snprintf(body, sizeof body, "{\"mode\":\"%s\"%s}", mode, asked ? ",\"asked\":true" : "");
    net_post("/api/home", body, TAG_HOME_SET);
    snprintf(S.home_mode, sizeof S.home_mode, "%s", mode);
}

void on_status(Result *r)
{
    if (S.setup == SETUP_DONE || S.setup == SETUP_REMOVING) return;
    cJSON *o = r->status == 200 ? cJSON_Parse(r->body) : NULL;
    if (!o) {
        /* nothing answered: start the service (luna starts it on demand) and look again */
        if (S.setup_tries++ < 12) {
            net_wake_service();
            S.setup_retry = SDL_GetTicks() + 2000;
            if (S.setup != SETUP_RESTARTING) setup_set(SETUP_STARTING);
        } else {
            setup_set(SETUP_FAILED);
        }
        return;
    }
    if (strcmp(jstr(o, "id"), SERVICE_ID)) { /* another service on our port: the old Home+'s (it says no id) */
        cJSON_Delete(o);
        setup_set(SETUP_OLD);
        return;
    }
    int elevated = cJSON_IsTrue(cJSON_GetObjectItem(o, "elevated"));
    snprintf(S.svc_version, sizeof S.svc_version, "%s", jstr(o, "version"));
    S.svc_elevated = elevated;
    S.home_asked = cJSON_IsTrue(cJSON_GetObjectItem(o, "homeAsked"));
    cJSON_Delete(o);
    if (elevated) {
        setup_set(SETUP_DONE);
        request_data();
        if (!S.home_asked) ask_home_screen();
        return;
    }
    if (S.setup == SETUP_RESTARTING && S.setup_tries++ < 6) {
        S.setup_retry = SDL_GetTicks() + 1500; /* still the old, unelevated process: give it a moment */
        return;
    }
    if (S.elevate_tries++ < 2) {
        setup_set(SETUP_ELEVATING);
        net_luna(HBC_ELEVATE, "{\"id\":\"" SERVICE_ID "\"}", TAG_ELEVATE);
    } else {
        setup_set(SETUP_DENIED);
    }
}

void on_elevate(Result *r)
{
    if (r->status == 200) {
        /* elevated: the running process is still unprivileged, so restart it */
        S.setup_tries = 0;
        setup_set(SETUP_RESTARTING);
        net_post("/api/restart", "{}", TAG_RESTART);
        S.setup_retry = SDL_GetTicks() + 1500;
        return;
    }
    const char *out = r->body ? r->body : "";
    fprintf(stderr, "homeplus: elevateService: %s\n", out);
    if (contains_ci(out, "does not exist") || contains_ci(out, "unknown service") || contains_ci(out, "not found") ||
        contains_ci(out, "luna-send: not found") || !*out)
        setup_set(SETUP_NO_HBC);
    else
        setup_set(SETUP_DENIED);
}

void setup_retry_now(void)
{
    S.setup_tries = S.elevate_tries = S.wake_tries = 0;
    S.setup = SETUP_CHECK;
    setup_screen();
    setup_check();
}
