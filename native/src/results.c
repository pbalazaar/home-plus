/* Home+ native: the service's answers, as they arrive. */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "home.h"

/* ------------------------------------------------------------------ results */

/* A sign-in's QR code goes on a white card, like a printed one. Plex's is white on transparency
 * (for a dark page), so on the card its modules vanished: a code with see-through parts is ink,
 * drawn black. An opaque one is printed already, dark on light. */
static void qr_ink(unsigned char *px, int w, int h)
{
    size_t n = (size_t)w * h, clear = 0;
    for (size_t i = 0; i < n; i++) clear += px[i * 4 + 3] < 128;
    if (clear < n / 4) return;
    for (size_t i = 0; i < n; i++) px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = 0;
}

int handle_result(Result *r)
{
    switch (r->tag) {
    default:  /* (a post that wants no answer: the log) */
    case TAG_WAKE:
        return 0;
    case TAG_LAYOUT:
        if (r->status == 200) parse_layout(r->body);
        break;
    case TAG_STATUS:
        on_status(r);
        break;
    case TAG_ELEVATE:
        on_elevate(r);
        break;
    case TAG_RESTART:
        return 0; /* the service exits; setup_retry wakes it and checks again */
    case TAG_UNINSTALL:
        if (r->status == 200) {
            sheet_open("Home+ Is Being Removed");
            sheet_body("Press Home to go back to LG\xE2\x80\x99s home screen.");
            S.sheet.locked = 1;
        } else {
            S.setup = SETUP_DONE;
            sheet_close();
            toast("Couldn\xE2\x80\x99t remove Home+", 4000);
        }
        break;
    case TAG_APPS:
        if (r->status == 200 && parse_apps(r->body)) {
            S.service_ok = 1;
            S.wake_tries = 0; /* (the next time it fails, it gets its tries again) */
        } else {
            S.service_ok = 0;
            /* service not up yet (or no apps in its answer): start it, then retry */
            if (S.wake_tries++ < 10) {
                net_wake_service();
                S.next_retry = SDL_GetTicks() + 2000;
            }
            if (S.wake_tries > 10 && !S.napps && S.setup == SETUP_DONE) { /* tried: not placeholders for good, but why and Try Again */
                S.toast_until = 0;
                setup_set(SETUP_FAILED);
            } else {
                toast(S.wake_tries < 10 ? "Starting the Home+ service\xE2\x80\xA6" : "The Home+ service isn\xE2\x80\x99t responding. Restart the TV, or reinstall Home+.", 600000);
            }
        }
        if (S.service_ok && S.toast_until > SDL_GetTicks() + 100000) S.toast_until = 0;
        break;
    case TAG_STREMIO:
        if (r->status == 200) parse_continue(r->body, r->index > SRC_NONE && r->index < SRC_COUNT ? r->index : SRC_STREMIO);
        else if (r->index == S.rail_req) S.rail_req = SRC_NONE;
        break;
    case TAG_CONTROL:
        if (r->status == 200) {
            parse_control(r->body);
        } else if (S.cc.ran >= 0 && S.cc.ran < S.cc.n) { /* a control the TV refused shakes, says so, and shows how things are */
            S.cc.it[S.cc.ran].shake = SDL_GetTicks();
            cc_problem("The TV didn\xE2\x80\x99t do that");
            net_get_json("/api/control", TAG_CONTROL, 0, 0);
        } else if (r->status && !r->index) { /* (not when nothing asked for it) */
            toast("The TV didn\xE2\x80\x99t do that", 3000);
        }
        S.cc.ran = -1;
        break;
    case TAG_FEATURED:
        S.feat_req = -1;
        if (r->generation == S.generation && r->index >= 0 && r->index < S.napps) {
            if (r->status == 200) parse_featured(r->body, &S.apps[r->index]);
            else S.apps[r->index].feat = 3; /* not this time */
        }
        break;
    case TAG_SYSTEM:
        if (r->status == 200) parse_system(r->body);
        break;
    case TAG_RUNNING:
        if (r->status == 200) sw_fill(r->body);
        else if (S.recents.loading) S.recents.loading = 0, S.recents.failed = 1;
        break;
    case TAG_DELETED: /* its tile went when you chose Delete: if the TV said no, it comes back */
        if (r->status != 200) {
            cJSON *o = r->body ? cJSON_Parse(r->body) : NULL;
            char msg[300];
            snprintf(msg, sizeof msg, "Couldn\xE2\x80\x99t delete it: %s", o && jstr(o, "errorText")[0] ? jstr(o, "errorText") : "the TV didn\xE2\x80\x99t answer");
            cJSON_Delete(o);
            for (int i = 0; i < S.napps; i++) S.apps[i].deleted = 0;
            rebuild_items();
            toast(msg, 4000);
        } else {
            net_get_json("/api/apps", TAG_APPS, 0, 0); /* the list without it, once webOS has caught up */
        }
        break;
    case TAG_CLOSED:
        if (r->status != 200) toast("Couldn\xE2\x80\x99t close that app", 2500);
        break;
    case TAG_EVENTS:
        /* SAM is bringing Home+ back (the Home button): LG's SDL finishes that as it pumps, every 100 ms while
         * hidden, so it pumps every 10 for a moment */
        if (r->body && strstr(r->body, "\"wake\"") && S.offscreen) S.keys_until = SDL_GetTicks() + 300;
        if (r->body && strstr(r->body, "\"controlCenter\"")) settings_key();
        if (r->body && strstr(r->body, "\"inputs\"")) input_key();
        if (r->body && strstr(r->body, "\"keys\"")) net_get_json("/api/keys", TAG_KEYS_STATUS, 0, 0); /* pasted on a phone */
        if (r->body) { /* asked for from the Control Center over another app, which brings Home+ to the front */
            int want = strstr(r->body, "\"settings\"") ? OPEN_SETTINGS : strstr(r->body, "\"recents\"") ? OPEN_RECENTS
                     : strstr(r->body, "\"accessibility\"") ? OPEN_ACCESSIBILITY : strstr(r->body, "\"general\"") ? OPEN_GENERAL
                     : strstr(r->body, "\"accounts\"") ? OPEN_ACCOUNTS : strstr(r->body, "\"about\"") ? OPEN_ABOUT : 0;
            if (want) open_asked(want); /* (while hidden too: coming back leaves it open) */
        }
        break;
    case TAG_HOME:
    case TAG_HOME_SET:
    case TAG_KEY_SET:
        if (r->status == 200) {
            cJSON *o = cJSON_Parse(r->body);
            if (o) {
                snprintf(S.home_mode, sizeof S.home_mode, "%s", jstr(o, "mode"));
                S.noflash_active = cJSON_IsTrue(cJSON_GetObjectItem(o, "noFlashActive"));
                S.noflash_available = cJSON_IsTrue(cJSON_GetObjectItem(o, "noFlashAvailable"));
                cJSON *sk = cJSON_GetObjectItem(o, "settingsKey");
                S.settings_key = cJSON_IsTrue(cJSON_GetObjectItem(sk, "on"));
                S.settings_key_available = cJSON_IsTrue(cJSON_GetObjectItem(sk, "available"));
                S.input_key = cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(o, "inputKey"), "on"));
                S.piccap = cJSON_IsTrue(cJSON_GetObjectItem(o, "piccap"));
                if (!strcmp(S.home_mode, "redirect") && !S.noflash_active && r->tag == TAG_HOME_SET)
                    toast("No-flash mode isn\xE2\x80\x99t available on this TV; using standard mode", 5000);
                cJSON_Delete(o);
                if (r->tag == TAG_KEY_SET && r->kind == JOB_JSON && S.sheet.open &&
                    (S.sheet.page == PAGE_HOME || S.sheet.page == PAGE_WELCOME)) {
                    int index = S.sheet.index, parent_index = S.sheet.parent_index; /* the buttons as they are */
                    settings_page(S.sheet.page, index);
                    S.sheet.parent_index = parent_index;
                }
            }
        } else if (r->tag == TAG_KEY_SET && r->kind == JOB_POST) {
            /* the Settings or Input button wasn't changed (LG Input Hook's file isn't JSON, say): why, then how they are */
            cJSON *o = r->body ? cJSON_Parse(r->body) : NULL;
            char msg[300];
            snprintf(msg, sizeof msg, "Couldn\xE2\x80\x99t change the button: %s", o && jstr(o, "errorText")[0] ? jstr(o, "errorText") : "the service didn\xE2\x80\x99t answer");
            cJSON_Delete(o);
            toast(msg, 5000);
            net_get_json("/api/home", TAG_KEY_SET, 0, 0);
        }
        break;
    case TAG_PREFS:
        if (r->status == 200) {
            cJSON *o = cJSON_Parse(r->body);
            S.sounds_off = o && cJSON_IsFalse(cJSON_GetObjectItem(o, "sounds"));
            snd_enable(!S.sounds_off);
            S.reduce_motion = o && cJSON_IsTrue(cJSON_GetObjectItem(o, "reduceMotion"));
            S.reduce_transparency = o && cJSON_IsTrue(cJSON_GetObjectItem(o, "reduceTransparency"));
            S.cc_blur_video = o && cJSON_IsTrue(cJSON_GetObjectItem(o, "ccBlurVideo"));
            double dm = jnum(o, "dimMinutes"), om = jnum(o, "offMinutes"); /* -1: never */
            S.idle_dim_ms = dm > 0 ? (Uint32)(dm * 60000) : dm < 0 ? IDLE_NEVER : 0;
            S.idle_off_ms = om > 0 ? (Uint32)(om * 60000) : om < 0 ? IDLE_NEVER : 0;
            int c24 = o && cJSON_IsTrue(cJSON_GetObjectItem(o, "clock24"));
            if (c24 != S.clock24) S.clock24 = c24, S.clock_min = -1;
            S.orbit_off = o && cJSON_IsFalse(cJSON_GetObjectItem(o, "pixelShift"));
            int vivid = o && cJSON_IsFalse(cJSON_GetObjectItem(o, "calmColours"));
            if (vivid != S.vivid_covers) { /* (changed in Settings over another app: covers are made from the icons) */
                S.vivid_covers = vivid;
                request_icons();
            }
            S.shelf_still = o && cJSON_IsFalse(cJSON_GetObjectItem(o, "shelfTurns"));
            const char *details = o ? jstr(o, "topShelfDetails") : "";
            shelf_set_details(!strcmp(details, "off") ? SHELF_DETAILS_OFF :
                              !strcmp(details, "compact") ? SHELF_DETAILS_COMPACT : SHELF_DETAILS_FULL);
            const char *look = o ? jstr(o, "iconLook") : "";
            S.icon_look = !strcmp(look, "dark") ? LOOK_DARK : !strcmp(look, "clear") ? LOOK_CLEAR : !strcmp(look, "tinted") ? LOOK_TINTED : LOOK_DEFAULT;
            const char *tint = o ? jstr(o, "iconTint") : "";
            S.icon_tint = TINT_INDIGO;
            for (int i = 0; i < NTINTS; i++)
                if (!strcasecmp(tint, TINTS[i].name)) S.icon_tint = i;
            cJSON_Delete(o);
            if (r->kind == JOB_POST && S.shelf_details != SHELF_DETAILS_OFF) rail_request(SRC_STREMIO);
        } else if (r->kind == JOB_POST) {
            toast("Couldn\xE2\x80\x99t save Top Shelf Details", 3500);
            net_get_json("/api/prefs", TAG_PREFS, 0, 0);
        }
        break;
    case TAG_MEDIA_LINK:
    case TAG_MEDIA_STATUS: /* Stremio, Jellyfin and Plex sign-in */
        on_media_signin(r);
        break;
    case TAG_TRAKT_LINK:
    case TAG_TRAKT_STATUS:
        on_trakt_signin(r);
        break;
    case TAG_KEYS:
        on_keys(r);
        break;
    case TAG_KEYS_STATUS:
        on_keys_status(r);
        break;
    case TAG_ICON:
        if (r->generation != S.generation || r->index >= S.napps) return 0; /* (an old list's) */
        {
            App *a = &S.apps[r->index];
            if (r->prep) icon_apply(a, r->prep); /* (worked out as it arrived: prepare_icon) */
            else if (r->rgba) make_logo(a, r->rgba, r->w, r->h); /* already shrunk to ICON_FIT while decoding */
            a->icon_state = r->rgba ? 2 : 3;
        }
        return app_shown(r->index); /* one off screen redraws nothing */
    case TAG_ART:
        if (r->generation == S.generation && r->index < S.napps) {
            App *a = &S.apps[r->index];
            a->art_tex = r->rgba ? r_tex(r->rgba, r->w, r->h) : NULL;
            a->art_state = r->rgba ? 2 : 3; /* on show already: it fades in over the glow */
        }
        break;
    case TAG_QR:
        if (r->generation == S.qr_gen && S.sheet.open && S.sheet_qr_s && r->rgba) {
            qr_ink(r->rgba, r->w, r->h);
            tex_free(&S.sheet_qr);
            S.sheet_qr = r_tex(r->rgba, r->w, r->h);
        }
        break;
    case TAG_RAIL_LOGO:
    case TAG_RAIL_BG:
        if (r->generation == S.rail_generation && r->index < S.nrail) {
            RailItem *it = &S.rail[r->index];
            Tex *t = r->rgba ? r_tex(r->rgba, r->w, r->h) : NULL;
            if (r->tag == TAG_RAIL_LOGO) {
                tex_free(&it->logo_tex);
                it->logo_tex = t, it->logo_state = t ? 2 : 3, it->lw = r->w, it->lh = r->h;
            } else {
                tex_free(&it->bg_tex);
                it->bg_tex = t, it->bg_state = t ? 2 : 3;
            }
        }
        break;
    case TAG_RESUME: /* the launch's answer */
        if (r->status != 200) { /* the title's not known (it has left the list since the card came): OK as on any card */
            launch(S.resume_app, S.resume_hint);
            break;
        }
        /* fall through */
    case TAG_LAUNCH:
        if (r->status != 200 || (r->body && strstr(r->body, "\"returnValue\":false"))) {
            S.away = 0;
            S.launching = 0;
            if (S.recents.zooming) sw_reset();
            toast("Couldn\xE2\x80\x99t open that app", 3500);
        }
        break;
    }
    return 1;
}
