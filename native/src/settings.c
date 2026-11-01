/* Home+ native: Settings, like tvOS's: its pages listed on the left, the one on show on the right. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "home.h"

/* ---- Settings: each row shows its current value under its name's line on what it does; OK or Left
 * and Right change it, › opens another sheet, and Back goes back to the list of pages. */

void settings_row(const char *label, const char *value, Action act, int arg, int chevron, int destructive)
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

/* Settings' pages, in the list on the left: each with its glyph and a line on what's in it */
static const struct { int page; const char *name, *about; int icon; } CATS[] = {
    { PAGE_HOME, "Remote", "What the Home, Settings and Input buttons open.", ICON_REMOTE },
    { PAGE_DISPLAY, "Display & OLED", "Looking after the screen while Home+ is on it.", ICON_BRIGHTNESS },
    { PAGE_ICONS, "App Tiles", "How every app\xE2\x80\x99s tile looks.", ICON_TILES },
    { PAGE_CC, "Control Center", "The Control Center over other apps.", ICON_CONTROLS },
    { PAGE_GENERAL, "General", "Sounds, the clock and hidden apps.", ICON_SETTINGS },
    { PAGE_ACCESS, "Accessibility", "Less motion, and less see-through glass.", ICON_ACCESS },
    { PAGE_ACCOUNTS, "Accounts", "Continue Watching, and what\xE2\x80\x99s popular on your apps.", ICON_PERSON },
    { PAGE_TV, "TV Settings", "LG\xE2\x80\x99s own settings.", ICON_TV },
    { PAGE_ABOUT, "About", "Home+ itself, and removing it.", ICON_INFO },
};
#define NCATS ((int)(sizeof CATS / sizeof CATS[0]))

/* the pages this TV has a use for */
static int cat_shown(int page)
{
    if (page == PAGE_CC) return S.settings_key_available && S.piccap; /* over other apps, with PicCap: Blur Over Video */
    if (page == PAGE_TV) return S.nsettings > 0;
    return 1;
}

static void cats_build(void)
{
    S.sheet.ncat = 0;
    for (int i = 0; i < NCATS && S.sheet.ncat < MAX_CATS; i++) {
        if (!cat_shown(CATS[i].page)) continue;
        int c = S.sheet.ncat++;
        S.sheet.cat_page[c] = CATS[i].page;
        S.sheet.cat_icon[c] = CATS[i].icon;
        S.sheet.cat_tex[c] = text_render(S.f_medium, 29, CATS[i].name, 340, &S.sheet.cat_w[c], &S.sheet.cat_h[c]);
    }
}

/* where a page is in the list on the left (-1: not one of them) */
static int cat_of(int page)
{
    for (int c = 0; c < S.sheet.ncat; c++)
        if (S.sheet.cat_page[c] == page) return c;
    return -1;
}

/* Settings rows Left and Right change in place: a switch either way, a choice of times one step along */
int settings_steps(Action act)
{
    switch (act) {
    case ACT_HOME_TOGGLE: case ACT_HOME_MODE: case ACT_SETTINGS_KEY: case ACT_INPUT_KEY:
    case ACT_DIM: case ACT_OFF: case ACT_ORBIT: case ACT_CALM: case ACT_SHELF: case ACT_SHELF_DETAILS:
    case ACT_SOUNDS: case ACT_CLOCK: case ACT_REDUCE_MOTION: case ACT_REDUCE_TRANSPARENCY: case ACT_CC_BLUR_VIDEO:
        return 1;
    default:
        return 0;
    }
}

/* a line on what the last row does, under its name */
static void settings_desc(const char *text)
{
    if (!S.sheet.n) return;
    SheetItem *it = &S.sheet.it[S.sheet.n - 1];
    tex_free(&it->desc);
    it->desc = text_render(S.f_medium, SPLIT_DESC_PX, text, 760, &it->dw, &it->dh);
}

static void row(const char *label, const char *value, Action act, int arg, int chevron, const char *desc)
{
    settings_row(label, value, act, arg, chevron, 0);
    if (desc) settings_desc(desc);
}

/* Settings: its pages on the left, the one on show on the right. PAGE_MAIN is the list itself, with
 * focus on page index (shown on the right); any other page has focus on its row index. */
void settings_page(int page, int index)
{
    if (page == PAGE_WELCOME) { /* the welcome steps (setup.c): their choices are Settings' own */
        welcome_page(index);
        return;
    }
    char v[96], v2[32];
    int home_on = S.home_mode[0] && strcmp(S.home_mode, "off");
    sheet_open("Settings");
    cats_build();
    int side = cat_of(page) < 0, cat = side ? index : cat_of(page);
    if (cat < 0 || cat >= S.sheet.ncat) cat = 0;
    page = S.sheet.cat_page[cat];
    if (side) index = 0;
    for (int i = 0; i < NCATS; i++) {
        if (CATS[i].page != page) continue;
        S.sheet.ptitle = text_render(S.f_semi, SPLIT_TITLE_PX, CATS[i].name, 900, &S.sheet.ptw, &S.sheet.pth);
        S.sheet.psub = text_render(S.f_medium, SHEET_BODY_PX, CATS[i].about, 1000, &S.sheet.psw, &S.sheet.psh);
    }
    switch (page) {
    case PAGE_HOME:
        row("Home Button", home_on ? "Opens Home+" : "Opens LG Home", ACT_HOME_TOGGLE, 0, 0,
            "Home+ in place of LG\xE2\x80\x99s home screen, after apps too.");
        if (home_on && S.noflash_available)
            row("No Flash", S.noflash_active ? "On" : "Off", ACT_HOME_MODE, 0, 0, "LG\xE2\x80\x99s home screen never shows, not even for a moment.");
        if (S.settings_key_available) { /* Home+'s key filter or LG Input Hook catches the keys */
            row("Settings Button", S.settings_key ? "Control Center" : "LG Quick Settings", ACT_SETTINGS_KEY, 0, 0,
                "The Control Center, over other apps too.");
            row("Input Button", S.input_key ? "Home+ Inputs" : "LG Input Picker", ACT_INPUT_KEY, 0, 0,
                "The Control Center\xE2\x80\x99s inputs, over other apps too.");
        }
        if (!S.noflash_available && !S.settings_key_available)
            sheet_body("On this TV, LG Input Hook from Homebrew Channel opens Home+\n"
                       "without LG\xE2\x80\x99s home screen showing first (see the README).");
        break;
    case PAGE_DISPLAY:
        row("Dim After", minutes(S.idle_dim_ms, IDLE_DIM_MS, v, sizeof v), ACT_DIM, 0, 0,
            "With no key pressed, all but the focused tile dims.");
        row("Turn Off After", minutes(S.idle_off_ms, IDLE_OFF_MS, v2, sizeof v2), ACT_OFF, 0, 0,
            "Then the screen fades to black. A key wakes it.");
        row("Pixel Shift", S.orbit_off ? "Off" : "On", ACT_ORBIT, 0, 0, "The picture moves a pixel every minute.");
        row("Calm Colours", S.vivid_covers ? "Off" : "On", ACT_CALM, 0, 0, "Dark app backgrounds, so ambient lights stay calm.");
        row("Top Shelf", S.shelf_still ? "Stays Still" : "Turns Pages", ACT_SHELF, 0, 0,
            "A new title every 10 seconds, or only when you ask.");
        row("Top Shelf Details", S.shelf_details == SHELF_DETAILS_OFF ? "Off" :
            S.shelf_details == SHELF_DETAILS_COMPACT ? "Compact" : "Full", ACT_SHELF_DETAILS, 0, 0,
            "Off: open hint. Compact: genre and rating. Full: synopsis.");
        break;
    case PAGE_ICONS: {
        static const char *const about[] = { "Each app\xE2\x80\x99s own colours.", "Each logo on a dark tile.",
                                             "Frosted glass, with white logos.", "Glass in a colour, with the logo in a pale tint of it." };
        for (int l = LOOK_DEFAULT; l <= LOOK_TINTED; l++) {
            sheet_add(LOOK_NAME[l], ACT_ICON_LOOK, l, 0);
            S.sheet.it[S.sheet.n - 1].check = S.icon_look == l;
            settings_desc(about[l]);
        }
        if (S.icon_look == LOOK_TINTED) {
            settings_row(TINTS[S.icon_tint].name, "", ACT_ICON_TINT, 0, 0, 0);
            S.sheet.it[S.sheet.n - 1].swatches = 1;
            settings_desc("Left and Right pick another colour.");
        }
        break;
    }
    case PAGE_CC:
        row("Blur Over Video", S.cc_blur_video ? "On" : "Off", ACT_CC_BLUR_VIDEO, 0, 0,
            "Pauses PicCap while the Control Center is open.");
        sheet_body("Over other apps, the Control Center\xE2\x80\x99s glass blurs what\xE2\x80\x99s behind it.\n"
                   "The TV captures video for one app at a time, and PicCap has it:\n"
                   "Blur Over Video pauses PicCap while the Control Center is open,\n"
                   "so its glass can blur the video too.");
        break;
    case PAGE_GENERAL:
        row("Sounds", S.sounds_off ? "Off" : "On", ACT_SOUNDS, 0, 0, "A soft tick when focus moves.");
        row("Clock", S.clock24 ? "24-Hour" : "12-Hour", ACT_CLOCK, 0, 0, "The clock at the top right.");
        if (S.nhidden) {
            snprintf(v, sizeof v, "%d", S.nhidden);
            row("Hidden Apps", v, ACT_HIDDEN, 0, 1, "Apps you\xE2\x80\x99ve hidden, to show them again.");
        }
        break;
    case PAGE_ACCESS:
        row("Reduce Motion", S.reduce_motion ? "On" : "Off", ACT_REDUCE_MOTION, 0, 0,
            "Menus appear in place, without growing or sliding.");
        row("Reduce Transparency", S.reduce_transparency ? "On" : "Off", ACT_REDUCE_TRANSPARENCY, 0, 0,
            "Glass is less see-through, so text stands out.");
        break;
    case PAGE_ACCOUNTS:
        if (S.rail_stremio || stremio_installed())
            row("Stremio", S.rail_stremio ? "Signed In" : "Not Signed In", S.rail_stremio ? ACT_ACCOUNT_ASK : ACT_MEDIA_LINK, SRC_STREMIO, 0,
                "What you\xE2\x80\x99re partway through, with your Trakt progress.");
        row("Trakt", S.trakt_connected ? "Connected" : S.trakt_configured ? "Not Connected" : "Not Set Up",
            S.trakt_connected ? ACT_ACCOUNT_ASK : S.trakt_configured ? ACT_TRAKT_LINK : ACT_KEYS, SRC_COUNT, 0, /* SRC_COUNT: Trakt */
            "Paused playback, in Stremio\xE2\x80\x99s Top Shelf.");
        for (int src = SRC_JELLYFIN; src < SRC_COUNT; src++)
            if (S.src_connected[src] || source_installed(src))
                row(SRC_NAME[src], S.src_connected[src] ? "Connected" : "Not Connected",
                    S.src_connected[src] ? ACT_ACCOUNT_ASK : ACT_MEDIA_LINK, src, 0, "What you\xE2\x80\x99re partway through on your server.");
        row("TMDB", S.tmdb_configured ? "Set Up" : "Not Set Up", ACT_KEYS, 0, 0, "What\xE2\x80\x99s popular on your streaming apps.");
        sheet_body("Each app\xE2\x80\x99s Continue Watching, or what\xE2\x80\x99s popular on it, shows when you focus it in the top row.");
        break;
    case PAGE_TV:
        for (int i = 0; i < S.nsettings && S.sheet.n < MAX_SHEET - 1; i++) {
            settings_row(S.settings[i].label, "", ACT_LAUNCH_ID, 0, 1, 0);
            copy_id(S.sheet.it[S.sheet.n - 1].id, S.settings[i].id);
        }
        break;
    case PAGE_ABOUT: {
        char keys[64] = "Every remote key so far is one Home+ knows.";
        if (S.last_unknown_key) snprintf(keys, sizeof keys, "Last remote key Home+ ignored: %d", S.last_unknown_key);
        char about[700];
        snprintf(about, sizeof about, "Home+ %s, %s\n%s\nMIT licence \xC2\xB7 github.com/pbalazaar/home-plus\n"
                 "Not affiliated with LG, Apple, Stremio, Trakt, Jellyfin or Plex.\n"
                 "Uses the TMDB API but is not endorsed or certified by TMDB.",
                 S.svc_version[0] ? S.svc_version : "\xE2\x80\x93",
                 S.svc_elevated ? "set up with Homebrew Channel" : "not set up yet", keys);
        sheet_body(about);
        settings_row("Remove Home+", "", ACT_REMOVE_ASK, 0, 1, 1);
        settings_desc("Undoes everything Home+ set up, then uninstalls it.");
        break;
    }
    }
    S.sheet.split = 1;
    S.sheet.side = side;
    S.sheet.cat = cat;
    S.sheet.page = side ? PAGE_MAIN : page;
    S.sheet.parent = side ? 0 : PAGE_MAIN; /* Back: from a page to the list, from the list out */
    S.sheet.parent_index = cat;
    /* back on the row it was on, already scrolled to it */
    int rows = split_rows();
    S.sheet.index = index > 0 && index < S.sheet.n ? index : 0;
    S.sheet.first = keep_in_view(0, S.sheet.index, rows, S.sheet.n, rows > 4);
    S.sheet.scroll = (float)S.sheet.first;
}

/* from the list on the left into the page on show (App Tiles: on the look that's on) */
void settings_enter(void)
{
    if (!S.sheet.split || !S.sheet.side || !S.sheet.n) return;
    int page = S.sheet.cat_page[S.sheet.cat];
    settings_page(page, page == PAGE_ICONS ? S.icon_look : 0);
}

/* Left or Right on a row that steps: as OK would, a step back for Left */
static void settings_step(int dir)
{
    SheetItem it = S.sheet.it[S.sheet.index];
    S.at_page = S.sheet.page;
    S.at_index = S.sheet.index;
    S.at_parent_index = S.sheet.parent_index;
    snd_play(SND_TICK);
    run_action(it.act, dir < 0 ? -1 : it.arg, it.id);
}

/* Settings' own keys (1: handled). Up and Down in the list show each page; Right or OK goes into it.
 * In a page, Left and Right change a row in place; elsewhere Left goes back to the list. */
int settings_keydown(SDL_Keycode k)
{
    if (!S.sheet.split) return 0;
    if (S.sheet.side) {
        if (k == SDLK_UP && S.sheet.cat > 0) settings_page(PAGE_MAIN, S.sheet.cat - 1);
        else if (k == SDLK_DOWN && S.sheet.cat < S.sheet.ncat - 1) settings_page(PAGE_MAIN, S.sheet.cat + 1);
        else if (k == SDLK_RIGHT) settings_enter();
        else return k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT;
        return 1;
    }
    if (k != SDLK_LEFT && k != SDLK_RIGHT) return 0;
    SheetItem *it = &S.sheet.it[S.sheet.index];
    if (it->swatches) return 0; /* Tinted's colours: input.c */
    if (settings_steps(it->act)) settings_step(k == SDLK_RIGHT ? 1 : -1);
    else if (k == SDLK_LEFT) settings_page(PAGE_MAIN, S.sheet.cat);
    return 1;
}

/* The pointer over Settings (1: something OK acts on). Over the list, it shows that page; over a row,
 * focus goes there, into the page. Mirrors draw_split()'s layout. */
int settings_pointer(int mx, int my, int apply)
{
    float px = S.sheet.px, py = S.sheet.py;
    if (mx >= px + PAD && mx < px + SPLIT_SIDE - PAD / 2) {
        float top = py + PAD + S.sheet.th + 32;
        int c = (int)floorf((my - top) / (SPLIT_CAT + SPLIT_CAT_GAP));
        if (my < top || c < 0 || c >= S.sheet.ncat) return 0;
        if (apply && (!S.sheet.side || S.sheet.cat != c)) settings_page(PAGE_MAIN, c);
        return 1;
    }
    if (mx < S.sheet.lx || mx > S.sheet.lx + S.sheet.lw || my < S.sheet.ly) return 0;
    float step = SPLIT_ROW + SPLIT_GAP, r = (my - S.sheet.ly) / step + S.sheet.scroll;
    int i = (int)floorf(r), rows = split_rows();
    if (r - i > SPLIT_ROW / step || i < 0 || i >= S.sheet.n || (my - S.sheet.ly) / step >= rows) return 0;
    if (apply) {
        if (S.sheet.side) {
            int first = S.sheet.first;
            settings_page(S.sheet.cat_page[S.sheet.cat], i);
            S.sheet.first = first, S.sheet.scroll = (float)first; /* (as it is, under the pointer) */
        } else {
            S.sheet.index = i;
        }
    }
    return 1;
}

/* after changing a setting: its page again, on the same row */
void settings_again(void)
{
    if (!S.at_page) return;
    settings_page(S.at_page, S.at_index);
}

#define SWATCH_PITCH 50
float swatch_x(float bx, float bw, int i)
{
    return bx + bw - 36 - SWATCH_D / 2.0f - (NTINTS - 1 - i) * SWATCH_PITCH;
}

/* the colour under the pointer on that row, or -1 */
int swatch_at(float mx)
{
    float bx = S.sheet.split ? S.sheet.lx : S.sheet.px + PAD, bw = S.sheet.split ? S.sheet.lw : S.sheet.pw - 2 * PAD;
    for (int i = 0; i < NTINTS; i++)
        if (fabsf(mx - swatch_x(bx, bw, i)) < SWATCH_PITCH / 2.0f) return i;
    return -1;
}

/* Tinted takes colour i: the tiles behind the sheet change, its row says the colour's name */
void icon_tint_set(int i)
{
    char body[48];
    int index = S.sheet.index;
    S.icon_tint = i;
    snprintf(body, sizeof body, "{\"iconTint\":\"%s\"}", TINTS[i].name);
    net_post("/api/prefs", body, 0);
    settings_page(PAGE_ICONS, index);
}

void open_settings_sheet(void)
{
    settings_page(PAGE_MAIN, 0);
}
