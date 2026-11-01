/* Home+ native: what a sheet's buttons and Settings' rows do. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "home.h"

void run_action(Action act, int arg, const char *id)
{
    App *a = item_app(S.focus);
    App *chosen = act == ACT_DELETE_ASK || act == ACT_DELETE || act == ACT_UNHIDE ? app_by_id(id) : NULL; /* the button's app, now */
    if (!chosen && (act == ACT_DELETE_ASK || act == ACT_DELETE || act == ACT_UNHIDE)) {
        toast("That app is gone", 3000);
        return;
    }
    switch (act) {
    case ACT_CC_EDIT: /* the controls wobble; OK moves one, holding OK sizes or removes it */
        S.cc.editing = 1;
        S.cc.picked = -1;
        cc_pack();
        break;
    case ACT_CC_ADD:
        cc_add(arg);
        break;
    case ACT_CC_SIZE:
        if (S.cc.index < S.cc.n && (arg == 1 || arg == 2 || arg == 4)) {
            CCItem *c = &S.cc.it[S.cc.index];
            const char size[2] = { (char)arg, 0 };
            if (!cc_fit(S.cc.index, size, cc_placed())) { /* it, or one after it, would leave the grid */
                cc_problem("No room for it at that size");
                break;
            }
            char title[48];
            snprintf(title, sizeof title, "%s", c->title_s);
            c->title_s[0] = 0; /* Large has a bigger name */
            cc_texts(c, title, "");
            cc_save();
        }
        break;
    case ACT_CC_REMOVE:
        if (S.cc.index < S.cc.n) {
            CCItem *c = &S.cc.it[S.cc.index];
            tex_free(&c->title);
            tex_free(&c->value);
            memmove(c, c + 1, (size_t)(S.cc.n - S.cc.index - 1) * sizeof *c);
            S.cc.n--;
            cc_pack();
            if (S.cc.index >= S.cc.n) S.cc.index = S.cc.n ? S.cc.n - 1 : 0;
            cc_save();
        }
        break;
    case ACT_CC_RESET:
        cc_post("{\"action\":\"reset\"}");
        break;
    case ACT_PLAY_START:
        if (a) resume_title(a, id, 1);
        break;
    case ACT_MOVE: S.moving = 1; break;
    case ACT_FRONT: move_item_to(0); save_layout(); break;
    case ACT_DELETE_ASK: { /* tvOS's question */
        char t[ID_LEN + 32];
        snprintf(t, sizeof t, "Delete \xE2\x80\x9C%s\xE2\x80\x9D?", chosen->title);
        sheet_open(t);
        sheet_body("Deleting this app also deletes its data.");
        sheet_add_app("Delete", ACT_DELETE, chosen->id, 1);
        sheet_add("Cancel", ACT_CANCEL, 0, 0);
        S.sheet.index = 1; /* Cancel first, so a stray OK keeps the app */
        break;
    }
    case ACT_DELETE: {
        App *d = chosen;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", d->id);
        char *body = cJSON_PrintUnformatted(o);
        net_post("/api/remove", body, TAG_DELETED);
        free(body);
        cJSON_Delete(o);
        d->deleted = 1; /* its tile goes now; the app list catches up once the TV has deleted it */
        rebuild_items();
        char msg[ID_LEN + 32];
        snprintf(msg, sizeof msg, "Deleting %s\xE2\x80\xA6", d->title);
        toast(msg, 3000);
        break;
    }
    case ACT_HIDE:
        if (a && S.nhidden < MAX_APPS) {
            char msg[200];
            snprintf(msg, sizeof msg, "%s is now in Hidden, at the end", a->title);
            copy_id(S.hidden[S.nhidden++], a->id);
            save_layout();
            rebuild_items();
            toast(msg, 3000);
        }
        break;
    case ACT_UNHIDE:
    case ACT_UNHIDE_ALL:
        for (int i = 0; i < S.nhidden; i++) {
            if (act == ACT_UNHIDE_ALL || !strcmp(S.hidden[i], chosen->id)) {
                memmove(&S.hidden[i], &S.hidden[i + 1], (S.nhidden - i - 1) * ID_LEN);
                S.nhidden--;
                i--;
            }
        }
        rebuild_items();
        save_layout();
        break;
    case ACT_HOME_MODE: {
        const char *next = !strcmp(S.home_mode, "watch") ? "redirect" : "watch";
        char body[48];
        snprintf(body, sizeof body, "{\"mode\":\"%s\"}", next);
        net_post("/api/home", body, TAG_HOME_SET);
        snprintf(S.home_mode, sizeof S.home_mode, "%s", next);
        toast(!strcmp(next, "redirect") ? "Home button: no-flash mode" : "Home button: standard mode", 3000);
        settings_again();
        break;
    }
    case ACT_SETUP_RETRY:
        setup_retry_now();
        break;
    case ACT_HOME_YES:
        set_home_mode(S.noflash_available ? "redirect" : "watch", 1);
        toast("The Home button now opens Home+", 3000);
        if (S.at_page == PAGE_WELCOME) welcome_next();
        break;
    case ACT_HOME_NO:
        set_home_mode("off", 1);
        toast("You can turn this on later in Settings", 3000);
        if (S.at_page == PAGE_WELCOME) welcome_next();
        break;
    case ACT_WELCOME_NEXT:
        welcome_next();
        break;
    case ACT_WELCOME_SKIP:
        welcome_finish();
        break;
    case ACT_SETTINGS_KEY:
        S.settings_key = !S.settings_key;
        net_post("/api/home", S.settings_key ? "{\"settingsKey\":true}" : "{\"settingsKey\":false}", TAG_KEY_SET);
        toast(S.settings_key ? "The Settings button opens the Control Center, over other apps too"
                             : "The Settings button opens LG\xE2\x80\x99s Quick Settings", 3000);
        settings_again();
        break;
    case ACT_INPUT_KEY:
        S.input_key = !S.input_key;
        net_post("/api/home", S.input_key ? "{\"inputKey\":true}" : "{\"inputKey\":false}", TAG_KEY_SET);
        toast(S.input_key ? "The Input button opens Home+\xE2\x80\x99s inputs, over other apps too"
                          : "The Input button opens LG\xE2\x80\x99s Input picker", 3000);
        settings_again();
        break;
    case ACT_HOME_TOGGLE: {
        int on = S.home_mode[0] && strcmp(S.home_mode, "off");
        set_home_mode(on ? "off" : S.noflash_available ? "redirect" : "watch", 1);
        toast(on ? "The Home button opens LG\xE2\x80\x99s home screen" : "The Home button now opens Home+", 3000);
        settings_again();
        break;
    }
    case ACT_REMOVE_ASK:
        sheet_open("Remove Home+?");
        sheet_body("This deletes Home+\xE2\x80\x99s boot script, settings and sign-ins,\nturns off the Home button takeover and uninstalls Home+.");
        sheet_add("Remove Home+", ACT_REMOVE, 0, 1);
        sheet_add("Cancel", ACT_UP, 0, 0);
        S.sheet.index = 1; /* Cancel first, so a stray OK keeps Home+ */
        if (S.at_page) S.sheet.page = PAGE_CONFIRM, S.sheet.parent = S.at_page, S.sheet.parent_index = S.at_index;
        break;
    case ACT_REMOVE:
        setup_set(SETUP_REMOVING);
        net_post("/api/uninstall", "{}", TAG_UNINSTALL);
        break;
    case ACT_MEDIA_LINK: { /* the service gets a code (Jellyfin: after finding the server) */
        char path[48];
        snprintf(path, sizeof path, "/api/%s/link", SRC_ID[arg]);
        char wait[64];
        if (arg == SRC_JELLYFIN) snprintf(wait, sizeof wait, "Looking for your Jellyfin server\xE2\x80\xA6");
        else snprintf(wait, sizeof wait, "Asking %s for a code\xE2\x80\xA6", SRC_NAME[arg]);
        toast(wait, 6000);
        net_post(path, "{}", TAG_MEDIA_LINK);
        S.media_poll_src = arg;
        settings_again();
        break;
    }
    case ACT_MEDIA_UNLINK: {
        char path[48];
        snprintf(path, sizeof path, "/api/%s/disconnect", SRC_ID[arg]);
        net_post(path, "{}", 0);
        S.src_connected[arg] = 0;
        if (arg == SRC_STREMIO) S.rail_stremio = 0;
        if (S.rail_source == arg) S.rail_source = SRC_NONE, rail_clear();
        if (S.at_page == PAGE_CONFIRM) settings_page(PAGE_ACCOUNTS, S.at_parent_index);
        else settings_again();
        break;
    }
    case ACT_TRAKT_LINK:
        toast("Asking Trakt for a code\xE2\x80\xA6", 4000);
        net_post("/api/trakt/link", "{}", TAG_TRAKT_LINK);
        break;
    case ACT_TRAKT_UNLINK:
        net_post("/api/trakt/disconnect", "{}", 0);
        S.trakt_connected = 0;
        S.rail_sig[0] = 0;
        rail_request(SRC_STREMIO);
        toast("Trakt disconnected", 3000);
        if (S.at_page == PAGE_CONFIRM) settings_page(PAGE_ACCOUNTS, S.at_parent_index);
        else settings_again();
        break;
    case ACT_ACCOUNT_ASK: { /* signing out asks first; Cancel and Back go back to Accounts */
        char title[64];
        snprintf(title, sizeof title, "Sign Out of %s?", arg == SRC_COUNT ? "Trakt" : SRC_NAME[arg]);
        sheet_open(title);
        sheet_body("Its Continue Watching leaves the Top Shelf until you sign in again.");
        sheet_add("Sign Out", arg == SRC_COUNT ? ACT_TRAKT_UNLINK : ACT_MEDIA_UNLINK, arg, 1);
        sheet_add("Cancel", ACT_UP, 0, 0);
        S.sheet.index = 1; /* Cancel first, so a stray OK keeps the sign-in */
        S.sheet.page = PAGE_CONFIRM;
        S.sheet.parent = PAGE_ACCOUNTS;
        S.sheet.parent_index = S.at_index;
        break;
    }
    case ACT_UP:
        if (S.at_page == PAGE_KEYS) keys_close();
        if (S.sheet.parent) settings_page(S.sheet.parent, S.sheet.parent_index);
        else sheet_dismiss();
        break;
    case ACT_KEYS: /* TMDB's and Trakt's keys, pasted on a phone: the service opens a page for it, its link a QR code */
        net_post("/api/keys/open", "{}", TAG_KEYS);
        settings_again();
        break;
    case ACT_ACCESSIBILITY:
        settings_page(PAGE_ACCESS, 0);
        break;
    case ACT_PAGE: /* a Settings page (App Tiles on the look that's on) */
        settings_page(arg, arg == PAGE_ICONS ? S.icon_look : 0);
        break;
    case ACT_HIDDEN: /* Back returns to its row */
        open_hidden_sheet();
        S.sheet.parent = S.at_page;
        S.sheet.parent_index = S.at_index;
        break;
    case ACT_CLOCK:
        S.clock24 = !S.clock24;
        S.clock_min = -1; /* redraw the clock now */
        net_post("/api/prefs", S.clock24 ? "{\"clock24\":true}" : "{\"clock24\":false}", 0);
        settings_again();
        break;
    case ACT_DIM:
    case ACT_OFF: { /* each press steps to the next choice (arg -1: Left, to the one before) */
        static const int dim[] = { 1, 2, 5, 10, -1 }, off[] = { 5, 10, 20, 30, 60, -1 };
        int is_dim = act == ACT_DIM, n = is_dim ? 5 : 6;
        const int *opt = is_dim ? dim : off;
        Uint32 cur = is_dim ? S.idle_dim_ms : S.idle_off_ms, dflt = is_dim ? IDLE_DIM_MS : IDLE_OFF_MS;
        int now = cur == IDLE_NEVER ? -1 : (int)((cur ? cur : dflt) / 60000), k = 0;
        while (k < n && opt[k] != now) k++;
        int next = opt[(k + (arg < 0 ? n - 1 : 1)) % n];
        Uint32 ms = next < 0 ? IDLE_NEVER : (Uint32)next * 60000;
        if (is_dim) S.idle_dim_ms = ms;
        else S.idle_off_ms = ms;
        char body[48];
        snprintf(body, sizeof body, "{\"%s\":%d}", is_dim ? "dimMinutes" : "offMinutes", next);
        net_post("/api/prefs", body, 0);
        settings_again();
        break;
    }
    case ACT_ICON_LOOK: { /* the tiles behind the sheet change as it's picked */
        static const char *const pref[] = { "default", "dark", "clear", "tinted" };
        char body[40];
        S.icon_look = arg;
        snprintf(body, sizeof body, "{\"iconLook\":\"%s\"}", pref[arg]);
        net_post("/api/prefs", body, 0);
        settings_again();
        break;
    }
    case ACT_ICON_TINT: { /* OK steps to the next colour; a click picks the one under the pointer */
        int i = S.ptr_press && S.click_x >= 0 ? swatch_at((float)S.click_x) : -1;
        icon_tint_set(i >= 0 ? i : (S.icon_tint + 1) % NTINTS);
        break;
    }
    case ACT_SHELF: /* the carousel turns to its next title every 10 s, or waits for Up and Left/Right */
        S.shelf_still = !S.shelf_still;
        net_post("/api/prefs", S.shelf_still ? "{\"shelfTurns\":false}" : "{\"shelfTurns\":true}", 0);
        settings_again();
        break;
    case ACT_SHELF_DETAILS: {
        static const char *const pref[] = { "off", "compact", "full" };
        int next = (S.shelf_details + (arg < 0 ? 2 : 1)) % 3;
        char body[48];
        shelf_set_details(next);
        snprintf(body, sizeof body, "{\"topShelfDetails\":\"%s\"}", pref[next]);
        net_post("/api/prefs", body, TAG_PREFS);
        settings_again();
        break;
    }
    case ACT_ORBIT:
        S.orbit_off = !S.orbit_off;
        net_post("/api/prefs", S.orbit_off ? "{\"pixelShift\":false}" : "{\"pixelShift\":true}", 0);
        settings_again();
        break;
    case ACT_CALM: /* covers are made from the icons: fetch them again to remake them */
        S.vivid_covers = !S.vivid_covers;
        net_post("/api/prefs", S.vivid_covers ? "{\"calmColours\":false}" : "{\"calmColours\":true}", 0);
        request_icons();
        settings_again();
        break;
    case ACT_REDUCE_MOTION:
    case ACT_REDUCE_TRANSPARENCY: {
        int *flag = act == ACT_REDUCE_MOTION ? &S.reduce_motion : &S.reduce_transparency;
        *flag = !*flag;
        char body[64];
        snprintf(body, sizeof body, "{\"%s\":%s}", act == ACT_REDUCE_MOTION ? "reduceMotion" : "reduceTransparency",
                 *flag ? "true" : "false");
        net_post("/api/prefs", body, 0);
        settings_again();
        break;
    }
    case ACT_CC_BLUR_VIDEO:
        S.cc_blur_video = !S.cc_blur_video;
        net_post("/api/prefs", S.cc_blur_video ? "{\"ccBlurVideo\":true}" : "{\"ccBlurVideo\":false}", 0);
        settings_again();
        break;
    case ACT_SOUNDS:
        S.sounds_off = !S.sounds_off;
        snd_enable(!S.sounds_off);
        net_post("/api/prefs", S.sounds_off ? "{\"sounds\":false}" : "{\"sounds\":true}", 0);
        settings_again();
        break;
    default: break;
    }
}
