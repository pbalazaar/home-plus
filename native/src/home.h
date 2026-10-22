/* Home+ native: the launcher's shared layout, types and state (S, its one global), and what each of its files
 * offers the others. render.h, gfx.h, net.h and sound.h are the libraries under it.
 *
 * Its files, each with a feature's data, logic and drawing:
 *   main.c      start-up, the frame loop, update()          results.c  the service's answers
 *   apps.c      the app list, your layout, launching        shelf.c    the Top Shelf and its carousel
 *   cc.c        the Control Center                          cc_list.c  its lists and gallery
 *   sheet.c     sheets (menus)                              settings.c Settings' pages
 *   actions.c   what sheets' buttons do                     recents.c  Recents (the App Switcher)
 *   setup.c     first run                                   accounts.c signing in (Stremio, Jellyfin, Plex, Trakt)
 *   input.c     keys, the pointer, focus                    idle.c     the idle wait, OLED care, offscreen
 *   draw.c      shared drawing, the home screen             tiles.c    an icon's logo and cover
 *   util.c      small helpers                               perf.c     frame timing (off unless asked) */
#ifndef HOMEPLUS_HOME_H
#define HOMEPLUS_HOME_H

#include <math.h>

#include <SDL.h>

#include "cJSON.h"
#include "gfx.h"
#include "net.h"
#include "sound.h"

/* ------------------------------------------------------------------ layout */

/* Spacing system, measured from Apple's tvOS 26 Home screen: the dock spans
 * x 48-1872 and y 785-1015, and inside it six columns of 5:3 tiles sit 50 px
 * apart, 43 px from its ends and 40 px from its top and bottom. Containers keep
 * their corners concentric with their content (outer radius = inner radius +
 * padding). The Top Shelf text, the rail and the top bar line up with the
 * dock's edges. */
#define W 1920
#define H 1080
#define SAFE_X 80
#define SAFE_Y 60
#define COLS 6
#define TILE_W 248
#define TILE_H 150
#define TILE_R 22                      /* every card's corner: app tiles, Continue Watching, the big Top Shelf tile */
#define PAD 40                         /* the dock's padding above and below the tiles */
#define PAD_X 43                       /* and at its ends */
#define DOCK_X 48                      /* glass platform behind the top row, almost the screen's width */
#define DOCK_W (W - 2 * DOCK_X)
#define GAP_X ((DOCK_W - 2 * PAD_X - COLS * TILE_W) / (COLS - 1)) /* 50 */
#define LEFT (DOCK_X + PAD_X)          /* first column */
#define RIGHT (W - DOCK_X)             /* the top bar ends where the dock does */
#define DOCK_H (TILE_H + 2 * PAD)
#define DOCK_Y (1015 - DOCK_H)
#define GRID_TOP (DOCK_Y + PAD)
#define ROW_H (TILE_H + 80)            /* the next row peeks out under the dock; labels sit in the gap */
#define FOCUS_SCALE 1.20f
#define LONG_PRESS_MS 650
#define ROTATE_MS 10000                /* the Top Shelf carousel turns to its next title */
#define PAGE_WAIT_MS 1200              /* the longest the Top Shelf waits for a page's picture */
#define SKEL_MS 250                    /* a wait longer than this shows placeholders (a skeleton) */
#define MAX_DOTS 16                    /* its position, as dots */
#define BAR 56                         /* the clock capsule */
#define TOP_Y 52                       /* where tvOS 26 has it */
#define TOP_DOT (BAR - 10)             /* its circle: Settings, where tvOS shows the profile picture */
#define SW_W 680                       /* App Switcher cards: the app's tile, large */
#define SW_H (SW_W * TILE_H / TILE_W)
#define SW_PITCH (SW_W + 80)
#define SW_CY 500                      /* their centre line */
#define SHEET_BH 80                    /* sheet buttons, and the gap between them */
#define SHEET_GAP 16
#define SHEET_TINT 16, 16, 20, 135 /* sheets' glass: dark tint, 53% */
#define SHEET_W 720                    /* a sheet (Settings' are wider: a label and its value) */
#define SHEET_TITLE_PX 30              /* its title, its words, its buttons */
#define SHEET_BODY_PX 26
#define SHEET_BUTTON_PX 31
#define RED_ON_GLASS 255, 90, 80       /* what can't be undone: its label on glass, */
#define RED_ON_WHITE 215, 40, 40       /* and on a focused (white) button */
#define ICON_FIT 256                   /* icons feed logos, covers and the big Top Shelf tile */
#define THUMB_FIT_W 480                /* rail thumbnails: cards, and briefly the background */
#define THUMB_FIT_H 270
#define TEXT_STATIC 218                /* OLED: static text stays at ~85% white */
#define TEXT_SECONDARY 179             /* 70%: values, details, sheets' words, hints */
#define TEXT_TERTIARY 140              /* 55%: group names, a row's âº, Settings' values */
#define HINT_PX 26                     /* a hint: what the remote's keys do here */
#define HEADING_PX 27                  /* the clock, and the Top Shelf's heading as big */
#define BLUR_DIM 110                   /* the blurred picture behind the grid and Recents, dimmed (of 255) */

#define MAX_APPS 128
#define MAX_RAIL 16
#define MAX_SHEET 64
#define ID_LEN 160

enum { TAG_APPS = 1, TAG_LAYOUT, TAG_STREMIO, TAG_MEDIA_LINK, TAG_MEDIA_STATUS, TAG_QR, TAG_ICON, TAG_ART, TAG_RAIL_LOGO, TAG_RAIL_BG, TAG_FEATURED, TAG_CONTROL, TAG_LAUNCH, TAG_WAKE,
       TAG_SYSTEM, TAG_TRAKT_LINK, TAG_TRAKT_STATUS, TAG_RUNNING, TAG_CLOSED, TAG_HOME, TAG_HOME_SET, TAG_KEY_SET,
       TAG_STATUS, TAG_ELEVATE, TAG_RESTART, TAG_UNINSTALL,
       TAG_PREFS, TAG_EVENTS, TAG_DELETED };

/* Media sources: each one's Continue Watching is the Top Shelf of its own app */
enum { SRC_NONE, SRC_STREMIO, SRC_JELLYFIN, SRC_PLEX, SRC_COUNT };
#define SRC_FEATURED (SRC_COUNT + 1)   /* a streaming app's popular titles (TMDB); not an index */

/* Control Center, like tvOS's: opened from the clock, its controls float under it on the
 * right, on Apple's grid of four round-button-sized units (91 px, 20 px apart). Which
 * controls, in what order and size, comes from the service, and is yours to change (like
 * iOS 18's): each takes the first space it fits, in order. */
#define CC_U 91
#define CC_G 20
#define CC_ROWS 7       /* the rows under the clock (the service's 28 units); editing's hint and Reset still fit under them */
#define MAX_CC 28       /* the service caps the list at 28 units */
#define MAX_CAT 32      /* the catalogue */
#define CC_TITLE_PX 19  /* a pill's name: "Sleep Timer" and "Eye Comfort" fit, with room either side */
#define CC_LIST_ROW_H 56 /* a list in the Control Center's place (Picture Mode, Inputs): its rows, */
#define CC_LIST_GAP 6    /* the gap between them, */
#define CC_LIST_HEAD 66  /* and its title's band */
#define CC_LIST_PX 24    /* a row's name, and its tick */
#define CC_DETAIL_PX 18  /* a row's detail: "Nothing Connected" */

/* A control on show: what the service says it is, and where it's placed (cc_pack) */
typedef struct {
    char id[24], kind[8], title_s[48], name_s[48], value_s[72]; /* name: the whole one (the tile's may be short) */
    int icon, units, on, stateful, available; /* units: 1 Small, 2 Wide, 4 Large; stateful: it can be off */
    int col, row, w, h;              /* on the grid; row -1: no room for it */
    Tex *title, *value;
    int tw, th, vw, vh;
    float s, sv;                     /* focus */
    Uint32 flash, shake;             /* a Small stepper's new value shows for a moment; a refusal shakes it */
} CCItem;

/* Settings pages (a sheet's page; 0 = not a Settings page) */
enum { PAGE_MAIN = 1, PAGE_HOME, PAGE_DISPLAY, PAGE_CC, PAGE_ACCESS, PAGE_ACCOUNTS, PAGE_TV, PAGE_ABOUT, PAGE_CONFIRM, PAGE_SIGNIN, PAGE_ICONS };

/* zones: what the remote is moving through */
enum { ZONE_GRID, ZONE_RAIL, ZONE_TOP };
/* how app tiles look (Settings -> Display & OLED -> App Tiles): their own colours; dark tiles;
 * glass with white logos; glass in a colour */
enum { LOOK_DEFAULT, LOOK_DARK, LOOK_CLEAR, LOOK_TINTED };
/* Tinted's colours: Apple's system colours, as on a dark screen */
static const struct {
    const char *name;
    Uint8 rgb[3];
} TINTS[] = { { "Red", { 255, 69, 58 } },    { "Orange", { 255, 159, 10 } }, { "Yellow", { 255, 214, 10 } },
              { "Green", { 48, 209, 88 } },  { "Teal", { 64, 200, 224 } },   { "Blue", { 10, 132, 255 } },
              { "Indigo", { 94, 92, 230 } }, { "Purple", { 191, 90, 242 } }, { "Pink", { 255, 55, 95 } },
              { "Graphite", { 142, 142, 147 } } };
#define NTINTS (int)(sizeof TINTS / sizeof TINTS[0])
#define TINT_INDIGO 6
/* the top bar: the clock capsule, with Settings in the circle at its end (Recents is
 * Back on the first app, or Red; Inputs are the remote's Input key) */
enum { TOP_SETTINGS, TOP_COUNT };
#define MAX_RUNNING 32
#define MAX_SYS 16

typedef struct {
    char id[ID_LEN], title[ID_LEN], icon[ID_LEN + 16], art[ID_LEN + 16];
    Uint8 col[3];
    int has_col, fullbleed;
    Tex *icon_tex, *art_tex, *label, *big;
    Tex *logo_tex;          /* the icon's logo, background keyed out and trimmed (tiles) */
    Tex *cover_tex;         /* made from the icon, for the Top Shelf when the app has no artwork */
    float logo_w, logo_h, logo_r; /* its size on a tile, and corner radius for picture icons */
    float ink_ref, ink_span;      /* the luminance its logo stood out from (-1: none), and the most it did */
    float ink_tint;               /* its logo is grey or white on a coloured icon: on a dark tile it takes that colour */
    int icon_state, art_state; /* 0 idle, 1 loading, 2 ready, 3 failed */
    int feat; /* a streaming service with popular titles (TMDB): 0 not asked, 1 asking, 2 yes, 3 no */
    int removable, deleted; /* it can be deleted from the TV; it was, and the app list hasn't caught up */
    int lw, lh, bw, bh;
    float s, sv; /* focus animation 0..1, and its spring velocity */
} App;

/* A title in the Top Shelf carousel: Continue Watching, or popular on a streaming service */
typedef struct {
    char id[64], type[16], name[200], bg[40], logo[40];
    float progress;
    int season, episode, remaining, year;
    Tex *bg_tex, *logo_tex, *title_tex, *meta_tex;
    int texts_made; /* title_tex and meta_tex are made (either can be NULL: a name with nothing to draw) */
    int bg_state, logo_state; /* 0 idle, 1 loading, 2 ready, 3 failed */
    int tw, th, mw, mh, lw, lh;
} RailItem;

/* A picture the Top Shelf can show: app i's artwork, its glow (its icon blurred, with its
 * tile large; the cover: the glow alone), title i's picture, or held picture i (one whose
 * owner let go of it while it was on show, kept until something covers it). */
enum { PIC_NONE, PIC_ART, PIC_GLOW, PIC_COVER, PIC_RAIL, PIC_HELD };
typedef struct { int kind, i; } Pic;

typedef enum { ACT_NONE, ACT_MOVE, ACT_FRONT, ACT_HIDE, ACT_DELETE_ASK, ACT_DELETE, ACT_UNHIDE, ACT_UNHIDE_ALL, ACT_CANCEL,
               ACT_LAUNCH_ID, ACT_TRAKT_LINK, ACT_TRAKT_UNLINK, ACT_MEDIA_LINK, ACT_MEDIA_UNLINK, ACT_ACCOUNT_ASK, ACT_UP, ACT_TMDB, ACT_SHELF,
               ACT_HOME_MODE, ACT_SOUNDS,
               ACT_SETUP_RETRY, ACT_HOME_YES, ACT_HOME_NO, ACT_HOME_TOGGLE, ACT_REMOVE_ASK, ACT_REMOVE,
               ACT_ACCESSIBILITY, ACT_REDUCE_MOTION, ACT_REDUCE_TRANSPARENCY, ACT_CC_BLUR_VIDEO,
               ACT_PAGE, ACT_CLOCK, ACT_DIM, ACT_OFF, ACT_ORBIT, ACT_CALM, ACT_HIDDEN, ACT_SETTINGS_KEY, ACT_INPUT_KEY,
               ACT_ICON_LOOK, ACT_ICON_TINT,
               ACT_CC_EDIT, ACT_CC_ADD, ACT_CC_SIZE, ACT_CC_REMOVE, ACT_CC_RESET } Action;

/* First run: the service has to be running and elevated (root, via Homebrew
 * Channel) before Home+ can list apps. Each step that can fail has a screen. */
enum { SETUP_CHECK, SETUP_STARTING, SETUP_ELEVATING, SETUP_RESTARTING, SETUP_NO_HBC, SETUP_DENIED,
       SETUP_FAILED, SETUP_OLD, SETUP_REMOVING, SETUP_DONE };
#define HBC_ELEVATE "luna://org.webosbrew.hbchannel.service/elevateService"
#define SHEET_BODY_LINES 5
#define IDLE_DIM_MS (2 * 60 * 1000)  /* defaults; /api/prefs dimMinutes / offMinutes override */
#define IDLE_OFF_MS (10 * 60 * 1000)
#define IDLE_NEVER 0xFFFFFFFFu /* Settings: never dim / never turn off */

typedef struct {
    Action act;
    int arg;
    int destructive;
    char id[ID_LEN]; /* ACT_LAUNCH_ID's app; the app of ACT_DELETE_ASK, ACT_DELETE and ACT_UNHIDE */
    Tex *tex;
    int w, h;
    Tex *val; /* Settings rows: the current value, drawn on the right */
    int vw, vh;
    int chevron; /* opens a page */
    int check;   /* the choice that's on: a tick on the right (its label still in the middle) */
    int swatches; /* Tinted's colours, the one that's on ringed: Left and Right pick another */
} SheetItem;

typedef struct {
    SDL_Window *win;
    Font *f_medium, *f_semi, *f_bold;

    /* shared textures, tinted per draw (shapes and glass are shaders) */
    Tex *scrim_v, *scrim_h, *scrim_top;
    /* top bar: a label under the focused button, and the Top Shelf hint while it has focus */
    Tex *top_hint[TOP_COUNT];
    int top_hw[TOP_COUNT], top_hh[TOP_COUNT];
    Tex *rail_hint, *rail_browse, *launch_tex; /* "Press OK to open Stremio", and while browsing; "Opening Netflix…" */
    int rail_hint_w, rail_hint_h, rail_browse_w, rail_browse_h, launch_w, launch_h;
    int launching, launch_item; /* OK on a tile: it breathes until the app is in front */
    Uint32 launch_at;

    App apps[MAX_APPS];
    int napps, generation; /* generation: bumps when the app list is rebuilt */
    char apps_sig[4096];
    int items[MAX_APPS + 1], nitems; /* indices into apps; -1 = "Hidden" tile */

    char order[MAX_APPS][ID_LEN];
    int norder;
    char hidden[MAX_APPS][ID_LEN];
    int nhidden;
    int layout_loaded;

    RailItem rail[MAX_RAIL];      /* the Top Shelf carousel */
    int nrail, rail_connected, rail_index, rail_generation;
    char rail_app[ID_LEN];        /* SRC_FEATURED: whose popular titles */
    Uint32 rail_next;             /* when it turns to its next title (0: it isn't turning) */
    int feat_req, tmdb_configured; /* the app whose titles are on the way (-1: none); TMDB set up */
    int rail_trending;            /* SRC_FEATURED: what's trending, not what's on the app */
    char rail_sig[2048];

    int focus, zone; /* ZONE_* */
    int top_index, top_col; /* the top bar's focus; the column it was entered from */
    float top_s[TOP_COUNT], top_v[TOP_COUNT];
    Tex *top_tex[TOP_COUNT];
    int top_w[TOP_COUNT], top_h[TOP_COUNT];

    /* Control Center data from /api/system */
    struct { char id[ID_LEN], label[96]; } settings[MAX_SYS];
    int nsettings;

    int rail_stremio, trakt_configured, trakt_connected;
    int rail_source, rail_req;        /* whose items the rail holds; the source asked for */
    int src_connected[SRC_COUNT];     /* signed in (Stremio counts Trakt too) */
    int media_poll_src;               /* Jellyfin/Plex sign-in: whose code is on screen */
    Uint32 media_poll;                /* and when to ask again whether it was approved */
    Tex *connect_tex[SRC_COUNT];      /* "Connect … to see Continue Watching" */
    int connect_w[SRC_COUNT], connect_h[SRC_COUNT];
    char home_mode[16]; /* "redirect" (no-flash), "watch" or "off" */
    int sounds_off; /* focus sounds switched off in Settings (saved by the service) */
    Uint32 idle_dim_ms, idle_off_ms; /* OLED idle stages (0 = default) */
    int noflash_active, noflash_available; /* available: LG's home is a web app we can replace */
    int settings_key, settings_key_available; /* the remote's Settings key opens the Control Center (LG Input Hook) */
    int input_key;                    /* ... and its Input key, the Control Center's inputs */
    int cc_blur_video, piccap; /* Settings -> Control Center: Blur Over Video (over other apps, with PicCap installed) */
    Uint32 trakt_poll; /* while showing a Trakt code: next status check */
    int moving;
    float scroll, scroll_v, scroll_target; /* grid y offset */
    float scrolled;              /* 0 at the top row, 1 below it */
    int shelf_cur;               /* the item whose Top Shelf it is */
    /* The Top Shelf shows a page: an app's own (its name), or one of its carousel's titles
     * (rail -1: the app's own; -2: none, its carousel left). The next page waits until its
     * picture is in (PAGE_WAIT_MS at most), so a title never sits on another's picture, and
     * the picture on show stays underneath until the new one has covered it. */
    int page_app, page_rail, page_step; /* step: it came from the title before it, in the same carousel */
    int prev_app, prev_rail;     /* the page that left, fading out */
    float page_a, page_leave, prev_vis; /* the page coming in; the page on show leaving while the next one loads */
    float skel_a;                /* placeholders for the text, while the next page takes a while */
    int want_app, want_rail;     /* the page the Top Shelf wants next, since want_at */
    Uint32 want_at;
    Pic pic_under, pic_top;      /* the picture on show, and the next one fading in over it */
    float pic_a;
    Tex *pic_held[2];

    /* App Switcher (Recents, like tvOS): the running apps as big cards over the
     * blurred home screen, most recent first */
    struct {
        int open, n, index, loading, failed, close_all; /* close_all: focus is on Close All */
        float t, tv;                 /* 0 closed .. 1 open */
        int zooming;                 /* OK: the card grows to fill the screen while the app opens */
        float zoom, zoomv;
        Uint32 zoom_at, empty_at;    /* empty_at: the last card went; close soon after */
        struct {
            char id[ID_LEN];
            Tex *name;
            int nw, nh;
            float x, xv;             /* offset from the centre, in cards */
            float s, sv;             /* focus */
            float fly, flyv;         /* closing: it flies up and away */
            int flying;
        } c[MAX_RUNNING];
    } recents;

    struct {
        int open, index, n, tw, th;
        int locked;           /* setup screens: Back doesn't close them */
        Tex *body[SHEET_BODY_LINES];
        int bw[SHEET_BODY_LINES], bh[SHEET_BODY_LINES], nbody;
        int first;            /* first list button on screen: long lists scroll */
        int page, parent, parent_index; /* Settings pages: Back opens the parent at that row */
        int wide;             /* label-and-value rows need the room */
        float scroll, scroll_v; /* where the list is drawn from, springing to first */
        float t, tv;          /* morph: 0 = still the button it grew from, 1 = open */
        float fx, fy, fw, fh; /* that button's rect */
        float px, py, pw, ph, pr; /* where the panel was last drawn */
        Tex *title;
        SheetItem it[MAX_SHEET];
    } sheet;

    Tex *clock_tex, *toast_tex;
    int clock_w, clock_h, clock_min, toast_w, toast_h;
    Uint32 toast_until;

    Uint32 ok_down;
    int ok_long;
    int away;           /* we launched another app; reset to the top when we're back */
    int asked;          /* the Control Center over another app asked for one of OPEN_*, for when we're back */
    int service_ok, wake_tries;
    int setup, setup_tries, elevate_tries, home_asked;
    int reduce_motion, reduce_transparency; /* Settings -> Accessibility (tvOS's names) */
    int clock24, orbit_off, vivid_covers;    /* Settings: 24-hour clock, no pixel shift, brand-colour covers */
    int shelf_still;                         /* Settings: the Top Shelf doesn't turn pages by itself */
    int icon_look;                           /* Settings: how app tiles look (LOOK_*) */
    int icon_tint;                           /* ...and the colour Tinted gives them (TINTS) */
    int click_x;                             /* where the pointer last clicked (-1: the keys' OK) */
    int at_page, at_index, at_parent_index;  /* the Settings row an action came from */
    int last_unknown_key;                    /* Settings -> About: what a remote sent that Home+ ignores */
    char svc_version[24];                    /* from /api/status */
    int svc_elevated;
    int ptr_press, ptr_dismiss;             /* the pointer's OK went down on something / outside a sheet */
    /* Which input is in charge. The remote's keys take over from the pointer: then the
     * pointer doesn't move focus, and its click (what OK sends while the cursor shows)
     * presses what the keys focused. Moving the pointer clearly hands it back. */
    int ptr_mode;
    int cursor_up; /* the Magic Remote's pointer is on screen */
    int ptr_ax, ptr_ay; /* where the pointer was when the keys took over, -1: not seen yet */
    Uint32 setup_retry;
    Uint32 next_retry;
    int logged_keys;
    /* OLED protection: idle stages, fades, pixel orbit */
    Uint32 last_input;
    Uint32 drawn_at;                  /* when the last frame was drawn */
    int idle_stage, swallow_up, offscreen, orbit_k; /* offscreen: another app is in front */
    int settle;             /* back from offscreen: the next frame moves every animation to its end */
    int wl_fd;              /* Wayland socket the idle wait sleeps on, -1 = SDL's own wait */
    Uint32 keys_until;      /* a key went down lately, or SAM's bringing Home+ back: keep pumping so SDL makes its
                             * repeats (finishes the relaunch) */
    float dim, black;
    /* motion: the focused element sways briefly and its highlight follows a moving light */
    float sway_t;           /* seconds since focus last moved */
    int focus_key;          /* changes whenever focus moves */
    int tick_armed; /* an arrow key was pressed: a focus change it causes ticks */
    float ptr_x, ptr_y;     /* light direction from the Magic Remote pointer */
    Uint32 ptr_at;          /* when the pointer last moved */
    float light_x, light_y; /* light for focused glass and rims this frame */
    float toast_t, toast_v; /* toast rising in */
    struct {                /* a closed sheet's glass shrinking back into its button */
        int on;
        float t, tv, fx, fy, fw, fh, px, py, pw, ph, pr;
    } ghost;
    Tex *heading_tex, *sub_tex, *sub_first_tex, *sheet_big;
    Tex *sheet_qr; /* sign-in: the service's QR code, scanned with a phone */
    Tex *chevron;  /* above the dock: Up leads into Continue Watching */
    struct { /* the Control Center */
        int open, index, ninputs;    /* index: the focused control (n: the + slot, n + 1: Reset, while editing) */
        float t, tv;
        CCItem it[MAX_CC];
        int n, plus_col, plus_row;   /* the + slot's place (row -1: no room) */
        Tex *glyph[ICON_COUNT], *glyph_big[ICON_COUNT]; /* glyphs, at 64 px, and at 128 for Large */
        float plus_s, plus_v, reset_s, reset_v; /* while editing: the + slot's and Reset's focus */
        struct { char id[24], title[48], group[24], sizes[4]; int available, placed, pinned, icon; } cat[MAX_CAT];
        int ncat;
        struct { char app[ID_LEN], name[120]; int plugged; } inputs[12];
        struct { char id[32], name[48]; } modes[24]; /* the picture modes this input offers now */
        int nmodes, mode, marquee;   /* mode: the one on (-1 unknown); marquee: a long value is scrolling */
        Uint32 focus_at;             /* when the focused control got focus: its long value scrolls from then */
        int ran;                     /* the control whose `run` is on its way (-1 none) */
        int to_network;              /* just opened: the focus goes to Wi-Fi once the list is in */
        /* editing, like iOS 18's: picked is the control being moved (-1 none) */
        int editing, picked;
        Tex *edit_hint, *move_hint, *no_room, *reset;
        int ehw, ehh, mhw, mhh, nrw, nrh, rsw, rsh;
        Tex *problem;                /* what went wrong (no room for one), under the grid until the focus moves or something changes */
        int prw, prh;
        /* a list control (Picture, Inputs) opens its list in the Control Center's place */
        int list, list_kind, list_index, list_n, list_first; /* kind: the one on show (or fading out) */
        int list_only;               /* opened by the Input key on the inputs alone: Back closes them */
        float list_t, list_v;
        Tex *list_title, *list_row[24], *list_val[24];
        /* list 3, the gallery (iOS 18's Add a Control): what this TV has that isn't placed, as round
         * buttons with their names, in groups */
        int gal_cat[MAX_CAT], gal_n, gal_nhead;
        float gal_x[MAX_CAT], gal_y[MAX_CAT], gal_hy[6], gal_h;
        Tex *gal_label[MAX_CAT][2], *gal_head[6]; /* a name too long for one line takes two, as iOS's */
        int gal_lw[MAX_CAT][2], gal_lh[MAX_CAT][2], gal_hw[6], gal_hh[6];
        int ltw, lth, lrw[24], lrh[24], lvw[24], lvh[24];
    } cc;
    float chevron_a;
    int sheet_qr_s, qr_gen;
    int sheet_big_w, sheet_big_h;
    int hw, hh, sw, sh, sfw, sfh;
    float special_s, special_v; /* focus animation of the "Hidden" tile */
} State;

extern State S;

static const char *const SRC_ID[SRC_COUNT] = { "", "stremio", "jellyfin", "plex" };
static const char *const SRC_NAME[SRC_COUNT] = { "", "Stremio", "Jellyfin", "Plex" };

/* what the Control Center over another app can ask Home+ to open */
enum { OPEN_SETTINGS = 1, OPEN_RECENTS, OPEN_ACCESSIBILITY };

/* Tinted's colours on their row in App Tiles: swatch i's centre, the row's right end like a value's */
#define SWATCH_D 34

/* ------------------------------------------------------------------ input */

/* LG's own key numbers: SDL_WEBOS_SCANCODE_* in LG's SDL_webOS.h, which the vendored
 * 2.0.14 headers don't have. Like the arrows they arrive in keysym.sym (Back is sym=482).
 * Settings and Input never showed up in the key log on the C3: webOS opens its own menus. */
enum {
    WEBOS_KEY_BACK = 482,
    WEBOS_KEY_CURSOR_SHOW = 484, /* the Magic Remote pointer woke up / went to sleep: not presses */
    WEBOS_KEY_CURSOR_HIDE = 485,
    WEBOS_KEY_RED = 486,
    WEBOS_KEY_GREEN = 487,
    WEBOS_KEY_YELLOW = 488,
    WEBOS_KEY_BLUE = 489
};

static inline float approach(float *v, float target, float speed, float dt, int *moving)
{
    float d = target - *v;
    if (fabsf(d) < 0.0005f) {
        *v = target;
        return *v;
    }
    *v += d * (1.0f - expf(-speed * dt));
    *moving = 1;
    return *v;
}

/* Spring toward target, parametrised like SwiftUI's: response is the period in
 * seconds, damping 1 settles without overshoot, below 1 bounces a little. */
static inline void spring(float *x, float *v, float target, float response, float damping, float dt, int *moving)
{
    if (S.reduce_motion && damping < 1) damping = 1; /* Reduce Motion: settle without overshoot */
    float k = 2 * (float)M_PI / response, c = 4 * (float)M_PI * damping / response;
    k *= k;
    int n = (int)ceilf(dt * 240);
    if (n < 1) n = 1;
    float h = dt / n;
    for (int i = 0; i < n; i++) {
        *v += (-k * (*x - target) - c * *v) * h;
        *x += *v * h;
    }
    if (fabsf(*x - target) < 0.0005f && fabsf(*v) < 0.005f) {
        *x = target;
        *v = 0;
        return;
    }
    *moving = 1;
}

static inline void tex_free(Tex **t) { r_tex_free(t); }

static inline float smooth01(float a, float b, float x)
{
    float t = (x - a) / (b - a);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

/* main.c */
void request_data(void);
void open_asked(int what);
int update(float dt);

/* util.c */
void log_remote(const char *fmt, ...);
void toast(const char *text, int ms);
int toast_on(void);
void append(char *dst, size_t cap, const char *src);
void copy_id(char *dst, const char *src);
int parse_hex(const char *s, Uint8 out[3]);
const char *jstr(cJSON *o, const char *k);
double jnum(cJSON *o, const char *k);
App *item_app(int item);
int contains_ci(const char *hay, const char *needle);
int media_source(const App *a);
void short_title(const App *a, char *out, size_t cap);
int is_hidden(const char *id);
void app_color(const App *a, Uint8 *r, Uint8 *g, Uint8 *b);
int keep_in_view(int first, int i, int visible, int n, int margin);

/* tiles.c */
void make_logo(App *a, const unsigned char *px, int w, int h);

/* draw.c */
void update_clock(void);
void draw_text(Tex *t, int w, int h, float x, float y, Uint8 a);
void sway(float *dx, float *dy);
void glass(const Glass *g);
void glass_shadow(float x, float y, float w, float h, float radius, float blur, float dy, float alpha);
void panel(int backdrop, float x, float y, float w, float h, float radius, float legible, float focus, float alpha);
void ensure_big(App *a);
void draw_skeleton(float x, float y, float w, float h, float radius, float alpha);
void draw_scrims(Uint8 alpha);
void draw_big_tile(App *a, float x, float y, float w, float h, float radius, Uint8 alpha, int shadow, int backdrop, float on_blur);
void draw_marquee(Tex *t, int tw, int th, float x, float y, float room, Uint8 c, Uint8 a, Uint32 since);
int render(float dt, const char *shot);

/* sheet.c */
void sheet_close(void);
void sheet_dismiss(void);
void sheet_open(const char *title);
void sheet_add(const char *label, Action act, int arg, int destructive);
void sheet_add_app(const char *label, Action act, const char *id, int destructive);
void sheet_body(const char *text);
int sheet_footer(void);
float sheet_head(void);
int sheet_rows(int nfoot);
void draw_sheet_button(const SheetItem *it, int focused, float bx, float y, float bw, float k, Uint8 ca);
void draw_sheet(void);
void draw_ghost(void);

/* recents.c */
void sw_reset(void);
void sw_open(void);
void sw_fill(const char *json);
void sw_ok(void);
void sw_update(float dt, int *anim);
void sw_key(SDL_Keycode k);
void draw_switcher(void);

/* cc.c */
void cc_texts(CCItem *c, const char *title, const char *value);
void parse_control(const char *json);
void parse_system(const char *json);
float cc_grid_x(void);
float cc_grid_y(void);
void cc_pack(void);
int cc_placed(void);
int cc_fit(int i, const char *sizes, int need);
int cc_rect(int i, float *x, float *y, float *w, float *h);
int cc_find(const char *id);
void cc_problem(const char *text);
void cc_post(const char *body);
void cc_open(void);
void cc_close(void);
void cc_save(void);
void cc_move(int dx, int dy);
float cc_dim(void);
void cc_long_press(void);
void cc_add(int cat);
void cc_activate(void);
Tex *cc_glyph(int icon, int big);
void cc_warm(void);
void draw_control_center(void);
void settings_key(void);
void input_key(void);

/* cc_list.c */
void cc_list_texts(void);
void cc_list_open(int which);
void cc_list_key(SDL_Keycode k);
void cc_list_ok(void);
void draw_cc_list(float a);
void draw_cc_gallery(float a);

/* shelf.c */
void pic_hold(Pic *p, Tex **tex);
int pic_rail(int i);
void rail_focus(int i);
App *shelf_app(void);
int want_source(void);
int rail_for(const App *a);
void featured_request(App *a);
void rail_request(int src);
void rail_clear(void);
void parse_continue(const char *json, int src);
void parse_featured(const char *json, App *a);
int rail_active(void);
int carousel_coming(const App *a);
void shelf_load(void);
int shelf_jump(void);
int shelf_complete(void);
void update_page(float dt, int *anim);
void draw_shelf_art(Uint8 alpha, int backdrop);
void draw_shelf_content(Uint8 alpha, int backdrop);

/* apps.c */
void rebuild_items(void);
void save_layout(void);
void parse_layout(const char *json);
int parse_apps(const char *json);
void launch_app(const char *id, const char *params, const char *hint);
void launch(const char *id, const char *hint);
void close_apps(const char *id);
App *app_by_id(const char *id);
void open_app_sheet(void);
void open_hidden_sheet(void);
void move_item_to(int to);
int apps_loading(void);

/* setup.c */
void setup_check(void);
void setup_set(int state);
void set_home_mode(const char *mode, int asked);
void on_status(Result *r);
void on_elevate(Result *r);
void setup_retry_now(void);

/* accounts.c */
void on_media_signin(Result *r);
void on_trakt_signin(Result *r);
int source_installed(int src);
int stremio_installed(void);

/* settings.c */
void settings_page(int page, int index);
void settings_again(void);
float swatch_x(float bx, float bw, int i);
int swatch_at(float mx);
void icon_tint_set(int i);
void open_settings_sheet(void);

/* actions.c */
void run_action(Action act, int arg, const char *id);

/* input.c */
float top_w(void);
float top_x(void);
void enter_top(int col);
int top_down_tile(void);
void focus_rect(float *x, float *y, float *w, float *h);
void move_focus(int dx, int dy);
void go_home(void);
int is_back(SDL_Keycode k);
void keys_take_over(void);
int pointer_moved(int mx, int my);
void on_key_down(SDL_KeyboardEvent *e);
void ok_release(void);
void on_key_up(SDL_KeyboardEvent *e);
int sheet_button_at(int mx, int my, int *outside);
int pointer_at(int mx, int my, int apply);
void to_logical(int *x, int *y);

/* idle.c */
void start_idle_wait(void);
int wait_event(SDL_Event *e, int timeout_ms, int drew);
int next_wake_ms(void);
void ready_home(void);
void go_offscreen(void);
void come_back(void);
int wake(int key);

/* results.c */
int handle_result(Result *r); /* 1 when it may change what's on screen */

/* perf.c */
enum { PROBE_ART, PROBE_SCREEN, PROBE_SCENE, PROBE_BACKDROP, PROBE_OVERLAYS, PROBE_N }; /* a frame's parts */
void perf_check(void);
void perf_key(Uint32 timestamp);
Uint64 perf_clock(void);
void perf_answer(int tag, Uint64 since);
void perf_begin(float dt);
void perf_updated(void);
void perf_probe(int part);
void perf_drawn(void);
void perf_end(void);
void perf_rest(void);
void perf_window(const SDL_Event *e);
void perf_back(void);

#endif
