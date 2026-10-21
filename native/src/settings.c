/* Home+ native: Settings, like tvOS's: each row shows its current value, and âº opens a page. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "home.h"

static const char *const LOOK_NAME[] = { "Default", "Dark", "Clear", "Tinted" };

/* ---- Settings, like tvOS's: each row shows its current value, › opens a page,
 * and Back goes up one. */

static void settings_row(const char *label, const char *value, Action act, int arg, int chevron, int destructive)
{
    if (S.sheet.n >= MAX_SHEET) return;
    sheet_add(label, act, arg, destructive);
    SheetItem *it = &S.sheet.it[S.sheet.n - 1];
    it->chevron = chevron;
    if (value && *value) it->val = text_render(S.f_medium, SHEET_BUTTON_PX, value, 380, &it->vw, &it->vh);
}

static const char *minutes(Uint32 ms, Uint32 dflt, char *buf, size_t cap)
{
    if (ms == IDLE_NEVER) return "Never";
    snprintf(buf, cap, "%u min", (unsigned)((ms ? ms : dflt) / 60000));
    return buf;
}

/* the row a page has on the main Settings page */
static int main_index(int page)
{
    int cc = S.settings_key_available && S.piccap; /* Control Center: Blur Over Video */
    switch (page) {
    case PAGE_HOME: return 0;
    case PAGE_DISPLAY: return 1;
    case PAGE_CC: return 2;
    case PAGE_ACCESS: return 4 + cc;
    case PAGE_ACCOUNTS: return 5 + cc;
    case PAGE_TV: return 6 + cc + (S.nhidden > 0);
    case PAGE_ABOUT: return 6 + cc + (S.nhidden > 0) + (S.nsettings > 0);
    default: return 0;
    }
}

void settings_page(int page, int index)
{
    char v[96], v2[32];
    int home_on = S.home_mode[0] && strcmp(S.home_mode, "off");
    switch (page) {
    case PAGE_HOME:
        sheet_open("Remote");
        settings_row("Home Button", home_on ? "Opens Home+" : "Opens LG Home", ACT_HOME_TOGGLE, 0, 0, 0);
        if (home_on && S.noflash_available) settings_row("No Flash", S.noflash_active ? "On" : "Off", ACT_HOME_MODE, 0, 0, 0);
        if (S.settings_key_available) { /* needs LG Input Hook, which catches the keys */
            settings_row("Settings Button", S.settings_key ? "Control Center" : "LG Quick Settings", ACT_SETTINGS_KEY, 0, 0, 0);
            settings_row("Input Button", S.input_key ? "Home+ Inputs" : "LG Input Picker", ACT_INPUT_KEY, 0, 0, 0);
        }
        if (!S.noflash_available)
            sheet_body(S.settings_key_available ? "The Settings and Input buttons can open the\n"
                                                  "Control Center and its inputs, over other apps too."
                                                : "On this TV, LG Input Hook from Homebrew Channel opens Home+\n"
                                                  "without LG\xE2\x80\x99s home screen showing first (see the README).");
        break;
    case PAGE_DISPLAY:
        sheet_open("Display & OLED");
        sheet_body("An idle screen dims, then goes black,\n"
                   "and the picture moves a pixel every minute.\n"
                   "Calm Colours keeps ambient lights from flooding the room.");
        settings_row("Dim After", minutes(S.idle_dim_ms, IDLE_DIM_MS, v, sizeof v), ACT_DIM, 0, 0, 0);
        settings_row("Turn Off After", minutes(S.idle_off_ms, IDLE_OFF_MS, v2, sizeof v2), ACT_OFF, 0, 0, 0);
        settings_row("Pixel Shift", S.orbit_off ? "Off" : "On", ACT_ORBIT, 0, 0, 0);
        settings_row("Calm Colours", S.vivid_covers ? "Off" : "On", ACT_CALM, 0, 0, 0);
        settings_row("Top Shelf", S.shelf_still ? "Stays Still" : "Turns Pages", ACT_SHELF, 0, 0, 0);
        settings_row("App Icons", LOOK_NAME[S.icon_look], ACT_PAGE, PAGE_ICONS, 1, 0);
        break;
    case PAGE_ICONS:
        sheet_open("App Icons");
        sheet_body(S.icon_look == LOOK_TINTED ? "Left and Right on the colours pick another."
                                               : "Dark, Clear and Tinted show every app\xE2\x80\x99s logo the same way.");
        for (int l = LOOK_DEFAULT; l <= LOOK_TINTED; l++) {
            sheet_add(LOOK_NAME[l], ACT_ICON_LOOK, l, 0);
            S.sheet.it[S.sheet.n - 1].check = S.icon_look == l;
        }
        if (S.icon_look == LOOK_TINTED) {
            settings_row(TINTS[S.icon_tint].name, "", ACT_ICON_TINT, 0, 0, 0);
            S.sheet.it[S.sheet.n - 1].swatches = 1;
        }
        break;
    case PAGE_CC:
        sheet_open("Control Center");
        settings_row("Blur Over Video", S.cc_blur_video ? "On" : "Off", ACT_CC_BLUR_VIDEO, 0, 0, 0);
        sheet_body("Over other apps, the Control Center\xE2\x80\x99s glass blurs what\xE2\x80\x99s behind it.\n"
                   "The TV captures video for one app at a time, and PicCap has it:\n"
                   "Blur Over Video pauses PicCap while the Control Center is open,\n"
                   "so its glass can blur the video too.");
        break;
    case PAGE_ACCESS:
        sheet_open("Accessibility");
        settings_row("Reduce Motion", S.reduce_motion ? "On" : "Off", ACT_REDUCE_MOTION, 0, 0, 0);
        settings_row("Reduce Transparency", S.reduce_transparency ? "On" : "Off", ACT_REDUCE_TRANSPARENCY, 0, 0, 0);
        break;
    case PAGE_ACCOUNTS:
        sheet_open("Accounts");
        if (S.rail_stremio || stremio_installed())
            settings_row("Stremio", S.rail_stremio ? "Signed In" : "Not Signed In",
                         S.rail_stremio ? ACT_ACCOUNT_ASK : ACT_MEDIA_LINK, SRC_STREMIO, 0, 0);
        settings_row("Trakt", S.trakt_connected ? "Connected" : S.trakt_configured ? "Not Connected" : "Not Set Up",
                     S.trakt_connected ? ACT_ACCOUNT_ASK : ACT_TRAKT_LINK, SRC_COUNT, 0, 0); /* SRC_COUNT: Trakt */
        for (int src = SRC_JELLYFIN; src < SRC_COUNT; src++)
            if (S.src_connected[src] || source_installed(src))
                settings_row(SRC_NAME[src], S.src_connected[src] ? "Connected" : "Not Connected",
                             S.src_connected[src] ? ACT_ACCOUNT_ASK : ACT_MEDIA_LINK, src, 0, 0);
        settings_row("TMDB", S.tmdb_configured ? "Set Up" : "Not Set Up", ACT_TMDB, 0, 0, 0);
        sheet_body("Each app\xE2\x80\x99s Continue Watching, or what\xE2\x80\x99s popular on it, shows when you focus it in the top row.");
        break;
    case PAGE_TV:
        sheet_open("TV Settings");
        for (int i = 0; i < S.nsettings && S.sheet.n < MAX_SHEET - 1; i++) {
            settings_row(S.settings[i].label, "", ACT_LAUNCH_ID, 0, 1, 0);
            copy_id(S.sheet.it[S.sheet.n - 1].id, S.settings[i].id);
        }
        break;
    case PAGE_ABOUT: {
        sheet_open("About");
        char keys[64] = "Every remote key so far is one Home+ knows.";
        if (S.last_unknown_key) snprintf(keys, sizeof keys, "Last remote key Home+ ignored: %d", S.last_unknown_key);
        char about[700];
        snprintf(about, sizeof about, "Home+ %s, %s\n%s\nMIT licence \xC2\xB7 github.com/pbalazaar/home-plus\n"
                 "Not affiliated with LG, Apple, Stremio, Trakt, Jellyfin or Plex.\n"
                 "Uses the TMDB API but is not endorsed or certified by TMDB.",
                 S.svc_version[0] ? S.svc_version : "\xE2\x80\x93",
                 S.svc_elevated ? "set up with Homebrew Channel" : "not set up yet", keys);
        sheet_body(about);
        break;
    }
    default: {
        page = PAGE_MAIN;
        sheet_open("Settings");
        settings_row("Remote", home_on ? "Home Opens Home+" : "", ACT_PAGE, PAGE_HOME, 1, 0);
        snprintf(v, sizeof v, "Dims After %s", minutes(S.idle_dim_ms, IDLE_DIM_MS, v2, sizeof v2));
        settings_row("Display & OLED", S.idle_dim_ms == IDLE_NEVER ? "Never Dims" : v, ACT_PAGE, PAGE_DISPLAY, 1, 0);
        if (S.settings_key_available && S.piccap) /* over other apps, with PicCap: Blur Over Video */
            settings_row("Control Center", S.cc_blur_video ? "Blurs Video" : "", ACT_PAGE, PAGE_CC, 1, 0);
        settings_row("Sounds", S.sounds_off ? "Off" : "On", ACT_SOUNDS, 0, 0, 0);
        settings_row("Clock", S.clock24 ? "24-Hour" : "12-Hour", ACT_CLOCK, 0, 0, 0);
        settings_row("Accessibility", S.reduce_motion || S.reduce_transparency ? "On" : "Off", ACT_PAGE, PAGE_ACCESS, 1, 0);
        char acc[96] = "";
        const char *names[] = { S.rail_stremio ? "Stremio" : "", S.trakt_connected ? "Trakt" : "",
                                S.src_connected[SRC_JELLYFIN] ? "Jellyfin" : "", S.src_connected[SRC_PLEX] ? "Plex" : "" };
        for (int i = 0; i < 4; i++) {
            if (!names[i][0]) continue;
            if (acc[0]) append(acc, sizeof acc, ", ");
            append(acc, sizeof acc, names[i]);
        }
        settings_row("Accounts", acc[0] ? acc : "None", ACT_PAGE, PAGE_ACCOUNTS, 1, 0);
        if (S.nhidden) {
            snprintf(v, sizeof v, "%d", S.nhidden);
            settings_row("Hidden Apps", v, ACT_HIDDEN, 0, 1, 0);
        }
        if (S.nsettings) settings_row("TV Settings", "", ACT_PAGE, PAGE_TV, 1, 0);
        settings_row("About", S.svc_version, ACT_PAGE, PAGE_ABOUT, 1, 0);
        settings_row("Remove Home+", "", ACT_REMOVE_ASK, 0, 1, 1);
    }
    }
    sheet_add("Done", ACT_CANCEL, 0, 0);
    S.sheet.page = page;
    S.sheet.parent = page == PAGE_MAIN ? 0 : page == PAGE_ICONS ? PAGE_DISPLAY : PAGE_MAIN;
    S.sheet.parent_index = main_index(page);
    S.sheet.wide = 1;
    /* back on the row it was on, already scrolled to it */
    int nfoot = sheet_footer(), nlist = S.sheet.n - nfoot, rows = sheet_rows(nfoot);
    S.sheet.index = index > 0 && index < S.sheet.n ? index : 0;
    S.sheet.first = S.sheet.index < nlist ? keep_in_view(0, S.sheet.index, rows, nlist, rows > 4) : 0;
    S.sheet.scroll = (float)S.sheet.first;
}

/* after changing a setting: its page again, on the same row */
void settings_again(void)
{
    if (!S.at_page) return;
    settings_page(S.at_page, S.at_index);
    if (S.at_page != PAGE_MAIN) S.sheet.parent_index = S.at_parent_index;
}

#define SWATCH_PITCH 50
float swatch_x(float bx, float bw, int i)
{
    return bx + bw - 36 - SWATCH_D / 2.0f - (NTINTS - 1 - i) * SWATCH_PITCH;
}

/* the colour under the pointer on that row, or -1 */
int swatch_at(float mx)
{
    float bx = S.sheet.px + PAD, bw = S.sheet.pw - 2 * PAD;
    for (int i = 0; i < NTINTS; i++)
        if (fabsf(mx - swatch_x(bx, bw, i)) < SWATCH_PITCH / 2.0f) return i;
    return -1;
}

/* Tinted takes colour i: the tiles behind the sheet change, its row says the colour's name */
void icon_tint_set(int i)
{
    char body[48];
    int index = S.sheet.index, parent_index = S.sheet.parent_index;
    S.icon_tint = i;
    snprintf(body, sizeof body, "{\"iconTint\":\"%s\"}", TINTS[i].name);
    net_post("/api/prefs", body, 0);
    settings_page(PAGE_ICONS, index);
    S.sheet.parent_index = parent_index;
}

void open_settings_sheet(void)
{
    settings_page(PAGE_MAIN, 0);
}
