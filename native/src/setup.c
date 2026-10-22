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

void set_home_mode(const char *mode, int asked)
{
    char body[96];
    snprintf(body, sizeof body, "{\"mode\":\"%s\"%s%s}", mode, asked ? ",\"asked\":true" : "",
             S.onboarded ? "" : ",\"onboarded\":false"); /* the welcome steps' Home question: they aren't done yet */
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
    cJSON *ob = cJSON_GetObjectItem(o, "onboarded");
    S.onboarded = ob ? cJSON_IsTrue(ob) : S.home_asked;
    cJSON_Delete(o);
    if (elevated) {
        setup_set(SETUP_DONE);
        request_data();
        if (!S.onboarded) {
            S.welcome = WELCOME_HELLO;
            welcome_page(0);
        }
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

/* ------------------------------------------------------------------ welcome */

/* The first time Home+ opens, once it's set up: a few choices, a sheet each, then a tip or two. Each can be
 * changed later in Settings. Only the steps this TV has a use for: the remote's buttons need LG Input Hook. */
static int welcome_wanted(int step)
{
    switch (step) {
    case WELCOME_KEYS: return S.settings_key_available;
    default: return 1;
    }
}

/* the step on show (S.welcome), on row index: also how a choice on it shows its new value (settings_again) */
void welcome_page(int index)
{
    switch (S.welcome) {
    case WELCOME_HELLO:
        sheet_open("Welcome to Home+");
        sheet_body("A few quick choices to set up your TV.\nYou can change any of them later in Settings.");
        sheet_add("Get Started", ACT_WELCOME_NEXT, 0, 0);
        sheet_add("Skip", ACT_WELCOME_SKIP, 0, 0);
        break;
    case WELCOME_HOME:
        sheet_open("Use Home+ as Your Home Screen?");
        sheet_body("The Home button will open Home+ instead of LG\xE2\x80\x99s home screen.\nYou can change this any time in Settings.");
        sheet_add("Use Home+", ACT_HOME_YES, 0, 0);
        sheet_add("Not Now", ACT_HOME_NO, 0, 0);
        break;
    case WELCOME_KEYS:
        sheet_open("Remote Buttons");
        sheet_body("The Settings and Input buttons can open Home+\xE2\x80\x99s\nControl Center and inputs, over other apps too.");
        settings_row("Settings Button", S.settings_key ? "Control Center" : "LG Quick Settings", ACT_SETTINGS_KEY, 0, 0, 0);
        settings_row("Input Button", S.input_key ? "Home+ Inputs" : "LG Input Picker", ACT_INPUT_KEY, 0, 0, 0);
        sheet_add("Continue", ACT_WELCOME_NEXT, 0, 0);
        break;
    case WELCOME_ACCOUNTS: {
        sheet_open("Continue Watching");
        sheet_body("Sign in, and what you\xE2\x80\x99re watching shows in the Top Shelf\n"
                   "when you focus the app in the top row. With a TMDB key,\nstreaming apps show what\xE2\x80\x99s popular on them.");
        int stremio = S.rail_stremio || S.src_connected[SRC_STREMIO];
        if (stremio || stremio_installed())
            settings_row("Stremio", stremio ? "Signed In" : "Sign In", stremio ? ACT_NONE : ACT_MEDIA_LINK, SRC_STREMIO, 0, 0);
        settings_row("Trakt", S.trakt_connected ? "Connected" : S.trakt_configured ? "Connect" : "Add Keys",
                     S.trakt_connected ? ACT_NONE : S.trakt_configured ? ACT_TRAKT_LINK : ACT_KEYS, 0, 0, 0);
        for (int src = SRC_JELLYFIN; src < SRC_COUNT; src++)
            if (S.src_connected[src] || source_installed(src))
                settings_row(SRC_NAME[src], S.src_connected[src] ? "Connected" : "Connect",
                             S.src_connected[src] ? ACT_NONE : ACT_MEDIA_LINK, src, 0, 0);
        settings_row("TMDB", S.tmdb_configured ? "Added" : "Add Key", ACT_KEYS, 0, 0, 0);
        sheet_add("Continue", ACT_WELCOME_NEXT, 0, 0);
        break;
    }
    case WELCOME_TILES:
        sheet_open("App Tiles");
        sheet_body("Pick a look for your apps: the tiles behind change as you go.");
        for (int l = LOOK_DEFAULT; l <= LOOK_TINTED; l++) {
            sheet_add(LOOK_NAME[l], ACT_ICON_LOOK, l, 0);
            S.sheet.it[S.sheet.n - 1].check = S.icon_look == l;
        }
        sheet_add("Continue", ACT_WELCOME_NEXT, 0, 0);
        break;
    default:
        S.welcome = WELCOME_DONE;
        sheet_open("You\xE2\x80\x99re All Set");
        sheet_body("Hold OK on an app for its options: move, hide or delete it.\n"
                   "Press Up, then OK, for the Control Center and Settings.");
        sheet_add("Start Using Home+", ACT_WELCOME_SKIP, 0, 0);
    }
    S.sheet.page = PAGE_WELCOME;
    S.sheet.locked = 1; /* Back doesn't close it: each step has its way on */
    S.sheet.wide = S.welcome == WELCOME_KEYS || S.welcome == WELCOME_ACCOUNTS;
    S.sheet.index = index > 0 && index < S.sheet.n ? index : 0;
}

void welcome_next(void)
{
    do S.welcome++;
    while (S.welcome < WELCOME_DONE && !welcome_wanted(S.welcome));
    welcome_page(0);
}

/* done, or skipped: it isn't shown again */
void welcome_finish(void)
{
    S.onboarded = 1;
    net_post("/api/home", "{\"onboarded\":true}", 0);
    sheet_dismiss();
}
