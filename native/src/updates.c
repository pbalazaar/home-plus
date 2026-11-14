/* Home+ updates: cached service state, foreground notices and explicit confirmation.
 * The service owns releases, suppression and installation; this file never downloads a package. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "home.h"

typedef struct {
    char version[48], current[48], phase[24], source[32], token[256];
    char title[160], summary[400], notes[600], warning[600], message[400];
    int prompt, preserve, upstream, automatic;
} UpdateState;
static UpdateState U;
static char shown_version[48], shown_token[256], kept[48];
static int pending, getting, generation, valid, update_view_dirty, about_view_dirty;
static int busy(void) { return !strcmp(U.phase, "checking") || !strcmp(U.phase, "installing") || !strcmp(U.phase, "recovering"); }

static void text(char *out, size_t size, cJSON *o, const char *key)
{
    const char *s = jstr(o, key);
    /* Refuse a truncated version/confirmation rather than submitting a different token. */
    if (strlen(s) >= size) out[0] = 0;
    else {
        snprintf(out, size, "%s", s);
        /* Release notes may contain newlines; keep each field on its own sheet line
         * so a long note cannot push the replacement warning off the sheet. */
        for (char *p = out; *p; p++) if ((unsigned char)*p < 32) *p = ' ';
    }
}

static void page(void)
{
    sheet_open(!strcmp(U.phase, "checking") ? "Checking for Home+ Updates" :
               !strcmp(U.phase, "installing") ? "Updating Home+" :
               !strcmp(U.phase, "recovering") ? "Recovering Home+" :
               !strcmp(U.phase, "error") ? "Home+ Update Status" : U.title[0] ? U.title : "Home+ Updates");
    S.sheet.page = PAGE_UPDATES;
    char body[2400];
    snprintf(body, sizeof body, "Installed %s  >  Available %s\n%s\n%s\n%s\n%s",
             U.current[0] ? U.current : S.svc_version, U.version[0] ? U.version : "Not checked",
             U.summary, !strcmp(U.phase, "ready") && U.notes[0] ? U.notes : U.message[0] ? U.message : U.notes,
             U.preserve ? "Custom changes are preserved in this build." :
             strstr(U.source, "Unknown") ? "Replacing this build discards unverified custom changes." : U.warning[0] ? "Review custom changes before replacing this build." : "",
             !strcmp(U.phase, "installing") || !strcmp(U.phase, "recovering") ? "Installation continues independently. Home+ reopens when ready." :
             U.upstream ? "Install upstream drops custom code." : "");
    sheet_body(body);
    snprintf(shown_version, sizeof shown_version, "%s", U.version);
    snprintf(shown_token, sizeof shown_token, "%s", U.token);
    sheet_add(busy() || pending ? "Close" : "Keep current build", ACT_UPDATE_KEEP, 0, 0);
    if (!pending && !busy()) {
        if (valid && shown_version[0] && shown_token[0]) {
            if (U.preserve) sheet_add("Update now", ACT_UPDATE_INSTALL, 0, 0);
            if (U.upstream) sheet_add("Install upstream", ACT_UPDATE_INSTALL, 1, 1);
        }
        sheet_add("Check again", ACT_UPDATE_CHECK, 0, 0);
    }
    /* Never transfer a held OK onto a newly available install choice. */
    S.ok_long = 1;
}

/* Responses can arrive while another app is in front. Reconcile only an existing
 * view; automatic notices still go through updates_tick's foreground guards. */
static void refresh_view(void)
{
    if (S.offscreen || S.away) return;
    if (S.sheet.open) {
        if (S.sheet.page == PAGE_UPDATES && update_view_dirty) page();
        else if (S.sheet.split && S.sheet.cat_page[S.sheet.cat] == PAGE_ABOUT && about_view_dirty)
            settings_again();
    }
    update_view_dirty = about_view_dirty = 0;
}

static void post(cJSON *o)
{
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!json) return;
    pending = 1;
    generation++; /* a GET started before this action cannot restore stale choices */
    getting = 0;
    net_post("/api/updates", json, TAG_UPDATE_ACTION);
    free(json);
    S.updates_poll = 0; /* the response wakes the loop and rearms polling */
}

void updates_keep(void)
{
    if (!pending && !busy() && shown_version[0]) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "action", "keep");
        cJSON_AddStringToObject(o, "version", shown_version);
        snprintf(kept, sizeof kept, "%s", shown_version);
        post(o);
    }
    sheet_dismiss();
}

void updates_open(int check)
{
    page();
    if (check && !pending && !busy()) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "action", "check");
        post(o);
        snprintf(U.phase, sizeof U.phase, "checking");
        snprintf(U.message, sizeof U.message, "Checking for a newer release...");
        U.preserve = U.upstream = 0;
        page();
    }
}

void updates_action(Action act, int arg)
{
    if (act == ACT_UPDATE_KEEP) { updates_keep(); return; }
    if (pending) return;
    if (act == ACT_UPDATE_CHECK) { updates_open(1); return; }
    cJSON *o = cJSON_CreateObject();
    if (act == ACT_UPDATE_AUTO) {
        cJSON_AddStringToObject(o, "action", "automatic");
        cJSON_AddBoolToObject(o, "enabled", !S.updates_automatic);
    } else if (act == ACT_UPDATE_INSTALL && !busy() && valid && !S.offscreen && !S.away &&
               !strcmp(shown_version, U.version) && !strcmp(shown_token, U.token) &&
               shown_version[0] && shown_token[0] && (arg ? U.upstream : U.preserve)) {
        cJSON_AddStringToObject(o, "action", "install");
        cJSON_AddStringToObject(o, "version", shown_version);
        cJSON_AddStringToObject(o, "confirmation", shown_token);
        cJSON_AddStringToObject(o, "target", arg ? "upstream" : "preserve");
    } else { cJSON_Delete(o); return; }
    post(o);
    if (act == ACT_UPDATE_INSTALL) {
        snprintf(U.phase, sizeof U.phase, "installing");
        snprintf(U.message, sizeof U.message, "Verifying the confirmed release...");
    }
    if (S.sheet.page == PAGE_UPDATES) page();
}

void updates_result(Result *r)
{
    if (r->tag == TAG_UPDATES && r->generation != generation) return;
    int was_pending = pending;
    if (r->tag == TAG_UPDATES) getting = 0;
    else pending = 0;
    UpdateState old = U;
    cJSON *o = r->body ? cJSON_Parse(r->body) : NULL;
    valid = r->status == 200 && o && cJSON_IsTrue(cJSON_GetObjectItem(o, "returnValue"));
    if (valid) {
        memset(&U, 0, sizeof U);
#define FIELD(member, key) text(U.member, sizeof U.member, o, key)
        FIELD(version, "upstreamVersion"); FIELD(current, "currentVersion"); FIELD(phase, "phase");
        FIELD(source, "currentSource"); FIELD(token, "confirmation"); FIELD(title, "title");
        FIELD(summary, "summary"); FIELD(notes, "notes"); FIELD(warning, "warning"); FIELD(message, "message");
        if (jstr(o, "progress")[0] && (!strcmp(U.phase, "installing") || !strcmp(U.phase, "recovering") || !U.message[0]))
            text(U.message, sizeof U.message, o, "progress");
#undef FIELD
        U.prompt = cJSON_IsTrue(cJSON_GetObjectItem(o, "prompt"));
        U.preserve = cJSON_IsTrue(cJSON_GetObjectItem(o, "canInstallPreserved"));
        U.upstream = cJSON_IsTrue(cJSON_GetObjectItem(o, "canInstallUpstream"));
        U.automatic = cJSON_IsTrue(cJSON_GetObjectItem(o, "automatic"));
        S.updates_automatic = U.automatic;
    } else {
        U.preserve = U.upstream = U.prompt = 0;
        snprintf(U.phase, sizeof U.phase, "error");
        snprintf(U.message, sizeof U.message, "%s", o && jstr(o, "errorText")[0] ? jstr(o, "errorText") : "The update service did not answer. Check again to retry.");
        if (r->tag == TAG_UPDATE_ACTION) { kept[0] = 0; toast(U.message, 5000); }
    }
    cJSON_Delete(o);
    S.updates_poll = SDL_GetTicks() + (busy() ? 1000 : 60000);
    if (was_pending || memcmp(&old, &U, sizeof U)) update_view_dirty = 1;
    if (old.automatic != U.automatic) about_view_dirty = 1;
    refresh_view();
}

void updates_tick(void)
{
    if (S.offscreen || S.away || S.setup != SETUP_DONE || !S.onboarded) return;
    refresh_view();
    Uint32 now = SDL_GetTicks();
    if (!pending && !getting && (!S.updates_poll || (Sint32)(now - S.updates_poll) >= 0)) {
        getting = 1;
        S.updates_poll = 0; /* no deadline while the GET is in flight */
        net_get_json("/api/updates", TAG_UPDATES, 0, generation);
    }
    if (valid && U.prompt && (!strcmp(U.phase, "ready") || !strcmp(U.phase, "conflict") || !strcmp(U.phase, "failed")) && strcmp(kept, U.version) && !pending && !S.sheet.open && !S.cc.open &&
        !S.recents.open && !S.moving && !S.launching && !S.ok_down && !S.idle_stage) page();
}
