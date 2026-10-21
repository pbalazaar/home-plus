/* Home+ native — a tvOS-style home screen for webOS, drawn with SDL2 + GLES2.
 *
 * Rendered on the GPU by our own GLES2 renderer (render.c): Liquid Glass panels,
 * rounded shapes and text, a few dozen quads a frame. Animations run at the
 * display refresh rate, and when nothing moves the loop sleeps.
 *
 * Data comes from the Home+ service's local API (see service/service.js). */
#include <ctype.h>
#include <dlfcn.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#include <SDL.h>
/* LG's libSDL2 is the Wayland build; the vendored generic headers only show
 * SDL_SysWMinfo's Wayland members when asked */
#define SDL_VIDEO_DRIVER_WAYLAND 1
#include <SDL_syswm.h>

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
static const struct { const char *name; int icon; } CC_ICONS[] = { /* the service names a control's glyph */
    { "power", ICON_POWER }, { "wifi", ICON_WIFI }, { "ethernet", ICON_ETHERNET }, { "screenOff", ICON_SCREEN_OFF },
    { "sound", ICON_SOUND }, { "timer", ICON_TIMER }, { "input", ICON_INPUT }, { "recents", ICON_RECENTS },
    { "access", ICON_ACCESS }, { "settings", ICON_SETTINGS }, { "picture", ICON_PICTURE }, { "eye", ICON_EYE },
    { "soundMode", ICON_SOUND_MODE }, { "brightness", ICON_BRIGHTNESS }, { "energy", ICON_ENERGY }, { "game", ICON_GAME },
    { "hdr", ICON_HDR }, { "ambient", ICON_AMBIENT }, { "capture", ICON_CAPTURE },
};

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
/* how app icons look (Settings -> Display & OLED -> App Icons): their own colours; dark tiles;
 * glass with white logos; glass in a colour */
enum { LOOK_DEFAULT, LOOK_DARK, LOOK_CLEAR, LOOK_TINTED };
static const char *const LOOK_NAME[] = { "Default", "Dark", "Clear", "Tinted" };
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
       SETUP_FAILED, SETUP_REMOVING, SETUP_DONE };
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

static struct {
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
    int icon_look;                           /* Settings: how app icons look (LOOK_*) */
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
    int wl_fd;              /* Wayland socket the idle wait sleeps on, -1 = SDL's own wait */
    Uint32 keys_until;      /* a key went down lately: keep pumping so SDL makes its repeats */
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
} S;

/* ------------------------------------------------------------------ helpers */

static float approach(float *v, float target, float speed, float dt, int *moving)
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
static void spring(float *x, float *v, float target, float response, float damping, float dt, int *moving)
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

static void tex_free(Tex **t) { r_tex_free(t); }

static void log_remote(const char *fmt, ...)
{
    char msg[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "homeplus: %s\n", msg);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "message", msg);
    char *body = cJSON_PrintUnformatted(o);
    net_post("/api/log", body, 0);
    free(body);
    cJSON_Delete(o);
}

static void toast(const char *text, int ms)
{
    tex_free(&S.toast_tex);
    S.toast_tex = text_render(S.f_medium, 26, text, 1300, &S.toast_w, &S.toast_h);
    S.toast_until = SDL_GetTicks() + ms;
    S.toast_t = S.toast_v = 0;
}

static int toast_on(void) { return S.toast_tex && SDL_GetTicks() < S.toast_until; }

/* appends src to dst (capacity cap), cut short when full */
static void append(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(dst);
    if (n + 1 < cap) snprintf(dst + n, cap - n, "%s", src);
}

static void copy_id(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n >= ID_LEN) n = ID_LEN - 1;
    memmove(dst, src, n);
    dst[n] = 0;
}

static int parse_hex(const char *s, Uint8 out[3])
{
    unsigned v;
    if (!s || s[0] != '#' || strlen(s) != 7 || sscanf(s + 1, "%6x", &v) != 1) return 0;
    out[0] = (v >> 16) & 255;
    out[1] = (v >> 8) & 255;
    out[2] = v & 255;
    return 1;
}

static const char *jstr(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

static double jnum(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static App *item_app(int item)
{
    if (item < 0 || item >= S.nitems || S.items[item] < 0) return NULL;
    return &S.apps[S.items[item]];
}

/* case-insensitive substring test (SDL_strcasestr needs SDL 2.24; the TV has 2.0.14) */
static int contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

static const char *const SRC_ID[SRC_COUNT] = { "", "stremio", "jellyfin", "plex" };
static const char *const SRC_NAME[SRC_COUNT] = { "", "Stremio", "Jellyfin", "Plex" };

/* The media source an app's Top Shelf shows, by its id or name (store builds differ). */
static int media_source(const App *a)
{
    if (!a) return SRC_NONE;
    if (contains_ci(a->id, "stremio") || contains_ci(a->title, "stremio")) return SRC_STREMIO;
    if (contains_ci(a->id, "jellyfin") || contains_ci(a->title, "jellyfin")) return SRC_JELLYFIN;
    if (contains_ci(a->id, "plex") || contains_ci(a->title, "plex") || !strcmp(a->id, "cdp-30")) return SRC_PLEX;
    return SRC_NONE;
}

/* An app's short name: LG's titles can be long ("Spotify - Music and Podcasts"),
 * so labels stop at the first " - " or ": " (tvOS shows short names too). */
static void short_title(const App *a, char *out, size_t cap)
{
    snprintf(out, cap, "%s", a->title);
    static const char *const cuts[] = { " - ", " \xE2\x80\x93 ", " \xE2\x80\x94 ", ": " };
    for (size_t i = 0; i < sizeof cuts / sizeof *cuts; i++) {
        char *c = strstr(out, cuts[i]);
        if (c && c - out >= 3) *c = 0;
    }
}

static int is_hidden(const char *id)
{
    for (int i = 0; i < S.nhidden; i++) if (!strcmp(S.hidden[i], id)) return 1;
    return 0;
}

static void app_color(const App *a, Uint8 *r, Uint8 *g, Uint8 *b)
{
    if (a && (a->has_col || a->fullbleed)) {
        *r = a->col[0]; *g = a->col[1]; *b = a->col[2];
    } else {
        *r = 70; *g = 70; *b = 82;
    }
}

/* ------------------------------------------------------------------ items & layout */

static void rebuild_items(void)
{
    int used[MAX_APPS] = { 0 };
    S.nitems = 0;
    for (int o = 0; o < S.norder; o++) {
        for (int i = 0; i < S.napps; i++) {
            if (!used[i] && !strcmp(S.apps[i].id, S.order[o])) {
                used[i] = 1;
                if (!is_hidden(S.apps[i].id) && !S.apps[i].deleted) S.items[S.nitems++] = i;
            }
        }
    }
    for (int i = 0; i < S.napps; i++) if (!used[i] && !is_hidden(S.apps[i].id) && !S.apps[i].deleted) S.items[S.nitems++] = i;
    int any_hidden = 0;
    for (int i = 0; i < S.napps; i++) if (is_hidden(S.apps[i].id)) any_hidden = 1;
    if (any_hidden) S.items[S.nitems++] = -1;
    if (S.focus >= S.nitems) S.focus = S.nitems > 0 ? S.nitems - 1 : 0;
}

static void save_layout(void)
{
    /* order = what's on screen, then hidden apps (their position doesn't matter) */
    S.norder = 0;
    for (int i = 0; i < S.nitems; i++) if (S.items[i] >= 0) copy_id(S.order[S.norder++], S.apps[S.items[i]].id);
    for (int i = 0; i < S.napps; i++) if (is_hidden(S.apps[i].id)) copy_id(S.order[S.norder++], S.apps[i].id);

    cJSON *o = cJSON_CreateObject(), *ord = cJSON_AddArrayToObject(o, "order"), *hid = cJSON_AddArrayToObject(o, "hidden");
    for (int i = 0; i < S.norder; i++) cJSON_AddItemToArray(ord, cJSON_CreateString(S.order[i]));
    for (int i = 0; i < S.nhidden; i++) cJSON_AddItemToArray(hid, cJSON_CreateString(S.hidden[i]));
    char *body = cJSON_PrintUnformatted(o);
    net_post("/api/layout", body, 0);
    free(body);
    cJSON_Delete(o);
}

static int pic_eq(Pic a, Pic b) { return a.kind == b.kind && (a.kind == PIC_NONE || a.i == b.i); }

/* a layer lets go of its picture (a held one is freed) */
static void pic_drop(Pic *p)
{
    if (p->kind == PIC_HELD) tex_free(&S.pic_held[p->i]);
    *p = (Pic){ PIC_NONE, 0 };
}

/* The owner of layer p's picture is about to free it (*tex): the layer holds it instead,
 * in the slot the other layer isn't using, until something covers it. */
static void pic_hold(Pic *p, Tex **tex)
{
    Pic *o = p == &S.pic_top ? &S.pic_under : &S.pic_top;
    int s = o->kind == PIC_HELD && o->i == 0;
    tex_free(&S.pic_held[s]);
    if (tex && *tex) {
        S.pic_held[s] = *tex;
        *tex = NULL;
        *p = (Pic){ PIC_HELD, s };
    } else {
        *p = (Pic){ PIC_NONE, 0 };
    }
}

/* the carousel title i's picture is on show (or fading in or out) */
static int pic_rail(int i)
{
    return (S.pic_under.kind == PIC_RAIL && S.pic_under.i == i) || (S.pic_top.kind == PIC_RAIL && S.pic_top.i == i);
}

static void parse_layout(const char *json)
{
    cJSON *o = cJSON_Parse(json);
    if (!o) return;
    cJSON *v;
    S.norder = S.nhidden = 0;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(o, "order"))
        if (cJSON_IsString(v) && S.norder < MAX_APPS) snprintf(S.order[S.norder++], ID_LEN, "%s", v->valuestring);
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(o, "hidden"))
        if (cJSON_IsString(v) && S.nhidden < MAX_APPS) snprintf(S.hidden[S.nhidden++], ID_LEN, "%s", v->valuestring);
    cJSON_Delete(o);
    S.layout_loaded = 1;
    if (S.napps) { /* the apps arrived first (the requests race): put them in this order now */
        rebuild_items();
        S.shelf_cur = S.page_app = S.prev_app = -1;
    }
}

static void free_app(App *a)
{
    tex_free(&a->logo_tex);
    tex_free(&a->cover_tex);
    tex_free(&a->icon_tex);
    tex_free(&a->art_tex);
    tex_free(&a->label);
    tex_free(&a->big);
}

static void parse_apps(const char *json)
{
    cJSON *o = cJSON_Parse(json);
    if (!o) return;
    cJSON *arr = cJSON_GetObjectItem(o, "apps"), *v;

    /* skip the rebuild when nothing changed: keeps textures and focus */
    char sig[4096] = "";
    cJSON_ArrayForEach(v, arr) {
        append(sig, sizeof sig, jstr(v, "id"));
        append(sig, sizeof sig, ",");
    }
    if (S.napps && !strcmp(sig, S.apps_sig)) {
        cJSON_Delete(o);
        return;
    }
    snprintf(S.apps_sig, sizeof S.apps_sig, "%s", sig);

    char keep[ID_LEN] = "";
    App *fa = item_app(S.focus);
    if (fa) snprintf(keep, sizeof keep, "%s", fa->id);

    /* the Top Shelf's picture stays until one from the new list covers it */
    for (int l = 0; l < 2; l++) {
        Pic *p = l ? &S.pic_top : &S.pic_under;
        if (p->kind == PIC_ART) pic_hold(p, &S.apps[p->i].art_tex);
        else if (p->kind == PIC_GLOW || p->kind == PIC_COVER) pic_hold(p, &S.apps[p->i].cover_tex);
    }
    S.page_app = S.prev_app = -1;
    for (int i = 0; i < S.napps; i++) free_app(&S.apps[i]);
    memset(S.apps, 0, sizeof S.apps);
    S.napps = 0;
    S.generation++;
    cJSON_ArrayForEach(v, arr) {
        if (S.napps >= MAX_APPS) break;
        App *a = &S.apps[S.napps++];
        snprintf(a->id, sizeof a->id, "%s", jstr(v, "id"));
        snprintf(a->title, sizeof a->title, "%s", jstr(v, "title"));
        snprintf(a->icon, sizeof a->icon, "%s", jstr(v, "icon"));
        snprintf(a->art, sizeof a->art, "%s", jstr(v, "art"));
        a->has_col = parse_hex(jstr(v, "color"), a->col);
        a->removable = cJSON_IsTrue(cJSON_GetObjectItem(v, "removable"));
    }
    cJSON_Delete(o);

    rebuild_items();
    for (int i = 0; i < S.nitems; i++) {
        App *a = item_app(i);
        if (a && keep[0] && !strcmp(a->id, keep)) S.focus = i;
    }
    for (int i = 0; i < S.napps; i++) {
        App *a = &S.apps[i];
        if (a->icon[0]) {
            a->icon_state = 1;
            net_get_image(a->icon, TAG_ICON, i, S.generation, 1, ICON_FIT, ICON_FIT);
        }
    }
    S.shelf_cur = -1;
}

/* the big title and meta line, made only for the card on show (~0.4 MB each) */
static void ensure_rail_texts(RailItem *it)
{
    if (it->texts_made) return; /* (not title_tex: a name that renders to nothing made them every frame) */
    /* "Season 2 · Episode 5 · 31 min left" / "Episode 5 · ..." (anime) / "Movie · 48 min left" */
    char meta[200] = "";
    if (!strcmp(it->type, "series") && it->season)
        snprintf(meta, sizeof meta, "Season %d  \xC2\xB7  Episode %d", it->season, it->episode);
    else if (it->episode)
        snprintf(meta, sizeof meta, "Episode %d", it->episode);
    else if (!strcmp(it->type, "movie"))
        snprintf(meta, sizeof meta, "Movie");
    else if (!strcmp(it->type, "series"))
        snprintf(meta, sizeof meta, "Series");
    if (it->year) { /* popular titles: "Series · 2026" */
        char yr[24];
        snprintf(yr, sizeof yr, "%s%d", meta[0] ? "  \xC2\xB7  " : "", it->year);
        append(meta, sizeof meta, yr);
    }
    if (it->remaining) {
        char left[48];
        snprintf(left, sizeof left, "%s%d min left", meta[0] ? "  \xC2\xB7  " : "", it->remaining);
        append(meta, sizeof meta, left);
    }
    tex_free(&it->title_tex);
    tex_free(&it->meta_tex);
    it->title_tex = text_render(S.f_bold, 66, it->name, 1100, &it->tw, &it->th);
    it->meta_tex = text_render(S.f_medium, 27, meta, 1100, &it->mw, &it->mh);
    it->texts_made = 1;
}

/* The first of n items to show, `visible` at a time, so that item i stays in view
 * with `margin` items around it: the list only scrolls when i would get closer
 * to an edge than that (or leave it). */
static int keep_in_view(int first, int i, int visible, int n, int margin)
{
    if (first > i - margin) first = i - margin;
    if (first < i + margin - (visible - 1)) first = i + margin - (visible - 1);
    if (first > n - visible) first = n - visible;
    return first < 0 ? 0 : first;
}

/* Shows title i in the Top Shelf carousel. */
static void rail_focus(int i)
{
    S.rail_index = S.nrail <= 0 || i < 0 ? 0 : i >= S.nrail ? S.nrail - 1 : i;
}

/* The app whose Top Shelf is on show: the focused top-row app, or below the dock
 * the last one focused (its picture stays behind the grid). */
static App *shelf_app(void) { return item_app(S.focus < COLS ? S.focus : S.shelf_cur); }

/* Whose carousel the Top Shelf wants: a media app's Continue Watching, or a
 * streaming app's popular titles. */
static int want_source(void)
{
    App *a = shelf_app();
    int src = media_source(a);
    return src ? src : a && a->feat == 2 ? SRC_FEATURED : SRC_NONE;
}

/* the carousel holds app a's titles */
static int rail_for(const App *a)
{
    if (!a || S.nrail <= 0) return 0;
    int src = media_source(a);
    if (src) return src == S.rail_source;
    return S.rail_source == SRC_FEATURED && !strcmp(S.rail_app, a->id);
}

/* asks the service whether app a is a streaming service, and what's popular on it */
static void featured_request(App *a)
{
    char path[ID_LEN + 32];
    snprintf(path, sizeof path, "/api/featured?app=%s", a->id);
    if (!a->feat) a->feat = 1;
    S.feat_req = (int)(a - S.apps);
    net_get_json(path, TAG_FEATURED, S.feat_req, S.generation);
}

static void rail_request(int src)
{
    char path[64];
    snprintf(path, sizeof path, "/api/continue?source=%s", SRC_ID[src]);
    S.rail_req = src;
    net_get_json(path, TAG_STREMIO, src, 0);
}

/* Puts titles (a JSON array) in the Top Shelf carousel: source src's, and for
 * popular titles app's, under a heading. */
static void rail_fill(cJSON *arr, int src, const char *app, const char *heading, const char *opens)
{
    if (src != S.rail_source || strcmp(app, S.rail_app)) { /* another carousel: its heading, from its first title */
        char hint[160];
        tex_free(&S.heading_tex);
        S.heading_tex = text_render(S.f_semi, HEADING_PX, heading, 0, &S.hw, &S.hh); /* as big as the clock's */
        snprintf(hint, sizeof hint, "OK opens %s", opens);
        tex_free(&S.rail_hint);
        S.rail_hint = text_render(S.f_medium, HINT_PX, hint, 0, &S.rail_hint_w, &S.rail_hint_h);
        snprintf(hint, sizeof hint, "Left and right for more  \xC2\xB7  OK opens %s", opens); /* while you step through them */
        tex_free(&S.rail_browse);
        S.rail_browse = text_render(S.f_medium, HINT_PX, hint, 0, &S.rail_browse_w, &S.rail_browse_h);
        S.rail_source = src;
        snprintf(S.rail_app, sizeof S.rail_app, "%s", app);
        S.rail_sig[0] = 0;
        S.rail_index = 0;
        S.rail_next = 0;
    }
    cJSON *v;
    char sig[2048] = "";
    cJSON_ArrayForEach(v, arr) {
        char part[96];
        snprintf(part, sizeof part, "%s:%.3f,", jstr(v, "id"), jnum(v, "progress"));
        append(sig, sizeof sig, part);
    }
    if (!strcmp(sig, S.rail_sig) && S.nrail) return;
    int same = S.rail_sig[0] != 0; /* a refresh of the same carousel: stay on the same title */
    snprintf(S.rail_sig, sizeof S.rail_sig, "%s", sig);

    static RailItem old[MAX_RAIL]; /* its pictures and texts move over to the titles that stay */
    int nold = S.nrail;
    char keep[64] = "";
    if (same && S.rail_index < nold) snprintf(keep, sizeof keep, "%s", S.rail[S.rail_index].id);
    memcpy(old, S.rail, sizeof old);
    memset(S.rail, 0, sizeof S.rail);
    S.nrail = 0;
    net_drop(TAG_RAIL_BG, -1, S.rail_generation); /* pictures not yet on the way are for the old order */
    net_drop(TAG_RAIL_LOGO, -1, S.rail_generation);
    S.rail_generation++; /* and the ones on the way are ignored */
    cJSON_ArrayForEach(v, arr) {
        if (S.nrail >= MAX_RAIL) break;
        RailItem *it = &S.rail[S.nrail++];
        snprintf(it->id, sizeof it->id, "%s", jstr(v, "id"));
        snprintf(it->type, sizeof it->type, "%s", jstr(v, "type"));
        snprintf(it->name, sizeof it->name, "%s", jstr(v, "name"));
        const char *bg = jstr(v, "background");
        snprintf(it->bg, sizeof it->bg, "%s", bg[0] ? bg : jstr(v, "thumbnail"));
        snprintf(it->logo, sizeof it->logo, "%s", jstr(v, "logo"));
        it->progress = (float)jnum(v, "progress");
        it->season = (int)jnum(v, "season");
        it->episode = (int)jnum(v, "episode");
        it->remaining = (int)jnum(v, "remainingMin");
        it->year = (int)jnum(v, "year");
    }
    int at = S.rail_index, moved[MAX_RAIL], pic_to[MAX_RAIL]; /* where each old title went, and its picture */
    for (int k = 0; k < nold; k++) moved[k] = pic_to[k] = -1;
    for (int i = 0; i < S.nrail; i++) {
        RailItem *it = &S.rail[i];
        for (int k = 0; k < nold; k++) {
            RailItem *o = &old[k];
            if (strcmp(o->id, it->id)) continue;
            moved[k] = i;
            if (strcmp(o->bg, it->bg) || strcmp(o->logo, it->logo)) break;
            if (o->bg_state == 2) it->bg_tex = o->bg_tex, it->bg_state = 2, o->bg_tex = NULL, pic_to[k] = i;
            if (o->logo_state == 2) it->logo_tex = o->logo_tex, it->logo_state = 2, it->lw = o->lw, it->lh = o->lh, o->logo_tex = NULL;
            if (it->progress == o->progress && it->remaining == o->remaining) {
                it->title_tex = o->title_tex, it->tw = o->tw, it->th = o->th, o->title_tex = NULL;
                it->meta_tex = o->meta_tex, it->mw = o->mw, it->mh = o->mh, o->meta_tex = NULL;
                it->texts_made = o->texts_made;
            }
            break;
        }
        if (keep[0] && !strcmp(it->id, keep)) at = i;
    }
    /* what's on show stays: the pages follow their titles (-2: it left), and a picture
     * whose title left is held until something covers it */
    for (int l = 0; l < 2; l++) {
        Pic *p = l ? &S.pic_top : &S.pic_under;
        if (p->kind != PIC_RAIL) continue;
        if (p->i < nold && pic_to[p->i] >= 0) p->i = pic_to[p->i];
        else pic_hold(p, p->i < nold ? &old[p->i].bg_tex : NULL);
    }
    if (S.page_rail >= 0) S.page_rail = S.page_rail < nold && moved[S.page_rail] >= 0 ? moved[S.page_rail] : -2;
    if (S.prev_rail >= 0) S.prev_rail = S.prev_rail < nold && moved[S.prev_rail] >= 0 ? moved[S.prev_rail] : -2;
    for (int k = 0; k < nold; k++) {
        RailItem *o = &old[k];
        tex_free(&o->bg_tex); tex_free(&o->logo_tex); tex_free(&o->title_tex); tex_free(&o->meta_tex);
    }
    rail_focus(at < S.nrail ? at : S.nrail - 1);
    if (!S.nrail && S.zone == ZONE_RAIL) S.zone = ZONE_GRID;
}

static void parse_continue(const char *json, int src)
{
    cJSON *o = cJSON_Parse(json);
    if (src == S.rail_req) S.rail_req = SRC_NONE;
    if (!o) return;
    int connected = cJSON_IsTrue(cJSON_GetObjectItem(o, "connected"));
    S.src_connected[src] = connected;
    if (src == SRC_STREMIO) { /* this answer also carries every account's state, for Settings */
        S.rail_connected = connected;
        S.rail_stremio = cJSON_IsTrue(cJSON_GetObjectItem(o, "stremio"));
        cJSON *tr = cJSON_GetObjectItem(o, "trakt");
        S.trakt_configured = cJSON_IsTrue(cJSON_GetObjectItem(tr, "configured"));
        S.trakt_connected = cJSON_IsTrue(cJSON_GetObjectItem(tr, "connected"));
        S.src_connected[SRC_JELLYFIN] = cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(o, "jellyfin"), "connected"));
        S.src_connected[SRC_PLEX] = cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(o, "plex"), "connected"));
        int tmdb = cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(o, "tmdb"), "configured"));
        if (tmdb && !S.tmdb_configured) /* just set up: ask again about apps that had nothing */
            for (int i = 0; i < S.napps; i++) if (S.apps[i].feat == 3) S.apps[i].feat = 0;
        S.tmdb_configured = tmdb;
    }
    if (src != want_source()) { /* another app's carousel: not on show */
        cJSON_Delete(o);
        return;
    }
    char heading[96];
    if (src == SRC_STREMIO)
        snprintf(heading, sizeof heading, "Continue Watching%s%s", S.rail_stremio ? " \xC2\xB7 Stremio" : "",
                 S.trakt_connected ? " \xC2\xB7 Trakt" : "");
    else
        snprintf(heading, sizeof heading, "Continue Watching \xC2\xB7 %s", SRC_NAME[src]);
    rail_fill(cJSON_GetObjectItem(o, "items"), src, "", heading, SRC_NAME[src]);
    cJSON_Delete(o);
}

static void cc_pack(void);
static void cc_list_texts(void);
static int cc_find(const char *id);

/* a control's name and value (whole: a long one scrolls while it has focus) */
static void cc_texts(CCItem *c, const char *title, const char *value)
{
    if (!c->title || strcmp(c->title_s, title)) {
        snprintf(c->title_s, sizeof c->title_s, "%s", title);
        tex_free(&c->title);
        c->title = text_render(S.f_semi, c->units == 4 ? 24 : CC_TITLE_PX, title, 0, &c->tw, &c->th);
    }
    if (strcmp(c->value_s, value)) {
        snprintf(c->value_s, sizeof c->value_s, "%s", value);
        tex_free(&c->value);
        if (value[0]) c->value = text_render(S.f_medium, 18, value, 0, &c->vw, &c->vh);
    }
}

static int cc_icon(const char *name)
{
    for (size_t k = 0; k < sizeof CC_ICONS / sizeof CC_ICONS[0]; k++)
        if (!strcmp(CC_ICONS[k].name, name)) return CC_ICONS[k].icon;
    return ICON_CONTROLS; /* one the launcher doesn't know: the Control Center's own */
}

/* the Control Center's state, from the service (also its answer to every action): the
 * controls on show, the catalogue, the picture modes and the inputs */
static void parse_control(const char *json)
{
    cJSON *o = cJSON_Parse(json), *v;
    if (!o) return;
    if (S.cc.picked < 0) { /* (while one is being moved, the list on screen is the one that counts) */
        static CCItem old[MAX_CC];
        int nold = S.cc.n;
        memcpy(old, S.cc.it, sizeof old);
        memset(S.cc.it, 0, sizeof S.cc.it);
        S.cc.n = 0;
        cJSON_ArrayForEach(v, cJSON_GetObjectItem(o, "controls")) {
            if (S.cc.n >= MAX_CC) break;
            CCItem *c = &S.cc.it[S.cc.n++];
            snprintf(c->id, sizeof c->id, "%s", jstr(v, "id"));
            snprintf(c->kind, sizeof c->kind, "%s", jstr(v, "kind"));
            snprintf(c->name_s, sizeof c->name_s, "%s", jstr(v, "name"));
            const char *size = jstr(v, "size");
            c->units = !strcmp(size, "large") ? 4 : !strcmp(size, "wide") ? 2 : 1;
            c->icon = cc_icon(jstr(v, "icon"));
            cJSON *on = cJSON_GetObjectItem(v, "on"); /* true, false, or none: tvOS shows which in the glyph */
            c->on = cJSON_IsTrue(on);
            c->stateful = cJSON_IsBool(on);
            c->available = cJSON_IsTrue(cJSON_GetObjectItem(v, "available"));
            for (int k = 0; k < nold; k++) { /* the same control: its texts and motion stay */
                CCItem *p = &old[k];
                if (strcmp(p->id, c->id) || p->units != c->units) continue;
                memcpy(c->title_s, p->title_s, sizeof c->title_s);
                memcpy(c->value_s, p->value_s, sizeof c->value_s);
                c->title = p->title, c->tw = p->tw, c->th = p->th, p->title = NULL;
                c->value = p->value, c->vw = p->vw, c->vh = p->vh, p->value = NULL;
                c->s = p->s, c->sv = p->sv, c->flash = p->flash, c->shake = p->shake;
                break;
            }
            cc_texts(c, jstr(v, "title"), jstr(v, "value"));
        }
        for (int k = 0; k < nold; k++) {
            tex_free(&old[k].title);
            tex_free(&old[k].value);
        }
        cc_pack();
        int last = S.cc.n + (S.cc.editing ? 2 : 0) - 1, net = cc_find("network");
        if (S.cc.to_network && net >= 0) S.cc.index = net;
        S.cc.to_network = 0;
        if (S.cc.index > last) S.cc.index = last < 0 ? 0 : last;
    }
    S.cc.ncat = 0;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(o, "catalog")) {
        if (S.cc.ncat >= MAX_CAT) break;
        __typeof__(S.cc.cat[0]) *e = &S.cc.cat[S.cc.ncat++];
        snprintf(e->id, sizeof e->id, "%s", jstr(v, "id"));
        snprintf(e->title, sizeof e->title, "%s", jstr(v, "title"));
        snprintf(e->group, sizeof e->group, "%s", jstr(v, "group"));
        int ns = 0;
        cJSON *sz;
        cJSON_ArrayForEach(sz, cJSON_GetObjectItem(v, "sizes")) {
            const char *t = cJSON_IsString(sz) ? sz->valuestring : "";
            if (ns < 3) e->sizes[ns++] = !strcmp(t, "large") ? 4 : !strcmp(t, "wide") ? 2 : 1;
        }
        e->sizes[ns] = 0;
        e->available = cJSON_IsTrue(cJSON_GetObjectItem(v, "available"));
        e->placed = cJSON_IsTrue(cJSON_GetObjectItem(v, "placed"));
        e->pinned = cJSON_IsTrue(cJSON_GetObjectItem(v, "pinned"));
        e->icon = cc_icon(jstr(v, "icon"));
    }
    cJSON *pic = cJSON_GetObjectItem(o, "picture");
    const char *mode = jstr(pic, "mode");
    S.cc.nmodes = 0;
    S.cc.mode = -1;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(pic, "modes")) {
        if (S.cc.nmodes >= (int)(sizeof S.cc.modes / sizeof S.cc.modes[0])) break;
        snprintf(S.cc.modes[S.cc.nmodes].id, sizeof S.cc.modes[0].id, "%s", jstr(v, "id"));
        snprintf(S.cc.modes[S.cc.nmodes].name, sizeof S.cc.modes[0].name, "%s", jstr(v, "label"));
        if (!strcmp(S.cc.modes[S.cc.nmodes].id, mode)) S.cc.mode = S.cc.nmodes;
        S.cc.nmodes++;
    }
    S.cc.ninputs = 0;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(o, "inputs")) {
        if (S.cc.ninputs >= 12) break;
        __typeof__(S.cc.inputs[0]) *in = &S.cc.inputs[S.cc.ninputs++];
        copy_id(in->app, jstr(v, "appId"));
        const char *dev = jstr(v, "device");
        if (dev[0]) snprintf(in->name, sizeof in->name, "%s \xC2\xB7 %s", jstr(v, "label"), dev); /* "Soundbar · PS5" */
        else snprintf(in->name, sizeof in->name, "%s", jstr(v, "label"));
        in->plugged = cJSON_IsTrue(cJSON_GetObjectItem(v, "plugged"));
    }
    if (S.cc.list == 1 || S.cc.list == 2) cc_list_texts(); /* the tick follows the picture mode; the inputs come in */
    cJSON_Delete(o);
}

/* An app's titles from TMDB, for its Top Shelf: what's popular on the streaming
 * service it is, or what's trending (an app with no picture); none: neither. */
static void parse_featured(const char *json, App *a)
{
    cJSON *o = cJSON_Parse(json);
    cJSON *arr = o ? cJSON_GetObjectItem(o, "items") : NULL;
    int n = cJSON_GetArraySize(arr), trending = o && !strcmp(jstr(o, "kind"), "trending");
    a->feat = n > 0 ? 2 : 3;
    if (n > 0 && a == shelf_app()) {
        char heading[ID_LEN + 16], name[ID_LEN];
        short_title(a, name, sizeof name);
        if (trending) snprintf(heading, sizeof heading, "Trending Now");
        else snprintf(heading, sizeof heading, "Popular on %s", name);
        rail_fill(arr, SRC_FEATURED, a->id, heading, name);
        S.rail_trending = trending;
    }
    cJSON_Delete(o);
}

static void parse_system(const char *json)
{
    cJSON *o = cJSON_Parse(json), *v;
    if (!o) return;
    S.nsettings = 0;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(o, "settings")) {
        if (S.nsettings >= MAX_SYS) break;
        copy_id(S.settings[S.nsettings].id, jstr(v, "id"));
        snprintf(S.settings[S.nsettings].label, sizeof S.settings[0].label, "%s", jstr(v, "label"));
        S.nsettings++;
    }
    cJSON_Delete(o);
}

static void request_data(void)
{
    if (!S.layout_loaded) net_get_json("/api/layout", TAG_LAYOUT, 0, 0);
    net_get_json("/api/apps", TAG_APPS, 0, 0);
    rail_request(SRC_STREMIO);
    if (want_source() > SRC_STREMIO) rail_request(want_source());
    net_get_json("/api/system", TAG_SYSTEM, 0, 0);
    net_get_json("/api/home", TAG_HOME, 0, 0);
    net_events(TAG_EVENTS); /* the Settings key, through the service (once) */
    net_get_json("/api/prefs", TAG_PREFS, 0, 0);
}

static void sheet_open(const char *title);
static void on_status(Result *r);
static void sheet_body(const char *text);
static void settings_page(int page, int index);
static void sw_open(void);
static void open_settings_sheet(void);

/* what the Control Center over another app can ask Home+ to open */
enum { OPEN_SETTINGS = 1, OPEN_RECENTS, OPEN_ACCESSIBILITY };
static void open_asked(int what)
{
    if (what == OPEN_SETTINGS) open_settings_sheet();
    else if (what == OPEN_RECENTS) sw_open();
    else if (what == OPEN_ACCESSIBILITY) settings_page(PAGE_ACCESS, 0);
}
static void on_elevate(Result *r);
static void sw_fill(const char *json);
static void sw_reset(void);
static void settings_key(void);
static void input_key(void);
static void cc_save(void);
static void cc_add(int cat);
static void cc_post(const char *body);
static void cc_problem(const char *text);
static int cc_placed(void);
static int cc_fit(int i, const char *sizes, int need);
static void sheet_close(void);
static void sheet_dismiss(void);
static void sheet_add(const char *label, Action act, int arg, int destructive);
static int sheet_footer(void);
static int sheet_rows(int nfoot);
static float sheet_head(void);

/* ------------------------------------------------------------------ uniform tiles */

/* Like tvOS app icons, every tile is the icon's own background colour from edge
 * to edge with its logo centred at the same visual size. LG icons are small
 * squares with their own background, so: find that colour on the border, key
 * it out, trim to the logo and size it to a common target. Icons without a
 * solid background keep their whole picture, at one size, with round corners;
 * logo-only icons (transparent) get one graphite tile (light if the logo is dark). */
static float smooth01(float a, float b, float x)
{
    float t = (x - a) / (b - a);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

/* A cover for apps without artwork: the icon stretched over a small 16:9
 * frame (over the app's colour where it's transparent), blurred into soft
 * colour fields, darkened towards the edges. Drawn full screen, the GPU's
 * filtering smooths it further. */
static void make_cover(App *a, const unsigned char *px, int w, int h)
{
    enum { CW = 160, CH = 90, PASSES = 3, R = 7 };
    float *f = malloc(sizeof(float) * CW * CH * 3), *t = malloc(sizeof(float) * CW * CH * 3);
    unsigned char *out = malloc((size_t)CW * CH * 4);
    if (!f || !t || !out) {
        free(f);
        free(t);
        free(out);
        return;
    }
    float sc = fmaxf((float)CW / w, (float)CH / h);
    for (int y = 0; y < CH; y++) {
        for (int x = 0; x < CW; x++) {
            int sx = (int)((x + 0.5f - CW / 2.0f) / sc + w / 2.0f), sy = (int)((y + 0.5f - CH / 2.0f) / sc + h / 2.0f);
            sx = sx < 0 ? 0 : sx >= w ? w - 1 : sx;
            sy = sy < 0 ? 0 : sy >= h ? h - 1 : sy;
            const unsigned char *p = px + ((size_t)sy * w + sx) * 4;
            float al = p[3] / 255.0f;
            for (int c = 0; c < 3; c++) f[(y * CW + x) * 3 + c] = p[c] * al + a->col[c] * (1 - al);
        }
    }
    for (int pass = 0; pass < PASSES; pass++) { /* box blur, horizontal then vertical */
        for (int y = 0; y < CH; y++)
            for (int x = 0; x < CW; x++)
                for (int c = 0; c < 3; c++) {
                    float sum = 0;
                    for (int k = -R; k <= R; k++) {
                        int xx = x + k < 0 ? 0 : x + k >= CW ? CW - 1 : x + k;
                        sum += f[(y * CW + xx) * 3 + c];
                    }
                    t[(y * CW + x) * 3 + c] = sum / (2 * R + 1);
                }
        for (int y = 0; y < CH; y++)
            for (int x = 0; x < CW; x++)
                for (int c = 0; c < 3; c++) {
                    float sum = 0;
                    for (int k = -R; k <= R; k++) {
                        int yy = y + k < 0 ? 0 : y + k >= CH ? CH - 1 : y + k;
                        sum += t[(yy * CW + x) * 3 + c];
                    }
                    f[(y * CW + x) * 3 + c] = sum / (2 * R + 1);
                }
    }
    for (int y = 0; y < CH; y++) {
        for (int x = 0; x < CW; x++) {
            float dx = (x - CW / 2.0f) / (CW / 2.0f), dy = (y - CH / 2.0f) / (CH / 2.0f);
            /* calm: halfway to grey and dark, so a focused app doesn't flood the room
             * (HyperHDR mirrors the screen into the lights); darker towards the edges */
            float shade = (S.vivid_covers ? 0.82f : 0.45f) * (1 - 0.35f * fminf(1, (dx * dx + dy * dy) * 0.6f));
            const float *pc = &f[(y * CW + x) * 3];
            float luma = 0.299f * pc[0] + 0.587f * pc[1] + 0.114f * pc[2];
            unsigned char *o = out + ((size_t)y * CW + x) * 4;
            for (int c = 0; c < 3; c++) {
                float v = (pc[c] + (luma - pc[c]) * (S.vivid_covers ? 0 : 0.55f)) * shade;
                o[c] = (unsigned char)(v > 255 ? 255 : v);
            }
            o[3] = 255;
        }
    }
    tex_free(&a->cover_tex);
    a->cover_tex = r_tex(out, CW, CH);
    free(f);
    free(t);
    free(out);
}

/* the most (or least) of v within 2 px, across then down: out (tmp is scratch) */
static void spread(const unsigned short *v, unsigned short *tmp, unsigned short *out, int w, int h, int most)
{
    for (int pass = 0; pass < 2; pass++) {
        const unsigned short *from = pass ? tmp : v;
        unsigned short *to = pass ? out : tmp;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int at = pass ? y : x, len = pass ? h : w, step = pass ? w : 1;
                unsigned short r = from[y * w + x];
                for (int d = at > 2 ? -2 : -at; d <= 2 && at + d < len; d++) {
                    unsigned short u = from[y * w + x + d * step];
                    r = most ? (u > r ? u : r) : (u < r ? u : r);
                }
                to[y * w + x] = r;
            }
    }
}

static void make_logo(App *a, const unsigned char *px, int w, int h)
{
    /* background: the first opaque pixel (2 px in, past the anti-aliased edge)
     * on 24 lines inward from each side */
    enum { LINES = 24 };
    unsigned char samp[4 * LINES][3];
    int n = 0;
    double m[3] = { 0 };
    for (int side = 0; side < 4; side++) {
        for (int k = 1; k <= LINES; k++) {
            int along = side < 2 ? w : h, depth = side < 2 ? h : w, t = k * along / (LINES + 1);
            for (int d = 0; d < depth / 3; d++) {
                int dd = d;
                int x = side == 0 || side == 1 ? t : side == 2 ? dd : w - 1 - dd;
                int y = side == 0 ? dd : side == 1 ? h - 1 - dd : t;
                if (px[((size_t)y * w + x) * 4 + 3] <= 200) continue;
                dd = d + 2 < depth ? d + 2 : d;
                x = side == 0 || side == 1 ? t : side == 2 ? dd : w - 1 - dd;
                y = side == 0 ? dd : side == 1 ? h - 1 - dd : t;
                const unsigned char *p = px + ((size_t)y * w + x) * 4;
                memcpy(samp[n++], p, 3);
                m[0] += p[0];
                m[1] += p[1];
                m[2] += p[2];
                break;
            }
        }
    }
    enum { SOLID, PICTURE, CLEAR } mode = CLEAR;
    if (n >= 4 * LINES * 3 / 4) {
        double dev = 0;
        for (int c = 0; c < 3; c++) m[c] /= n;
        for (int i = 0; i < n; i++) dev += fabs(samp[i][0] - m[0]) + fabs(samp[i][1] - m[1]) + fabs(samp[i][2] - m[2]);
        mode = dev / n < 24 ? SOLID : PICTURE;
    }

    /* keyed copy: background pixels fade to transparent; then the logo's bounding box */
    unsigned char *k = malloc((size_t)w * h * 4);
    if (!k) return;
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    double lum = 0, lum_n = 0;
    /* for the looks: how far the logo's ink stands out from the icon's background (a logo with
     * none: its own luminance), and how much of it is grey or white */
    float ref = mode == CLEAR ? -1 : (float)((0.299 * m[0] + 0.587 * m[1] + 0.114 * m[2]) / 255);
    int stand[32] = { 0 }, grey = 0, far = 0;
    /* A logo's edge pixels are part background. How far one is from the background, against how far
     * the logo's ink is nearby (the most within 2 px), says how much of it is logo; taking the
     * background's part out of its colour leaves the ink's. On the icon's own colour the tile looks
     * the same; on a look's tile no rim of the old background shows round the logo. */
    unsigned short *dist = mode == SOLID ? malloc(sizeof *dist * (size_t)w * h * 4) : NULL, *ink_near = NULL, *bg_near = NULL;
    unsigned char *key = dist ? malloc((size_t)w * h) : NULL; /* the plain key: the logo's box and size, as before */
    if (dist) {
        unsigned short *tmp = dist + (size_t)w * h;
        ink_near = tmp + (size_t)w * h;
        bg_near = ink_near + (size_t)w * h;
        for (int i = 0; i < w * h; i++) {
            const unsigned char *p = px + (size_t)i * 4;
            dist[i] = (unsigned short)(fabs(p[0] - m[0]) + fabs(p[1] - m[1]) + fabs(p[2] - m[2]));
        }
        spread(dist, tmp, ink_near, w, h, 1);
        spread(dist, tmp, bg_near, w, h, 0); /* only where the background is near: inside, two inks meet */
    }
    for (int i = 0; i < w * h; i++) {
        const unsigned char *p = px + (size_t)i * 4;
        unsigned char *q = k + (size_t)i * 4;
        float alpha = p[3], d = 0, plain;
        memcpy(q, p, 3);
        if (dist && key) {
            float n = ink_near[i], logo = bg_near[i] >= 24 ? 1 : n > 24 ? fminf(1, fmaxf(0, (dist[i] - 12) / (n - 12))) : 0;
            d = dist[i];
            key[i] = (unsigned char)(alpha * smooth01(24, 72, d));
            alpha *= logo * smooth01(24, 72, n);
            if (logo > 0.02f)
                for (int c = 0; c < 3; c++) q[c] = (unsigned char)fmin(255, fmax(0, m[c] + (p[c] - m[c]) / logo + 0.5));
        } else if (mode == SOLID) { /* (no memory for that: a plain key) */
            d = fabsf(p[0] - (float)m[0]) + fabsf(p[1] - (float)m[1]) + fabsf(p[2] - (float)m[2]);
            alpha *= smooth01(24, 72, d);
        }
        q[3] = (unsigned char)alpha;
        plain = key ? key[i] : alpha;
        if (mode == PICTURE ? p[3] > 40 : plain > 40) {
            int x = i % w, y = i / w;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
            float l = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / 255, lq = (0.299f * q[0] + 0.587f * q[1] + 0.114f * q[2]) / 255;
            lum += l;
            lum_n++;
            stand[(int)((ref < 0 ? lq : fabsf(lq - ref)) * 31.99f)]++;
            if (d > 200) { /* the ink itself, not its edge blending into the background */
                int hi = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]);
                int lo = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]);
                far++;
                if (hi - lo < 40) grey++;
            }
        }
    }
    free(dist);
    /* the most it stands out: past all but its 5% that stand out most (a few stray pixels don't count) */
    a->ink_ref = ref;
    a->ink_span = 1;
    for (int b = 31, acc = 0; b >= 0; b--)
        if ((acc += stand[b]) > lum_n / 20) {
            a->ink_span = (b + 1) / 32.0f;
            break;
        }
    int hi = (int)fmax(m[0], fmax(m[1], m[2])), lo = (int)fmin(m[0], fmin(m[1], m[2]));
    a->ink_tint = mode == SOLID && hi - lo > 60 && far && grey > far * 0.85;
    if (x1 < x0) { /* nothing but background: keep the whole icon */
        x0 = y0 = 0;
        x1 = w - 1;
        y1 = h - 1;
        mode = PICTURE;
        a->ink_ref = -1;
        a->ink_span = 1;
    }
    if (mode == PICTURE) {
        memcpy(k, px, (size_t)w * h * 4);
        free(key), key = NULL;
    }
    double ink = 0; /* how much logo there is: sizes logos by what you see, not their box */
    x0 = x0 > 2 ? x0 - 2 : 0;
    y0 = y0 > 2 ? y0 - 2 : 0;
    x1 = x1 < w - 3 ? x1 + 2 : w - 1;
    y1 = y1 < h - 3 ? y1 + 2 : h - 1;
    int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    unsigned char *crop = malloc((size_t)cw * ch * 4);
    if (!crop) {
        free(k);
        free(key);
        return;
    }
    for (int y = 0; y < ch; y++) memcpy(crop + (size_t)y * cw * 4, k + ((size_t)(y0 + y) * w + x0) * 4, (size_t)cw * 4);
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++) ink += (key ? key[(size_t)(y0 + y) * w + x0 + x] : crop[((size_t)y * cw + x) * 4 + 3]) / 255.0;
    free(key);
    free(k);
    tex_free(&a->logo_tex);
    a->logo_tex = r_tex(crop, cw, ch);
    free(crop);

    /* one visual size: about the same area for every logo, within a box; as
     * big as tvOS's glyphs, which fill about 60% of a tile's height */
    if (mode == PICTURE) {
        float side = TILE_H * 0.66f;
        a->logo_w = a->logo_h = side;
        a->logo_r = TILE_R * side / TILE_H; /* same corner-to-size ratio as the tile */
    } else {
        /* the same amount of logo on every tile (a square of about 40% of the
         * tile's height, filled), but never wider or taller than the tile allows */
        float sc = TILE_H * 0.42f / sqrtf((float)(ink > 1 ? ink : 1));
        sc = fminf(sc, fminf(TILE_W * 0.72f / cw, TILE_H * 0.62f / ch));
        sc = fminf(sc, 1.5f); /* LG ships 130 px icons: more than this goes soft */
        a->logo_w = cw * sc;
        a->logo_h = ch * sc;
        a->logo_r = 0;
    }
    /* the tile: the icon's background, a darkened average behind pictures, graphite behind bare logos */
    if (mode == SOLID) {
        for (int c = 0; c < 3; c++) a->col[c] = (Uint8)(m[c] + 0.5);
    } else if (mode == PICTURE) {
        for (int c = 0; c < 3; c++) a->col[c] = (Uint8)(n ? m[c] * 0.55 : 40);
    } else {
        int light = lum_n > 0 && lum / lum_n < 0.25;
        a->col[0] = a->col[1] = light ? 236 : 40;
        a->col[2] = light ? 240 : 46;
    }
    a->has_col = 1;
    a->fullbleed = mode == SOLID;
    make_cover(a, px, w, h);
}

/* ------------------------------------------------------------------ results */

static void handle_result(Result *r)
{
    switch (r->tag) {
    case TAG_WAKE:
        break;
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
        break; /* the service exits; setup_retry wakes it and checks again */
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
        if (r->status == 200) {
            S.service_ok = 1;
            parse_apps(r->body);
        } else {
            S.service_ok = 0;
            /* service not up yet: start it, then retry */
            if (S.wake_tries++ < 10) {
                net_wake_service();
                S.next_retry = SDL_GetTicks() + 2000;
            }
            toast(S.wake_tries < 10 ? "Starting the Home+ service\xE2\x80\xA6" : "The Home+ service isn\xE2\x80\x99t responding. Restart the TV, or reinstall Home+.", 600000);
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
        } else if (r->status) {
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
        if (r->body && strstr(r->body, "\"controlCenter\"")) settings_key();
        if (r->body && strstr(r->body, "\"inputs\"")) input_key();
        if (r->body) { /* asked for from the Control Center over another app, which brings Home+ to the front */
            int want = strstr(r->body, "\"settings\"") ? OPEN_SETTINGS : strstr(r->body, "\"recents\"") ? OPEN_RECENTS
                     : strstr(r->body, "\"accessibility\"") ? OPEN_ACCESSIBILITY : 0;
            if (want && S.away) S.asked = want; /* coming back starts at the top: opened after that */
            else if (want) open_asked(want);
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
                if (r->tag == TAG_KEY_SET && r->kind == JOB_JSON && S.sheet.open && S.sheet.page == PAGE_HOME) {
                    int index = S.sheet.index, parent_index = S.sheet.parent_index; /* the buttons as they are */
                    settings_page(PAGE_HOME, index);
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
            S.vivid_covers = o && cJSON_IsFalse(cJSON_GetObjectItem(o, "calmColours"));
            S.shelf_still = o && cJSON_IsFalse(cJSON_GetObjectItem(o, "shelfTurns"));
            const char *look = o ? jstr(o, "iconLook") : "";
            S.icon_look = !strcmp(look, "dark") ? LOOK_DARK : !strcmp(look, "clear") ? LOOK_CLEAR : !strcmp(look, "tinted") ? LOOK_TINTED : LOOK_DEFAULT;
            const char *tint = o ? jstr(o, "iconTint") : "";
            S.icon_tint = TINT_INDIGO;
            for (int i = 0; i < NTINTS; i++)
                if (!strcasecmp(tint, TINTS[i].name)) S.icon_tint = i;
            cJSON_Delete(o);
        }
        break;
    case TAG_MEDIA_LINK:
    case TAG_MEDIA_STATUS: { /* Stremio, Jellyfin and Plex sign-in */
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
            if (code_shown) sheet_dismiss();
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
                if (S.at_page == PAGE_ACCOUNTS) /* Cancel goes back there; from an app's options it closes */
                    S.sheet.parent = PAGE_ACCOUNTS, S.sheet.parent_index = S.at_index;
            }
            S.media_poll = SDL_GetTicks() + 3000;
        } else {
            S.media_poll = 0;
            if (code_shown) sheet_dismiss();
            if (err[0]) toast(err, 7000);
            else if (r->tag == TAG_MEDIA_LINK) toast("Couldn\xE2\x80\x99t start the sign-in. Try again.", 5000);
            else if (code_shown) toast("The code expired. Try again.", 5000);
        }
        cJSON_Delete(o);
        break;
    }
    case TAG_TRAKT_LINK:
    case TAG_TRAKT_STATUS: {
        cJSON *o = r->status == 200 ? cJSON_Parse(r->body) : NULL;
        if (!o) {
            if (r->tag == TAG_TRAKT_LINK) toast("Trakt isn\xE2\x80\x99t set up yet. The README\xE2\x80\x99s Trakt section says how.", 7000);
            S.trakt_poll = 0;
            break;
        }
        int connected = cJSON_IsTrue(cJSON_GetObjectItem(o, "connected"));
        cJSON *pend = cJSON_GetObjectItem(o, "pending");
        if (connected) {
            S.trakt_poll = 0;
            S.trakt_connected = 1;
            if (S.sheet.open && S.sheet_big) sheet_dismiss();
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
                sheet_add("Cancel", ACT_CANCEL, 0, 0);
            }
            S.trakt_poll = SDL_GetTicks() + 3000;
        } else {
            S.trakt_poll = 0;
            if (r->tag == TAG_TRAKT_LINK) toast("Trakt sign-in didn\xE2\x80\x99t start. Check the Client ID/Secret (see README).", 6000);
        }
        cJSON_Delete(o);
        break;
    }
    case TAG_ICON:
        if (r->generation == S.generation && r->index < S.napps) {
            App *a = &S.apps[r->index];
            if (r->rgba) {
                make_logo(a, r->rgba, r->w, r->h); /* already shrunk to ICON_FIT while decoding */
                a->icon_state = 2;
            } else {
                a->icon_state = 3;
            }
        }
        break;
    case TAG_ART:
        if (r->generation == S.generation && r->index < S.napps) {
            App *a = &S.apps[r->index];
            a->art_tex = r->rgba ? r_tex(r->rgba, r->w, r->h) : NULL;
            a->art_state = r->rgba ? 2 : 3; /* on show already: it fades in over the glow */
        }
        break;
    case TAG_QR:
        if (r->generation == S.qr_gen && S.sheet.open && S.sheet_qr_s && r->rgba) {
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
    case TAG_LAUNCH:
        if (r->status != 200 || (r->body && strstr(r->body, "\"returnValue\":false"))) {
            S.away = 0;
            S.launching = 0;
            if (S.recents.zooming) sw_reset();
            toast("Couldn\xE2\x80\x99t open that app", 3500);
        }
        break;
    }
}

/* ------------------------------------------------------------------ actions */

/* the focused top-row app's carousel is in the Top Shelf */
static int rail_active(void)
{
    return S.focus < COLS && rail_for(item_app(S.focus));
}

/* Opens an app. params: where in it (its launch parameters, as JSON), or NULL; hint: an
 * optional message the system shows over the app once it has opened. */
static void launch_app(const char *id, const char *params, const char *hint)
{
    cJSON *o = cJSON_CreateObject(), *p = params ? cJSON_Parse(params) : NULL;
    cJSON_AddStringToObject(o, "id", id);
    if (p) cJSON_AddItemToObject(o, "params", p);
    if (hint && hint[0]) cJSON_AddStringToObject(o, "toast", hint);
    char *body = cJSON_PrintUnformatted(o);
    net_post("/api/launch", body, TAG_LAUNCH);
    free(body);
    cJSON_Delete(o);
    S.away = 1;
}

static void launch(const char *id, const char *hint) { launch_app(id, NULL, hint); }

static void sheet_close(void)
{
    tex_free(&S.sheet.title);
    tex_free(&S.sheet_big);
    tex_free(&S.sheet_qr);
    S.sheet_qr_s = 0;
    S.media_poll = 0;
    for (int i = 0; i < S.sheet.nbody; i++) tex_free(&S.sheet.body[i]);
    S.sheet.nbody = 0;
    S.sheet.locked = 0;
    S.trakt_poll = 0;
    for (int i = 0; i < S.sheet.n; i++) {
        tex_free(&S.sheet.it[i].tex);
        tex_free(&S.sheet.it[i].val);
        S.sheet.it[i].chevron = S.sheet.it[i].check = S.sheet.it[i].swatches = 0;
    }
    S.sheet.open = 0;
    S.sheet.n = 0;
    S.sheet.page = S.sheet.parent = S.sheet.parent_index = S.sheet.wide = 0;
}

/* the clock capsule: the time, then the circle; its right edge lines up with the dock's */
static float top_w(void) { return S.clock_w + 24 + 14 + TOP_DOT + 5; }
static float top_x(void) { return RIGHT - top_w(); }

/* Up into the top bar remembers the column it came from, for Down */
static void enter_top(int col)
{
    S.zone = ZONE_TOP;
    S.top_index = TOP_SETTINGS;
    S.top_col = col < 0 ? 0 : col >= COLS ? COLS - 1 : col;
}

/* the dock tile in a column, or the last tile when the row is shorter */
static int dock_tile(int col)
{
    return col < S.nitems ? col : S.nitems > 0 ? S.nitems - 1 : 0;
}

/* Where Down from the top bar lands: the dock tile in the column it was entered from */
static int top_down_tile(void) { return dock_tile(S.top_col); }

/* rect of the focused element: where a sheet grows from */
static void focus_rect(float *x, float *y, float *w, float *h)
{
    if (S.zone == ZONE_TOP) {
        *x = top_x();
        *y = TOP_Y;
        *w = top_w();
        *h = BAR;
    } else if (S.zone == ZONE_GRID && S.focus < S.nitems) {
        *x = LEFT + (S.focus % COLS) * (TILE_W + GAP_X);
        *y = GRID_TOP + (S.focus / COLS) * ROW_H + S.scroll;
        *w = TILE_W;
        *h = TILE_H;
    } else {
        *w = 240;
        *h = 80;
        *x = (W - *w) / 2;
        *y = (H - *h) / 2;
    }
}

/* Closes the sheet the way it opened: its glass shrinks back into the button. */
static void sheet_dismiss(void)
{
    if (S.sheet.open && S.sheet.pw > 0 && !S.reduce_motion) {
        S.ghost.on = 1;
        S.ghost.t = S.sheet.t > 1 ? 1 : S.sheet.t;
        S.ghost.tv = 0;
        S.ghost.fx = S.sheet.fx; S.ghost.fy = S.sheet.fy; S.ghost.fw = S.sheet.fw; S.ghost.fh = S.sheet.fh;
        S.ghost.px = S.sheet.px; S.ghost.py = S.sheet.py; S.ghost.pw = S.sheet.pw; S.ghost.ph = S.sheet.ph;
        S.ghost.pr = S.sheet.pr;
    }
    sheet_close();
}

static void sheet_open(const char *title)
{
    int was_open = S.sheet.open || S.ghost.on; /* replacing a sheet (even one just closing): no morph */
    S.ghost.on = 0;
    sheet_close();
    if (was_open) {
        S.sheet.t = 1; /* swapping one sheet for another: no morph */
        S.sheet.tv = 0;
    } else {
        S.sheet.t = S.sheet.tv = 0;
        focus_rect(&S.sheet.fx, &S.sheet.fy, &S.sheet.fw, &S.sheet.fh);
    }
    if (S.reduce_motion) S.sheet.t = 1, S.sheet.tv = 0; /* appears in place, no growing */
    S.sheet.open = 1;
    S.sheet.index = S.sheet.first = 0;
    S.sheet.scroll = S.sheet.scroll_v = 0;
    S.sheet.title = text_render(S.f_semi, SHEET_TITLE_PX, title, 900, &S.sheet.tw, &S.sheet.th);
}

static void sheet_add(const char *label, Action act, int arg, int destructive)
{
    if (S.sheet.n >= MAX_SHEET) return;
    SheetItem *it = &S.sheet.it[S.sheet.n++];
    it->act = act;
    it->arg = arg;
    it->destructive = destructive;
    it->tex = text_render(S.f_medium, SHEET_BUTTON_PX, label, 600, &it->w, &it->h);
}

/* A button for one app: by its id, not its place in S.apps, which a new app list rebuilds while the
 * sheet is open (a relaunch, or the list after a deletion) */
static void sheet_add_app(const char *label, Action act, const char *id, int destructive)
{
    int n = S.sheet.n;
    sheet_add(label, act, 0, destructive);
    if (S.sheet.n > n) copy_id(S.sheet.it[n].id, id);
}

/* A few lines of explanation under the sheet's title; lines are split on '\n'. */
static void sheet_body(const char *text)
{
    char line[300];
    const char *p = text;
    while (*p && S.sheet.nbody < SHEET_BODY_LINES) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = 0;
        int i = S.sheet.nbody++;
        S.sheet.body[i] = text_render(S.f_medium, SHEET_BODY_PX, line, 900, &S.sheet.bw[i], &S.sheet.bh[i]);
        p = e ? e + 1 : p + n;
    }
}

static void close_apps(const char *id)
{
    cJSON *o = cJSON_CreateObject();
    if (id) cJSON_AddStringToObject(o, "id", id);
    else cJSON_AddTrueToObject(o, "all");
    char *body = cJSON_PrintUnformatted(o);
    net_post("/api/close", body, TAG_CLOSED);
    free(body);
    cJSON_Delete(o);
}

static App *app_by_id(const char *id)
{
    for (int i = 0; i < S.napps; i++)
        if (!strcmp(S.apps[i].id, id)) return &S.apps[i];
    return NULL;
}

static void sw_clear(void)
{
    for (int i = 0; i < S.recents.n; i++) tex_free(&S.recents.c[i].name);
    S.recents.n = S.recents.index = S.recents.close_all = S.recents.zooming = 0;
    S.recents.zoom = S.recents.zoomv = 0;
    S.recents.empty_at = 0;
}

/* gone at once (Home+ went to the back, or came home) */
static void sw_reset(void)
{
    sw_clear();
    S.recents.open = S.recents.loading = S.recents.failed = 0;
    S.recents.t = S.recents.tv = 0;
}

/* Recents opens at once over the blurred home screen; the running apps arrive a
 * moment later, so keys pressed meanwhile never reach the grid behind. */
static void sw_open(void)
{
    sheet_close();
    sw_clear();
    S.recents.open = S.recents.loading = 1;
    S.recents.failed = 0;
    net_get_json("/api/running", TAG_RUNNING, 0, 0);
}

static void sw_fill(const char *json)
{
    if (!S.recents.open || !S.recents.loading) return; /* closed meanwhile */
    S.recents.loading = 0;
    cJSON *o = cJSON_Parse(json), *v;
    cJSON *list = o ? cJSON_GetObjectItem(o, "apps") : NULL;
    cJSON_ArrayForEach(v, list) {
        if (S.recents.n >= MAX_RUNNING) break;
        int i = S.recents.n++;
        memset(&S.recents.c[i], 0, sizeof S.recents.c[i]);
        copy_id(S.recents.c[i].id, jstr(v, "id"));
        S.recents.c[i].name = text_render(S.f_semi, 30, jstr(v, "title"), SW_W, &S.recents.c[i].nw, &S.recents.c[i].nh);
        S.recents.c[i].x = i + 0.5f; /* they slide in from the right */
    }
    cJSON_Delete(o);
}

static void sw_ok(void)
{
    if (S.recents.zooming || !S.recents.n) return;
    if (S.recents.close_all) {
        close_apps(NULL);
        for (int i = 0; i < S.recents.n; i++) {
            S.recents.c[i].flying = 1;
            S.recents.c[i].fly = -0.12f * abs(i - S.recents.index); /* one after another, outwards */
        }
        S.recents.close_all = 0;
        return;
    }
    if (S.recents.c[S.recents.index].flying) return;
    S.recents.zooming = 1; /* the card grows to fill the screen while the app opens */
    S.recents.zoom_at = SDL_GetTicks();
    launch(S.recents.c[S.recents.index].id, NULL);
}

static void sw_update(float dt, int *anim)
{
    spring(&S.recents.t, &S.recents.tv, S.recents.open ? 1.0f : 0.0f, S.recents.open ? 0.42f : 0.3f, S.recents.open ? 0.86f : 1.0f, dt, anim);
    if (!S.recents.open && S.recents.t < 0.004f) {
        if (S.recents.n) sw_clear();
        return;
    }
    for (int i = 0; i < S.recents.n; i++) {
        spring(&S.recents.c[i].x, &S.recents.c[i].xv, (float)(i - S.recents.index), 0.4f, 0.86f, dt, anim);
        spring(&S.recents.c[i].s, &S.recents.c[i].sv, i == S.recents.index && !S.recents.close_all ? 1.0f : 0.0f, 0.35f, 0.75f, dt, anim);
        if (S.recents.c[i].flying) spring(&S.recents.c[i].fly, &S.recents.c[i].flyv, 1.2f, 0.35f, 1.0f, dt, anim);
    }
    /* a card that flew off is gone; the others close the gap on their springs */
    for (int i = 0; i < S.recents.n; i++) {
        if (S.recents.c[i].fly < 1.0f) continue;
        tex_free(&S.recents.c[i].name);
        memmove(&S.recents.c[i], &S.recents.c[i + 1], (size_t)(S.recents.n - i - 1) * sizeof S.recents.c[0]);
        S.recents.n--;
        if (S.recents.index > i) S.recents.index--;
        if (S.recents.index >= S.recents.n) S.recents.index = S.recents.n > 0 ? S.recents.n - 1 : 0;
        if (!S.recents.n) S.recents.empty_at = SDL_GetTicks();
        i--;
        *anim = 1;
    }
    Uint32 now = SDL_GetTicks();
    if (S.recents.open && S.recents.empty_at && now - S.recents.empty_at > 500) S.recents.open = 0; /* nothing left to show */
    if (S.recents.zooming) {
        spring(&S.recents.zoom, &S.recents.zoomv, 1.0f, 0.45f, 0.95f, dt, anim);
        if (now - S.recents.zoom_at > 10000) { /* the app never came up */
            sw_reset();
            toast("That app is taking a while to open", 3500);
        }
    }
    if (S.recents.loading) *anim = 1; /* its placeholders shimmer */
}

/* Control Center: TV settings + accounts */
/* ------------------------------------------------------------------ first run */

static void setup_check(void) { net_get_json("/api/status", TAG_STATUS, 0, 0); }

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
    if (S.setup == SETUP_NO_HBC || S.setup == SETUP_DENIED || S.setup == SETUP_FAILED)
        sheet_add("Try Again", ACT_SETUP_RETRY, 0, 0);
}

static void setup_set(int state)
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

static void set_home_mode(const char *mode, int asked)
{
    char body[64];
    snprintf(body, sizeof body, "{\"mode\":\"%s\"%s}", mode, asked ? ",\"asked\":true" : "");
    net_post("/api/home", body, TAG_HOME_SET);
    snprintf(S.home_mode, sizeof S.home_mode, "%s", mode);
}

static void on_status(Result *r)
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

static void on_elevate(Result *r)
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

static void setup_retry_now(void)
{
    S.setup_tries = S.elevate_tries = 0;
    S.setup = SETUP_CHECK;
    setup_screen();
    setup_check();
}

static int source_installed(int src)
{
    for (int i = 0; i < S.napps; i++)
        if (media_source(&S.apps[i]) == src) return 1;
    return 0;
}

static int stremio_installed(void) { return source_installed(SRC_STREMIO); }

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

static void settings_page(int page, int index)
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
static void settings_again(void)
{
    if (!S.at_page) return;
    settings_page(S.at_page, S.at_index);
    if (S.at_page != PAGE_MAIN) S.sheet.parent_index = S.at_parent_index;
}

/* Tinted's colours on their row in App Icons: swatch i's centre, the row's right end like a value's */
#define SWATCH_D 34
#define SWATCH_PITCH 50
static float swatch_x(float bx, float bw, int i)
{
    return bx + bw - 36 - SWATCH_D / 2.0f - (NTINTS - 1 - i) * SWATCH_PITCH;
}

/* the colour under the pointer on that row, or -1 */
static int swatch_at(float mx)
{
    float bx = S.sheet.px + PAD, bw = S.sheet.pw - 2 * PAD;
    for (int i = 0; i < NTINTS; i++)
        if (fabsf(mx - swatch_x(bx, bw, i)) < SWATCH_PITCH / 2.0f) return i;
    return -1;
}

/* Tinted takes colour i: the tiles behind the sheet change, its row says the colour's name */
static void icon_tint_set(int i)
{
    char body[48];
    int index = S.sheet.index, parent_index = S.sheet.parent_index;
    S.icon_tint = i;
    snprintf(body, sizeof body, "{\"iconTint\":\"%s\"}", TINTS[i].name);
    net_post("/api/prefs", body, 0);
    settings_page(PAGE_ICONS, index);
    S.sheet.parent_index = parent_index;
}

static void open_settings_sheet(void)
{
    settings_page(PAGE_MAIN, 0);
}


static void open_app_sheet(void)
{
    App *a = item_app(S.focus);
    if (!a || S.zone != 0) return;
    sheet_open(a->title);
    sheet_add("Move", ACT_MOVE, 0, 0);
    sheet_add("Move to Front", ACT_FRONT, 0, 0);
    int src = media_source(a);
    if (src && !(src == SRC_STREMIO ? S.rail_stremio : S.src_connected[src])) {
        char t[48];
        snprintf(t, sizeof t, "Sign In to %s", SRC_NAME[src]);
        sheet_add(t, ACT_MEDIA_LINK, src, 0);
    }
    sheet_add("Hide App", ACT_HIDE, 0, 0); /* not red: Hidden gives it back */
    if (a->removable) sheet_add_app("Delete App", ACT_DELETE_ASK, a->id, 1); /* from the TV, like tvOS's */
    sheet_add("Cancel", ACT_CANCEL, 0, 0);
}

static void open_hidden_sheet(void)
{
    sheet_open("Hidden Apps");
    int n = 0;
    for (int i = 0; i < S.napps; i++) {
        if (!is_hidden(S.apps[i].id)) continue;
        char label[200];
        snprintf(label, sizeof label, "Show \xE2\x80\x9C%s\xE2\x80\x9D", S.apps[i].title);
        sheet_add_app(label, ACT_UNHIDE, S.apps[i].id, 0);
        n++;
    }
    if (n > 1) sheet_add("Show All", ACT_UNHIDE_ALL, 0, 0);
    sheet_add("Cancel", ACT_CANCEL, 0, 0);
}

static void move_item_to(int to)
{
    int last = S.nitems - 1;
    if (last >= 0 && S.items[last] < 0) last--; /* never past the Hidden tile */
    if (to > last) to = last;
    if (to < 0 || to == S.focus) return;
    int v = S.items[S.focus];
    if (to < S.focus) memmove(&S.items[to + 1], &S.items[to], (S.focus - to) * sizeof(int));
    else memmove(&S.items[S.focus], &S.items[S.focus + 1], (to - S.focus) * sizeof(int));
    S.items[to] = v;
    S.focus = to;
}

static void run_action(Action act, int arg, const char *id)
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
        break;
    case ACT_HOME_NO:
        set_home_mode("off", 1);
        toast("You can turn this on later in Settings", 3000);
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
        if (S.at_page) S.sheet.page = PAGE_CONFIRM, S.sheet.parent = PAGE_MAIN, S.sheet.parent_index = S.at_index;
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
        if (S.rail_source == arg) S.rail_source = SRC_NONE, S.nrail = 0;
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
        S.sheet.page = PAGE_CONFIRM;
        S.sheet.parent = PAGE_ACCOUNTS;
        S.sheet.parent_index = S.at_index;
        break;
    }
    case ACT_UP:
        if (S.sheet.parent) settings_page(S.sheet.parent, S.sheet.parent_index);
        else sheet_dismiss();
        break;
    case ACT_TMDB: /* the key is set over SSH */
        toast(S.tmdb_configured ? "Streaming apps show what\xE2\x80\x99s popular on TMDB. tmdbSetup with an empty key removes it."
                                : "Add a free TMDB key over SSH to see what\xE2\x80\x99s popular on streaming apps. See the README.", 8000);
        settings_again();
        break;
    case ACT_ACCESSIBILITY:
        settings_page(PAGE_ACCESS, 0);
        break;
    case ACT_PAGE: /* a Settings page (App Icons on the look that's on); Back returns to the row that opened it */
        settings_page(arg, arg == PAGE_ICONS ? S.icon_look : 0);
        S.sheet.parent_index = S.at_index;
        break;
    case ACT_HIDDEN:
        open_hidden_sheet();
        S.sheet.parent = PAGE_MAIN;
        S.sheet.parent_index = S.at_index;
        break;
    case ACT_CLOCK:
        S.clock24 = !S.clock24;
        S.clock_min = -1; /* redraw the clock now */
        net_post("/api/prefs", S.clock24 ? "{\"clock24\":true}" : "{\"clock24\":false}", 0);
        settings_again();
        break;
    case ACT_DIM:
    case ACT_OFF: { /* each press steps to the next choice */
        static const int dim[] = { 1, 2, 5, 10, -1 }, off[] = { 5, 10, 20, 30, 60, -1 };
        int is_dim = act == ACT_DIM, n = is_dim ? 5 : 6;
        const int *opt = is_dim ? dim : off;
        Uint32 cur = is_dim ? S.idle_dim_ms : S.idle_off_ms, dflt = is_dim ? IDLE_DIM_MS : IDLE_OFF_MS;
        int now = cur == IDLE_NEVER ? -1 : (int)((cur ? cur : dflt) / 60000), k = 0;
        while (k < n && opt[k] != now) k++;
        int next = opt[(k + 1) % n];
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
    case ACT_ORBIT:
        S.orbit_off = !S.orbit_off;
        net_post("/api/prefs", S.orbit_off ? "{\"pixelShift\":false}" : "{\"pixelShift\":true}", 0);
        settings_again();
        break;
    case ACT_CALM: /* covers are made from the icons: fetch them again to remake them */
        S.vivid_covers = !S.vivid_covers;
        net_post("/api/prefs", S.vivid_covers ? "{\"calmColours\":false}" : "{\"calmColours\":true}", 0);
        for (int i = 0; i < S.napps; i++) {
            App *ca = &S.apps[i];
            if (!ca->icon[0]) continue;
            ca->icon_state = 1;
            net_get_image(ca->icon, TAG_ICON, i, S.generation, 1, ICON_FIT, ICON_FIT);
        }
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

/* ---- Control Center */

static float cc_grid_x(void) { return RIGHT - 4 * CC_U - 3 * CC_G; } /* its right edge: the clock's */
static float cc_grid_y(void) { return TOP_Y + BAR + 26; }

/* Places the controls in order on the four-unit grid, each in the first space it fits (reading
 * order), in the rows that fit under the clock; the + slot (while editing) takes the first free
 * unit after the last of them. */
static void cc_pack(void)
{
    unsigned char used[CC_ROWS][4];
    memset(used, 0, sizeof used);
    int last = -1;
    for (int i = 0; i < S.cc.n; i++) {
        CCItem *c = &S.cc.it[i];
        c->w = c->units == 1 ? 1 : 2;
        c->h = c->units == 4 ? 2 : 1;
        c->row = -1;
        for (int r = 0; r + c->h <= CC_ROWS && c->row < 0; r++)
            for (int col = 0; col + c->w <= 4 && c->row < 0; col++) {
                int fits = 1;
                for (int y = r; y < r + c->h; y++)
                    for (int x = col; x < col + c->w; x++) fits &= !used[y][x];
                if (!fits) continue;
                for (int y = r; y < r + c->h; y++)
                    for (int x = col; x < col + c->w; x++) used[y][x] = 1;
                c->row = r, c->col = col;
                if (r * 4 + col > last) last = r * 4 + col;
            }
    }
    S.cc.plus_row = -1;
    for (int u = last + 1; u < CC_ROWS * 4; u++)
        if (!used[u / 4][u % 4]) {
            S.cc.plus_row = u / 4, S.cc.plus_col = u % 4;
            break;
        }
}

/* the controls with a place on the grid */
static int cc_placed(void)
{
    int k = 0;
    for (int i = 0; i < S.cc.n; i++) k += S.cc.it[i].row >= 0;
    return k;
}

/* Gives control i the first of `sizes` (units, 0-ended) that leaves `need` controls on the grid, it
 * among them; 0, and as it was, if none does (as the overlay's fit()) */
static int cc_fit(int i, const char *sizes, int need)
{
    CCItem *c = &S.cc.it[i];
    int was = c->units;
    for (const char *u = sizes; *u; u++) {
        c->units = *u;
        cc_pack();
        if (c->row >= 0 && cc_placed() >= need) return 1;
    }
    c->units = was;
    cc_pack();
    return 0;
}

/* the rows in use (with the + slot while editing) */
static int cc_rows(void)
{
    int rows = 0;
    for (int i = 0; i < S.cc.n; i++)
        if (S.cc.it[i].row >= 0 && S.cc.it[i].row + S.cc.it[i].h > rows) rows = S.cc.it[i].row + S.cc.it[i].h;
    if (S.cc.editing && S.cc.plus_row + 1 > rows) rows = S.cc.plus_row + 1;
    return rows;
}

/* Where focusable thing i is: a control (0..n-1); while editing, the + slot (n) and Reset
 * (n + 1). 0: it isn't on screen (no room for it). */
static int cc_rect(int i, float *x, float *y, float *w, float *h)
{
    int col, row, cw = 1, ch = 1;
    if (i >= 0 && i < S.cc.n) {
        const CCItem *c = &S.cc.it[i];
        if (c->row < 0) return 0;
        col = c->col, row = c->row, cw = c->w, ch = c->h;
    } else if (i == S.cc.n && S.cc.editing && S.cc.plus_row >= 0) {
        col = S.cc.plus_col, row = S.cc.plus_row;
    } else if (i == S.cc.n + 1 && S.cc.editing) { /* under the grid and its hint */
        *x = cc_grid_x(), *y = cc_grid_y() + cc_rows() * (CC_U + CC_G) + 44;
        *w = 4 * CC_U + 3 * CC_G, *h = 64;
        return 1;
    } else {
        return 0;
    }
    *x = cc_grid_x() + col * (CC_U + CC_G);
    *y = cc_grid_y() + row * (CC_U + CC_G);
    *w = cw * CC_U + (cw - 1) * CC_G;
    *h = ch * CC_U + (ch - 1) * CC_G;
    return 1;
}

static int cc_find(const char *id)
{
    for (int i = 0; i < S.cc.n; i++)
        if (!strcmp(S.cc.it[i].id, id)) return i;
    return -1;
}

/* what went wrong, said in the hint's place under the grid as over other apps (NULL: nothing) */
static void cc_problem(const char *text)
{
    tex_free(&S.cc.problem);
    if (text) S.cc.problem = text_render(S.f_medium, HINT_PX, text, 4 * CC_U + 3 * CC_G, &S.cc.prw, &S.cc.prh);
}

/* asks the service to do something (the overlay's run()): what went wrong before is let go */
static void cc_post(const char *body)
{
    cc_problem(NULL);
    net_post("/api/control", body, TAG_CONTROL);
}

static void cc_open(void)
{
    S.cc.open = 1;
    S.cc.editing = S.cc.list = S.cc.list_only = 0;
    cc_problem(NULL);
    S.cc.picked = S.cc.ran = -1;
    int net = cc_find("network"); /* nearest the clock, and harmless (unlike Power Off) */
    S.cc.index = net >= 0 ? net : 0;
    S.cc.to_network = net < 0; /* the first time, the list is on its way */
    S.cc.focus_at = SDL_GetTicks();
    net_get_json("/api/control", TAG_CONTROL, 0, 0);
}

static void cc_save(void);
static void cc_close(void)
{
    if (S.cc.picked >= 0) { /* one being moved stays where it is now */
        S.cc.picked = -1;
        cc_save();
    }
    S.cc.open = S.cc.editing = S.cc.list = S.cc.list_only = 0;
}

/* saves the list as it is on screen (the service answers with the whole state) */
static void cc_save(void)
{
    cJSON *o = cJSON_CreateObject(), *arr;
    cJSON_AddStringToObject(o, "action", "layout");
    arr = cJSON_AddArrayToObject(o, "controls");
    for (int i = 0; i < S.cc.n; i++) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "id", S.cc.it[i].id);
        cJSON_AddStringToObject(c, "size", S.cc.it[i].units == 4 ? "large" : S.cc.it[i].units == 2 ? "wide" : "small");
        cJSON_AddItemToArray(arr, c);
    }
    char *body = cJSON_PrintUnformatted(o);
    cc_post(body);
    free(body);
    cJSON_Delete(o);
}

/* the picked control moves one place earlier or later in the order (Left, Right), or to the row
 * above or below (Up, Down), and the others reflow around it */
static void cc_move_picked(int dx, int dy)
{
    int i = S.cc.picked, to = i + dx;
    if (dy) {
        const CCItem *c = &S.cc.it[i];
        int row = dy > 0 ? c->row + c->h : c->row - 1, best = -1;
        float bd = 1e9f, cx = c->col + c->w / 2.0f;
        for (int k = 0; k < S.cc.n; k++) {
            const CCItem *o = &S.cc.it[k];
            if (k == i || o->row < 0 || row < o->row || row >= o->row + o->h) continue;
            float d = fabsf(o->col + o->w / 2.0f - cx);
            if (d < bd) bd = d, best = k;
        }
        if (best < 0 && dy > 0) best = S.cc.n - 1; /* below the last row: the end */
        if (best < 0) return;
        to = best;
    }
    if (to < 0 || to >= S.cc.n || to == i) return;
    CCItem t = S.cc.it[i];
    if (to > i) memmove(&S.cc.it[i], &S.cc.it[i + 1], (size_t)(to - i) * sizeof t);
    else memmove(&S.cc.it[to + 1], &S.cc.it[to], (size_t)(i - to) * sizeof t);
    S.cc.it[to] = t;
    S.cc.picked = S.cc.index = to;
    cc_pack();
}

/* the nearest control that way (they come in three sizes); up past the top row: the clock */
static void cc_move(int dx, int dy)
{
    if (S.cc.picked >= 0) {
        cc_move_picked(dx, dy);
        return;
    }
    float x, y, w, h, best_d = 1e9f;
    int best = -1, n = S.cc.n + (S.cc.editing ? 2 : 0);
    if (!cc_rect(S.cc.index, &x, &y, &w, &h)) x = cc_grid_x(), y = cc_grid_y(), w = h = 0;
    float cx = x + w / 2, cy = y + h / 2;
    for (int i = 0; i < n; i++) {
        float ix, iy, iw, ih;
        if (i == S.cc.index || !cc_rect(i, &ix, &iy, &iw, &ih)) continue;
        float ax = ix + iw / 2 - cx, ay = iy + ih / 2 - cy, along = dx ? ax * dx : ay * dy, across = fabsf(dx ? ay : ax);
        int beside = dx ? iy < y + h && iy + ih > y : ix < x + w && ix + iw > x;
        if (along <= 1 || (!beside && across > along)) continue; /* as tvOS's focus: beside it, or within 45 degrees */
        float d = along + 2.5f * across;
        if (d < best_d) best_d = d, best = i;
    }
    if (best >= 0) S.cc.index = best, S.cc.focus_at = SDL_GetTicks(), cc_problem(NULL); /* what went wrong goes with the focus */
    else if (dy < 0 && !S.cc.editing) cc_close();
}

/* A list control's list (Picture's modes, the inputs) opens in the Control Center's place, like
 * tvOS's: a picture mode applies there and then (and the tick moves); Back goes back. */
#define CC_LIST_ROWS 8
static int is_back(SDL_Keycode k);
static void cc_list_texts(void)
{
    tex_free(&S.cc.list_title);
    S.cc.list_title = text_render(S.f_semi, 24, S.cc.list == 1 ? "Picture Mode" : "Inputs", 0, &S.cc.ltw, &S.cc.lth);
    S.cc.list_n = S.cc.list == 1 ? S.cc.nmodes : S.cc.ninputs;
    if (S.cc.list_n > 24) S.cc.list_n = 24;
    for (int i = 0; i < 24; i++) {
        tex_free(&S.cc.list_row[i]);
        tex_free(&S.cc.list_val[i]);
        if (i >= S.cc.list_n) continue;
        const char *name = S.cc.list == 1 ? S.cc.modes[i].name : S.cc.inputs[i].name;
        const char *val = S.cc.list == 1 ? (i == S.cc.mode ? "\xE2\x9C\x93" : "") : S.cc.inputs[i].plugged ? "" : "Nothing Connected";
        S.cc.list_row[i] = text_render(S.f_medium, CC_LIST_PX, name, 300, &S.cc.lrw[i], &S.cc.lrh[i]);
        if (val[0]) S.cc.list_val[i] = text_render(S.f_medium, val[0] == '\xE2' ? CC_LIST_PX : CC_DETAIL_PX, val, 0, &S.cc.lvw[i], &S.cc.lvh[i]);
    }
}

/* While the Control Center is being edited, or its last row is over the dock, the home screen steps
 * back (as iOS's does): this much black over it, 0..1, in the picture its glass refracts too, so the
 * glass over the dock (that row, Reset, the hints) dims with what's under it */
static float cc_dim(void)
{
    int over_dock = cc_grid_y() + cc_rows() * (CC_U + CC_G) - CC_G > DOCK_Y;
    return S.cc.editing || over_dock ? 150 / 255.0f * fminf(fmaxf(S.cc.t, 0), 1) : 0;
}

/* The gallery's layout: each group's name, then its controls four to a row, buttons with their names
 * under them (as iOS 18's Add a Control shows them). Wider than the grid (its right edge the grid's),
 * so names read from the couch */
#define GAL_W 680
#define GAL_PAD 24
#define GAL_HEAD 66
#define GAL_GROUP 52    /* a group's name, near the top: room between it and its controls */
#define GAL_GROUP_TOP 6
#define GAL_BTN 72
#define GAL_ROW 144
#define GAL_NAME_PX 20
#define GAL_NAME_GAP 16 /* between neighbours' names: a longer one takes two lines */
static void cc_gallery_build(void)
{
    for (int i = 0; i < MAX_CAT; i++) tex_free(&S.cc.gal_label[i][0]), tex_free(&S.cc.gal_label[i][1]);
    for (int i = 0; i < 6; i++) tex_free(&S.cc.gal_head[i]);
    tex_free(&S.cc.list_title);
    S.cc.list_title = text_render(S.f_semi, 24, "Add a Control", 0, &S.cc.ltw, &S.cc.lth);
    float cw = (GAL_W - 2 * GAL_PAD) / 4.0f, y = GAL_HEAD;
    const char *group = "";
    int col = 0;
    S.cc.gal_n = S.cc.gal_nhead = 0;
    for (int i = 0; i < S.cc.ncat; i++) { /* the catalogue is in its groups' order */
        const __typeof__(S.cc.cat[0]) *e = &S.cc.cat[i];
        if (!e->available || cc_find(e->id) >= 0) continue;
        if (strcmp(e->group, group) && S.cc.gal_nhead < 6) { /* a new group: its name, under the last one's controls */
            if (S.cc.gal_n) y += GAL_ROW;
            group = e->group;
            S.cc.gal_head[S.cc.gal_nhead] = text_render(S.f_semi, 22, group, 0, &S.cc.gal_hw[S.cc.gal_nhead], &S.cc.gal_hh[S.cc.gal_nhead]);
            S.cc.gal_hy[S.cc.gal_nhead++] = y;
            y += GAL_GROUP;
            col = 0;
        } else if (col == 4) {
            y += GAL_ROW;
            col = 0;
        }
        int k = S.cc.gal_n++;
        S.cc.gal_cat[k] = i;
        S.cc.gal_x[k] = GAL_PAD + col * cw + (cw - GAL_BTN) / 2;
        S.cc.gal_y[k] = y;
        int room = (int)cw - GAL_NAME_GAP, w0, h0, split = -1, best = 1 << 30;
        Tex *t = text_render(S.f_medium, GAL_NAME_PX, e->title, 0, &w0, &h0);
        tex_free(&t);
        for (const char *sp = e->title; w0 > room && (sp = strchr(sp, ' ')); sp++) { /* two lines: the split whose longer line is shortest */
            char first[48];
            int w1, w2, h;
            snprintf(first, sizeof first, "%.*s", (int)(sp - e->title), e->title);
            t = text_render(S.f_medium, GAL_NAME_PX, first, 0, &w1, &h), tex_free(&t);
            t = text_render(S.f_medium, GAL_NAME_PX, sp + 1, 0, &w2, &h), tex_free(&t);
            if ((w1 > w2 ? w1 : w2) < best) best = w1 > w2 ? w1 : w2, split = (int)(sp - e->title);
        }
        if (split >= 0) { /* "Dynamic" / "Tone Mapping" */
            char first[48];
            snprintf(first, sizeof first, "%.*s", split, e->title);
            S.cc.gal_label[k][0] = text_render(S.f_medium, GAL_NAME_PX, first, room, &S.cc.gal_lw[k][0], &S.cc.gal_lh[k][0]);
            S.cc.gal_label[k][1] = text_render(S.f_medium, GAL_NAME_PX, e->title + split + 1, room, &S.cc.gal_lw[k][1], &S.cc.gal_lh[k][1]);
        } else {
            S.cc.gal_label[k][0] = text_render(S.f_medium, GAL_NAME_PX, e->title, room, &S.cc.gal_lw[k][0], &S.cc.gal_lh[k][0]);
        }
        col++;
    }
    S.cc.gal_h = (S.cc.gal_n ? y + GAL_ROW : GAL_HEAD + 60) + 4;
}

/* the gallery's control in direction (dx, dy) from the focused one: the nearest that way */
static void cc_gallery_move(int dx, int dy)
{
    int cur = S.cc.list_index, best = -1;
    float bd = 1e9f;
    for (int k = 0; k < S.cc.gal_n; k++) {
        float ax = S.cc.gal_x[k] - S.cc.gal_x[cur], ay = S.cc.gal_y[k] - S.cc.gal_y[cur];
        if (k == cur || (dx && ax * dx <= 1) || (dy && ay * dy <= 1)) continue;
        float d = dx ? fabsf(ax) + 2.5f * fabsf(ay) : fabsf(ay) + 2.5f * fabsf(ax);
        if (d < bd) bd = d, best = k;
    }
    if (best >= 0) S.cc.list_index = best;
}

static void cc_add(int cat);
static void cc_list_open(int which)
{
    S.cc.list = S.cc.list_kind = which;
    S.cc.list_index = S.cc.list_first = 0;
    if (which == 3) {
        cc_gallery_build();
        return;
    }
    cc_list_texts();
    S.cc.list_index = which == 1 && S.cc.mode >= 0 ? S.cc.mode : 0;
    S.cc.list_first = S.cc.list_index >= CC_LIST_ROWS ? S.cc.list_index - CC_LIST_ROWS + 1 : 0;
}

static void cc_list_key(SDL_Keycode k)
{
    if (S.cc.list == 3) { /* the gallery's buttons, in their grid */
        if (k == SDLK_LEFT) cc_gallery_move(-1, 0);
        else if (k == SDLK_RIGHT) cc_gallery_move(1, 0);
        else if (k == SDLK_UP) cc_gallery_move(0, -1);
        else if (k == SDLK_DOWN) cc_gallery_move(0, 1);
        else if (is_back(k)) S.cc.list = 0;
        return;
    }
    if (k == SDLK_UP && S.cc.list_index > 0) S.cc.list_index--;
    else if (k == SDLK_DOWN && S.cc.list_index < S.cc.list_n - 1) S.cc.list_index++;
    else if (is_back(k) && S.cc.list_only) cc_close();
    else if (is_back(k)) S.cc.list = 0;
    if (S.cc.list_index < S.cc.list_first) S.cc.list_first = S.cc.list_index;
    if (S.cc.list_index >= S.cc.list_first + CC_LIST_ROWS) S.cc.list_first = S.cc.list_index - CC_LIST_ROWS + 1;
}

static void cc_list_ok(void)
{
    int i = S.cc.list_index;
    if (S.cc.list == 3) { /* add it: the gallery closes, and it has the focus */
        if (i < S.cc.gal_n) {
            S.cc.list = 0;
            cc_add(S.cc.gal_cat[i]);
        }
        return;
    }
    if (i < 0 || i >= S.cc.list_n) return;
    if (S.cc.list == 1) {
        char body[96];
        snprintf(body, sizeof body, "{\"action\":\"pictureMode\",\"value\":\"%s\"}", S.cc.modes[i].id);
        cc_post(body);
    } else {
        cc_close();
        launch(S.cc.inputs[i].app, NULL);
    }
}

/* editing: OK picks a control up (and puts it down), the + slot adds one, Reset asks */
static void cc_edit_ok(void)
{
    if (S.cc.picked >= 0) {
        S.cc.picked = -1;
        cc_save();
    } else if (S.cc.index < S.cc.n) {
        S.cc.picked = S.cc.index;
    } else if (S.cc.index == S.cc.n) {
        if (S.cc.plus_row >= 0) cc_list_open(3);
    } else {
        sheet_open("Reset Control Center?");
        sheet_body("The controls go back to how they were at first.");
        sheet_add("Reset", ACT_CC_RESET, 0, 1);
        sheet_add("Cancel", ACT_CANCEL, 0, 0);
        S.sheet.index = 1;
    }
}

/* the catalogue's entry for a control */
static int cc_cat(const char *id)
{
    for (int i = 0; i < S.cc.ncat; i++)
        if (!strcmp(S.cc.cat[i].id, id)) return i;
    return -1;
}

/* holding OK: Edit Controls; while editing, the focused control's sizes and Remove */
static void cc_long_press(void)
{
    if (S.cc.list || S.cc.picked >= 0) return;
    if (!S.cc.editing) {
        sheet_open("Control Center");
        sheet_add("Edit Controls", ACT_CC_EDIT, 0, 0);
        sheet_add("Cancel", ACT_CANCEL, 0, 0);
        return;
    }
    if (S.cc.index >= S.cc.n) return;
    CCItem *c = &S.cc.it[S.cc.index];
    int e = cc_cat(c->id);
    int sizes = e >= 0 && S.cc.cat[e].sizes[1]; /* a choice of size only when there are two or more */
    int removable = e < 0 || !S.cc.cat[e].pinned; /* not Settings: the only way in */
    if (!sizes && !removable) { /* nothing to change about it */
        c->shake = SDL_GetTicks();
        return;
    }
    sheet_open(c->name_s[0] ? c->name_s : c->title_s);
    static const char *const NAME[5] = { "", "Small", "Wide", "", "Large" };
    for (int k = 0; sizes && S.cc.cat[e].sizes[k]; k++) {
        int u = S.cc.cat[e].sizes[k];
        sheet_add(NAME[u], ACT_CC_SIZE, u, 0);
        S.sheet.it[S.sheet.n - 1].check = u == c->units; /* in the middle like the rest, ticked on the right */
    }
    if (removable) sheet_add("Remove", ACT_CC_REMOVE, 0, 1);
    sheet_add("Cancel", ACT_CANCEL, 0, 0);
    if (!sizes) S.sheet.index = S.sheet.n - 1; /* Remove first: Cancel has the focus, so a stray OK keeps it */
}

/* a control from the catalogue, at the end in the first of its sizes that fits, with the focus; with
 * no room for it at any of them it isn't added, and that's said under the grid */
static void cc_add(int cat)
{
    if (cat < 0 || cat >= S.cc.ncat || S.cc.n >= MAX_CC) return;
    int had = cc_placed();
    CCItem *c = &S.cc.it[S.cc.n++];
    memset(c, 0, sizeof *c);
    memcpy(c->id, S.cc.cat[cat].id, sizeof c->id); /* the same size, and ended */
    c->units = S.cc.cat[cat].sizes[0] ? S.cc.cat[cat].sizes[0] : 1;
    c->icon = S.cc.cat[cat].icon;
    c->available = 1;
    if (!cc_fit(S.cc.n - 1, S.cc.cat[cat].sizes[0] ? S.cc.cat[cat].sizes : "\1", had + 1)) {
        char t[80];
        snprintf(t, sizeof t, "No room for %s", S.cc.cat[cat].title);
        S.cc.n--;
        cc_pack();
        cc_problem(t);
        return;
    }
    cc_texts(c, S.cc.cat[cat].title, ""); /* its value comes with the service's answer */
    S.cc.index = S.cc.n - 1;
    cc_save();
}

static void cc_activate(void)
{
    if (S.cc.list) {
        cc_list_ok();
        return;
    }
    if (S.cc.editing) {
        cc_edit_ok();
        return;
    }
    if (S.cc.index >= S.cc.n) return;
    CCItem *c = &S.cc.it[S.cc.index];
    if (!c->available) { /* Not Available: nothing happens, which it shows */
        c->shake = SDL_GetTicks();
        return;
    }
    if (!strcmp(c->id, "power")) {
        cc_post("{\"action\":\"powerOff\"}");
        cc_close();
    } else if (!strcmp(c->id, "screenOff")) {
        cc_post("{\"action\":\"screenOff\"}");
        cc_close();
    } else if (!strcmp(c->id, "network")) { /* LG's network settings, as its own Quick Settings open them */
        cc_close();
        launch_app("com.webos.app.firstuse-overlay", "{\"target\":\"network\"}", NULL);
    } else if (!strcmp(c->id, "pictureMode")) {
        cc_list_open(1);
    } else if (!strcmp(c->id, "inputs")) {
        cc_list_open(2);
    } else if (!strcmp(c->id, "recents")) {
        cc_close();
        sw_open();
    } else if (!strcmp(c->id, "accessibility")) {
        cc_close();
        settings_page(PAGE_ACCESS, 0);
    } else if (!strcmp(c->id, "settings")) {
        cc_close();
        open_settings_sheet();
    } else if (!strcmp(c->kind, "stepper") || !strcmp(c->kind, "switch")) { /* the service steps or switches it */
        char body[96];
        snprintf(body, sizeof body, "{\"action\":\"run\",\"value\":\"%s\"}", c->id);
        cc_post(body);
        S.cc.ran = S.cc.index;
        if (c->units == 1 && !strcmp(c->kind, "stepper")) c->flash = SDL_GetTicks(); /* its new value shows under it */
    }
}

static void activate(void)
{
    if (S.zone == ZONE_TOP) {
        cc_open();
        return;
    }
    if (S.zone == 1) {
        App *a = item_app(S.focus);
        if (a && S.rail_index < S.nrail) {
            /* LG's apps can't be sent to a title: open the app and say where the title is */
            char hint[300];
            const char *name = S.rail[S.rail_index].name;
            if (S.rail_source == SRC_FEATURED && S.rail_trending) { /* not the app's: just open it */
                launch(a->id, NULL);
                return;
            }
            if (S.rail_source == SRC_FEATURED) {
                snprintf(hint, sizeof hint, "Search for \xE2\x80\x9C%s\xE2\x80\x9D in %s", name, a->title);
                launch(a->id, hint);
                snprintf(hint, sizeof hint, "Opening %s. Search for \xE2\x80\x9C%s\xE2\x80\x9D.", a->title, name);
            } else {
                snprintf(hint, sizeof hint, "\xE2\x80\x9C%s\xE2\x80\x9D is in %s\xE2\x80\x99s Continue Watching", name, a->title);
                launch(a->id, hint);
                snprintf(hint, sizeof hint, "Opening %s. Look for \xE2\x80\x9C%s\xE2\x80\x9D in Continue Watching.", a->title, name);
            }
            toast(hint, 6000);
        }
        return;
    }
    if (S.moving) {
        S.moving = 0;
        save_layout();
        return;
    }
    if (S.focus < S.nitems && S.items[S.focus] < 0) {
        open_hidden_sheet();
        return;
    }
    App *a = item_app(S.focus);
    if (!a || (S.launching && SDL_GetTicks() - S.launch_at < 3000)) return; /* a second OK while it opens */
    launch(a->id, NULL);
    /* the tile breathes and says so until the app is in front (webOS takes 1-5 s) */
    char msg[200];
    snprintf(msg, sizeof msg, "Opening %s\xE2\x80\xA6", a->title);
    tex_free(&S.launch_tex);
    S.launch_tex = text_render(S.f_medium, 26, msg, TILE_W + 60, &S.launch_w, &S.launch_h);
    S.launching = 1;
    S.launch_item = S.focus;
    S.launch_at = SDL_GetTicks();
}

static void move_focus(int dx, int dy)
{
    if (S.zone == ZONE_TOP) {
        if (dy > 0) { /* down: back to the app it came from */
            S.zone = ZONE_GRID;
            S.focus = top_down_tile();
        }
        return;
    }
    if (S.zone == ZONE_RAIL) { /* stepping through the carousel's titles */
        if (dy > 0) S.zone = ZONE_GRID; /* back to its app */
        else if (dy < 0) enter_top(S.focus % COLS);
        else if (dx && S.rail_index + dx >= 0 && S.rail_index + dx < S.nrail) rail_focus(S.rail_index + dx);
        return;
    }
    int n = S.nitems, i = S.focus, next = i;
    if (dx) {
        next = i + dx;
        if (next < 0 || next >= n || next / COLS != i / COLS) return;
    }
    if (dy > 0) {
        next = i + COLS;
        if (next >= n) {
            if ((n - 1) / COLS > i / COLS) next = n - 1;
            else return;
        }
    }
    if (dy < 0) {
        if (i < COLS) {
            /* up from the top row: into its Top Shelf carousel (the chevron), else the clock */
            if (!S.moving) {
                if (rail_active()) S.zone = ZONE_RAIL;
                else enter_top(i);
            }
            return;
        }
        next = i - COLS;
    }
    if (S.moving) {
        if (S.items[next] >= 0) move_item_to(next);
        return;
    }
    S.focus = next;
}

static void go_home(void)
{
    sheet_close();
    sw_reset();
    if (S.moving) { S.moving = 0; save_layout(); }
    S.zone = 0;
    S.focus = 0;
}

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

/* LG's SDL 2.0.14 reports remote keys with the *scancode* number in keysym.sym
 * (Right arrow arrives as sym=79 = SDL_SCANCODE_RIGHT). Map those back to keycodes;
 * real keycodes (from a USB keyboard or the desktop build) pass through, and so do
 * the colour keys, which have no SDL keycode. */
static SDL_Keycode remote_key(const SDL_Keysym *ks)
{
    switch (ks->sym) {
    case SDL_SCANCODE_RIGHT: return SDLK_RIGHT;
    case SDL_SCANCODE_LEFT: return SDLK_LEFT;
    case SDL_SCANCODE_DOWN: return SDLK_DOWN;
    case SDL_SCANCODE_UP: return SDLK_UP;
    case SDL_SCANCODE_RETURN: return SDLK_RETURN;
    case SDL_SCANCODE_KP_ENTER: return SDLK_KP_ENTER;
    case SDL_SCANCODE_ESCAPE: return SDLK_ESCAPE;
    case SDL_SCANCODE_BACKSPACE: return SDLK_BACKSPACE;
    case SDL_SCANCODE_AC_BACK: return SDLK_AC_BACK;
    case WEBOS_KEY_BACK: return SDLK_AC_BACK;
    default: return ks->sym;
    }
}

static int is_back(SDL_Keycode k)
{
    return k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE;
}

/* keys Home+ acts on; anything else gets logged so it can be mapped */
static int is_mapped(SDL_Keycode k)
{
    switch (k) {
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_LEFT: case SDLK_RIGHT: case SDLK_UP: case SDLK_DOWN:
    case WEBOS_KEY_RED: case WEBOS_KEY_GREEN: case WEBOS_KEY_YELLOW: case WEBOS_KEY_BLUE:
        return 1;
    default:
        return is_back(k);
    }
}

/* Left/Right pick an app, Up closes it (its card flies away), Down reaches Close All */
static void sw_key(SDL_Keycode k)
{
    if (S.recents.zooming) return;
    if (is_back(k) || k == WEBOS_KEY_RED) {
        S.recents.open = 0; /* animates out; sw_update() clears it */
        return;
    }
    if (!S.recents.n) return;
    if (S.recents.close_all) {
        if (k == SDLK_UP) S.recents.close_all = 0;
        return;
    }
    if (k == SDLK_LEFT && S.recents.index > 0) S.recents.index--;
    else if (k == SDLK_RIGHT && S.recents.index < S.recents.n - 1) S.recents.index++;
    else if (k == SDLK_DOWN) S.recents.close_all = 1;
    else if (k == SDLK_UP && !S.recents.c[S.recents.index].flying) {
        S.recents.c[S.recents.index].flying = 1;
        close_apps(S.recents.c[S.recents.index].id);
    }
}

#define PTR_TAKE_PX 40 /* pointer travel (logical px) that hands control back to it */
static int ptr_lx = -1, ptr_ly = -1; /* the pointer's last position */

static void keys_take_over(void)
{
    S.ptr_mode = 0;
    S.ptr_ax = ptr_lx;
    S.ptr_ay = ptr_ly;
}

/* An arrow key hides the Magic Remote's pointer; moving the remote brings it back. LG's
 * SDL has SDL_webOSCursorVisibility (its SDL_webOS.h): looked up at run time, since the
 * vendored 2.0.14 headers and desktop SDL don't have it. */
static void hide_pointer(void)
{
    static SDL_bool (*visibility)(SDL_bool);
    static int looked;
    if (!looked) {
        void *self = dlopen(NULL, RTLD_LAZY);
        looked = 1;
        if (self) *(void **)&visibility = dlsym(self, "SDL_webOSCursorVisibility");
    }
    if (visibility && S.cursor_up) visibility(SDL_FALSE);
    S.cursor_up = 0;
}

/* the pointer moved to mx,my: 1 when it's (now) the one in charge */
static int pointer_moved(int mx, int my)
{
    ptr_lx = mx;
    ptr_ly = my;
    S.cursor_up = 1;
    if (S.ptr_mode) return 1;
    if (S.ptr_ax < 0) { /* first sighting since the keys took over: just remember it */
        S.ptr_ax = mx;
        S.ptr_ay = my;
        return 0;
    }
    int dx = mx - S.ptr_ax, dy = my - S.ptr_ay;
    if (dx * dx + dy * dy < PTR_TAKE_PX * PTR_TAKE_PX) return 0; /* a hand's tremor */
    S.ptr_mode = 1;
    return 1;
}

static void on_key_down(SDL_KeyboardEvent *e)
{
    SDL_Keycode k = remote_key(&e->keysym);
    if (k == WEBOS_KEY_CURSOR_SHOW || k == WEBOS_KEY_CURSOR_HIDE) {
        S.cursor_up = k == WEBOS_KEY_CURSOR_SHOW;
        return;
    }
    if (k != SDLK_RETURN && k != SDLK_KP_ENTER) keys_take_over();
    if (k == SDLK_LEFT || k == SDLK_RIGHT || k == SDLK_UP || k == SDLK_DOWN) {
        S.tick_armed = 1;
        hide_pointer();
    }
    if (S.logged_keys < 40 && !is_mapped(k)) {
        S.logged_keys++;
        S.last_unknown_key = (int)k;
        log_remote("unknown key sym=%d scancode=%d mod=%d", (int)k, (int)e->keysym.scancode, (int)e->keysym.mod);
    }
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
        if (!e->repeat && !S.ok_down) {
            S.ok_down = SDL_GetTicks();
            S.ok_long = 0;
        }
        return;
    }
    if (S.recents.open) {
        sw_key(k);
        return;
    }
    if (S.sheet.open) {
        if (k == SDLK_UP && S.sheet.index > 0) S.sheet.index--;
        else if (k == SDLK_DOWN && S.sheet.index < S.sheet.n - 1) S.sheet.index++;
        else if ((k == SDLK_LEFT || k == SDLK_RIGHT) && S.sheet.it[S.sheet.index].swatches) {
            int i = S.icon_tint + (k == SDLK_RIGHT ? 1 : -1);
            if (i >= 0 && i < NTINTS) icon_tint_set(i), snd_play(SND_TICK);
        } else if (is_back(k) && !S.sheet.locked) {
            if (S.sheet.parent) settings_page(S.sheet.parent, S.sheet.parent_index);
            else sheet_dismiss();
        }
        return;
    }
    if (S.cc.open) {
        if (S.cc.list) cc_list_key(k);
        else if (k == SDLK_LEFT) cc_move(-1, 0);
        else if (k == SDLK_RIGHT) cc_move(1, 0);
        else if (k == SDLK_UP) cc_move(0, -1);
        else if (k == SDLK_DOWN) cc_move(0, 1);
        else if (is_back(k)) {
            if (S.cc.picked >= 0) { /* put it down */
                S.cc.picked = -1;
                cc_save();
            } else if (S.cc.editing) { /* done (every change is already saved) */
                S.cc.editing = 0;
                cc_pack();
                if (S.cc.index >= S.cc.n) S.cc.index = S.cc.n ? S.cc.n - 1 : 0;
            } else {
                cc_close();
            }
        }
        return;
    }
    switch (k) {
    case SDLK_LEFT: move_focus(-1, 0); break;
    case SDLK_RIGHT: move_focus(1, 0); break;
    case SDLK_UP: move_focus(0, -1); break;
    case SDLK_DOWN: move_focus(0, 1); break;
    /* colour keys: Recents, Settings, the focused app's options, move it (again: done) */
    case WEBOS_KEY_RED: if (!S.moving) sw_open(); break;
    case WEBOS_KEY_GREEN: if (!S.moving) open_settings_sheet(); break;
    case WEBOS_KEY_YELLOW: if (!S.moving) open_app_sheet(); break;
    case WEBOS_KEY_BLUE:
        if (S.moving) { S.moving = 0; save_layout(); }
        else if (S.zone == ZONE_GRID && item_app(S.focus)) S.moving = 1;
        break;
    default:
        if (is_back(k)) {
            if (S.zone != ZONE_GRID) S.zone = ZONE_GRID;
            else if (S.moving) { S.moving = 0; save_layout(); }
            else if (S.focus) S.focus = 0;
            else sw_open(); /* already home: Back shows what's open */
        }
    }
}

/* OK let go (the remote's key, or the pointer's click): select what has focus. */
static void ok_release(void)
{
    int was_long = S.ok_long;
    S.ok_down = 0;
    if (was_long) return;
    snd_play(SND_SELECT);
    if (S.recents.open) {
        sw_ok();
    } else if (S.sheet.open) {
        if (!S.sheet.n) return; /* "Setting up…": nothing to press */
        SheetItem it = S.sheet.it[S.sheet.index];
        S.at_page = S.sheet.page; /* a Settings action rebuilds its page in place */
        S.at_index = S.sheet.index;
        S.at_parent_index = S.sheet.parent_index;
        if (!S.at_page || it.act == ACT_CANCEL || it.act == ACT_LAUNCH_ID) sheet_dismiss(); /* else its page is rebuilt */
        if (it.act == ACT_LAUNCH_ID) launch(it.id, NULL);
        else run_action(it.act, it.arg, it.id);
    } else if (S.cc.open) {
        cc_activate();
    } else {
        activate();
    }
}

static void on_key_up(SDL_KeyboardEvent *e)
{
    SDL_Keycode k = remote_key(&e->keysym);
    if ((k != SDLK_RETURN && k != SDLK_KP_ENTER) || !S.ok_down) return;
    ok_release();
}

static int hit_item(int mx, int my)
{
    for (int i = 0; i < S.nitems; i++) {
        float x = LEFT + (i % COLS) * (TILE_W + GAP_X);
        float y = GRID_TOP + (i / COLS) * ROW_H + S.scroll;
        if (mx >= x && mx < x + TILE_W && my >= y && my < y + TILE_H) return i;
    }
    return -1;
}


/* A sheet's button under the pointer (-1: none); *outside: not on the panel at all.
 * Mirrors draw_sheet()'s layout, from where the panel was last drawn. */
static int sheet_button_at(int mx, int my, int *outside)
{
    *outside = mx < S.sheet.px || mx > S.sheet.px + S.sheet.pw || my < S.sheet.py || my > S.sheet.py + S.sheet.ph;
    if (*outside || !S.sheet.n) return -1;
    int nfoot = sheet_footer(), nlist = S.sheet.n - nfoot, rows = sheet_rows(nfoot);
    float step = SHEET_BH + SHEET_GAP, y0 = S.sheet.py + sheet_head(), list_h = rows * step;
    if (mx < S.sheet.px + PAD || mx > S.sheet.px + S.sheet.pw - PAD) return -1;
    if (my >= y0 && my < y0 + list_h) {
        float r = (my - y0) / step + S.sheet.scroll;
        int i = (int)floorf(r);
        if (r - i <= SHEET_BH / step && i >= 0 && i < nlist) return i;
        return -1;
    }
    for (int k = 0; k < nfoot; k++) {
        float fy = y0 + list_h + k * step;
        if (my >= fy && my < fy + SHEET_BH) return nlist + k;
    }
    return -1;
}

/* What's under the pointer; with apply, it gets focus (as if the remote moved
 * there). Returns 1 when it's something OK would act on. */
static int pointer_at(int mx, int my, int apply)
{
    if (S.recents.open) {
        if (S.recents.zooming || !S.recents.n) return 0;
        float bx = (W - 360) / 2.0f, by = H - SAFE_Y - SHEET_BH; /* Close All */
        if (mx >= bx && mx < bx + 360 && my >= by && my < by + SHEET_BH) {
            if (apply) S.recents.close_all = 1;
            return 1;
        }
        for (int i = 0; i < S.recents.n; i++) {
            float cx = W / 2.0f + S.recents.c[i].x * SW_PITCH;
            if (fabsf(mx - cx) < SW_W * 0.42f && fabsf((float)(my - SW_CY)) < SW_H * 0.42f) {
                if (apply) S.recents.index = i, S.recents.close_all = 0;
                return 1;
            }
        }
        return 0;
    }
    if (S.sheet.open) {
        int outside, i = sheet_button_at(mx, my, &outside);
        if (i >= 0 && apply) S.sheet.index = i;
        return i >= 0;
    }
    if (S.cc.open) {
        for (int i = 0; i < S.cc.n + (S.cc.editing ? 2 : 0) && !S.cc.list && S.cc.picked < 0; i++) {
            float x, y, w, h;
            if (!cc_rect(i, &x, &y, &w, &h)) continue;
            if (mx >= x && mx < x + w && my >= y && my < y + h) {
                if (apply && S.cc.index != i) S.cc.index = i, cc_problem(NULL);
                return 1;
            }
        }
        return 0;
    }
    if (S.moving) return 1; /* OK anywhere puts the moved app down */
    if (S.scrolled < 0.5f) { /* the top bar and Continue Watching are only there at the top */
        if (mx >= top_x() && mx < top_x() + top_w() && my >= TOP_Y && my < TOP_Y + BAR) {
            if (apply && S.zone != ZONE_TOP) enter_top(S.focus % COLS);
            return 1;
        }
    }
    int h = hit_item(mx, my);
    if (h < 0) return 0;
    if (apply) {
        S.focus = h;
        S.zone = ZONE_GRID;
        /* the light comes from where the pointer is, relative to the tile */
        float dx = mx - (LEFT + (h % COLS) * (TILE_W + GAP_X) + TILE_W / 2.0f);
        float dy = my - (GRID_TOP + (h / COLS) * ROW_H + S.scroll + TILE_H / 2.0f);
        float l = sqrtf(dx * dx + dy * dy);
        if (l > 8) {
            S.ptr_x = dx / l;
            S.ptr_y = dy / l;
            S.ptr_at = SDL_GetTicks();
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ animation */

/* OLED protection: after a while without input the screen dims (all but the
 * focused tile, like tvOS), then fades to black and stops drawing. */

static float special_s, special_v; /* focus animation of the "Hidden" tile */

static void update_clock(void)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    int m = t->tm_hour * 60 + t->tm_min;
    if (m == S.clock_min && S.clock_tex) return;
    char buf[16];
    int h12 = t->tm_hour % 12 ? t->tm_hour % 12 : 12;
    if (S.clock24) snprintf(buf, sizeof buf, "%02d:%02d", t->tm_hour, t->tm_min);
    else snprintf(buf, sizeof buf, "%d:%02d %s", h12, t->tm_min, t->tm_hour < 12 ? "AM" : "PM");
    tex_free(&S.clock_tex);
    S.clock_tex = text_render(S.f_semi, HEADING_PX, buf, 0, &S.clock_w, &S.clock_h);
    S.clock_min = m;
}

/* the app list hasn't come yet (and Home+ isn't stuck on an error) */
static int apps_loading(void) { return !S.nitems && (S.setup <= SETUP_RESTARTING || S.setup == SETUP_DONE); }

/* app a's carousel is on the way: its Continue Watching, or what's popular on it */
static int carousel_coming(const App *a)
{
    int src = media_source(a);
    if (src) return S.rail_req == src;
    return S.tmdb_configured && S.service_ok && (a->feat <= 1 || (a->feat == 2 && !rail_for(a)));
}

/* Whether the Top Shelf can show the page (item, rail) yet: a title once its picture and
 * logo are in (or failed, or it has none); an app's own page once no carousel is on the
 * way for it and its artwork isn't loading. */
static int page_ready(int item, int rail)
{
    App *a = item_app(item);
    if (!a) return 1;
    if (rail >= 0) {
        const RailItem *r = &S.rail[rail];
        return (r->bg_state >= 2 || !r->bg[0]) && (r->logo_state >= 2 || !r->logo[0]);
    }
    return !carousel_coming(a) && !(a->art[0] && a->art_state < 2);
}

/* a page's picture: its title's, else its app's artwork, else the app's glow (with its
 * tile large on its own page; the glow alone behind a title). Once another carousel has
 * taken its place, the picture on show stays. */
static Pic page_pic(int item, int rail)
{
    App *a = item_app(item);
    int mine = a && rail_for(a);
    if (rail == -2 || (rail >= 0 && !mine)) return S.pic_top;
    if (rail >= 0 && rail < S.nrail && S.rail[rail].bg_tex) return (Pic){ PIC_RAIL, rail };
    if (!a) return (Pic){ PIC_NONE, 0 };
    return (Pic){ a->art_tex ? PIC_ART : rail == -1 ? PIC_GLOW : PIC_COVER, (int)(a - S.apps) };
}

/* The Top Shelf's next page shows once it's ready, or after PAGE_WAIT_MS; meanwhile the
 * page on show stays and its text leaves. Its picture fades in over the one on show. */
static void update_page(float dt, int *anim)
{
    App *a = item_app(S.shelf_cur);
    int wi = S.shelf_cur, wr = a && rail_for(a) ? S.rail_index : -1;
    Uint32 now = SDL_GetTicks();
    /* the wait starts over for another app, or another title (stepping through them),
     * not when the app's carousel comes in */
    if (wi != S.want_app || (wr >= 0 && S.want_rail >= 0 && wr != S.want_rail)) S.want_at = now;
    S.want_app = wi, S.want_rail = wr;
    int on = wi == S.page_app && wr == S.page_rail;
    if (!on && (page_ready(wi, wr) || now - S.want_at >= PAGE_WAIT_MS)) {
        S.page_step = wr >= 0 && S.page_rail >= 0 && wi == S.page_app; /* the next title, same carousel */
        S.prev_app = S.page_app, S.prev_rail = S.page_rail, S.prev_vis = 1 - S.page_leave;
        S.page_app = wi, S.page_rail = wr;
        S.page_a = S.page_leave = 0;
        on = 1;
    }
    approach(&S.page_leave, on ? 0 : 1, 10, dt, anim);
    approach(&S.page_a, 1, 9, dt, anim);
    /* a carousel's page that takes a while: placeholders stand in for its text */
    int slow = !on && now - S.want_at >= SKEL_MS && (wr >= 0 || (a && carousel_coming(a)));
    approach(&S.skel_a, slow ? 1 : 0, 8, dt, anim);
    if (S.skel_a > 0.004f && !S.reduce_motion) *anim = 1; /* their shimmer */
    Pic want = page_pic(S.page_app, S.page_rail);
    if (!pic_eq(want, S.pic_top)) {
        if (S.pic_a >= 0.5f) { /* mostly in: it's the one underneath now */
            pic_drop(&S.pic_under);
            S.pic_under = S.pic_top;
        } else {
            pic_drop(&S.pic_top);
        }
        S.pic_top = want;
        S.pic_a = 0;
    }
    approach(&S.pic_a, 1, 5, dt, anim);
    if (S.pic_a >= 1) pic_drop(&S.pic_under); /* covered */
}

/* Advances every animated value once per frame. Drawing only reads them: the
 * scene is drawn twice, into the glass backdrop and then to the screen. */
static int update(float dt)
{
    int anim = 0;
    update_clock();
    int row = S.focus / COLS;
    approach(&S.scrolled, row > 0 ? 1.0f : 0.0f, 10, dt, &anim);
    S.scroll_target = row == 0 ? 0 : 380 - (GRID_TOP + row * ROW_H);
    if (row > 0) {
        /* don't scroll past the point where the last row (and its label) sits at the
         * bottom of the safe area: a short last stretch would otherwise stop mid-screen */
        int rows = (S.nitems + COLS - 1) / COLS;
        float lowest = (H - SAFE_Y) - (GRID_TOP + (rows - 1) * ROW_H + TILE_H + 60);
        if (S.scroll_target < lowest) S.scroll_target = lowest;
    }
    spring(&S.scroll, &S.scroll_v, S.scroll_target, 0.5f, 1.0f, dt, &anim);

    /* the Top Shelf belongs to the last top-row app focused, also while it shows
     * Continue Watching: scrolled below the dock, its picture stays behind, blurred */
    if (S.focus < COLS) S.shelf_cur = S.focus;
    /* the chevron shows while Up from the focused app goes into its Continue Watching */
    int chev = S.zone == ZONE_GRID && S.focus < COLS && rail_active() && !S.sheet.open && !S.recents.open && !S.moving;
    approach(&S.chevron_a, chev ? 1.0f : 0.0f, 8, dt, &anim);
    int want = want_source();
    if (want && want != SRC_FEATURED && want != S.rail_source && want != S.rail_req && S.service_ok) rail_request(want);
    App *sa = shelf_app();
    if (sa && !media_source(sa) && S.tmdb_configured && S.service_ok && S.feat_req < 0 &&
        (sa->feat == 0 || (sa->feat == 2 && !rail_for(sa))))
        featured_request(sa); /* a streaming service? (the service keeps its list) */
    if (rail_active()) {
        Uint32 now = SDL_GetTicks();
        int nx = (S.rail_index + 1) % S.nrail;
        /* it turns every 10 s while its app has focus, nothing covers it and the screen is awake
         * (unless Settings says it stays still) */
        int turning = !S.shelf_still && S.zone == ZONE_GRID && S.nrail > 1 && !S.sheet.open && !S.recents.open && !S.moving &&
                      S.idle_stage == 0;
        if (!turning) S.rail_next = 0;
        else if (!S.rail_next) S.rail_next = now + ROTATE_MS;
        else if (now >= S.rail_next && S.rail[nx].bg_state != 1 && S.rail[nx].logo_state != 1) { /* the next is ready */
            rail_focus(nx);
            nx = (S.rail_index + 1) % S.nrail;
            S.rail_next = now + ROTATE_MS;
        }
        /* pictures: the selected title's first, then both of its neighbours, loaded ahead so
         * turning or stepping through never waits; and those still on show. The rest are let
         * go (~8 MB each on this GPU), with their requests that haven't started. */
        int pv = (S.rail_index + S.nrail - 1) % S.nrail, order[3] = { S.rail_index, nx, pv };
        RailItem *sel = &S.rail[S.rail_index];
        if (sel->logo_state == 1) net_bump(TAG_RAIL_LOGO, S.rail_index, S.rail_generation); /* asked for as a neighbour */
        if (sel->bg_state == 1) net_bump(TAG_RAIL_BG, S.rail_index, S.rail_generation);
        for (int k = 0; k < 3; k++) {
            RailItem *it = &S.rail[order[k]];
            if (it->bg[0] && it->bg_state == 0) {
                it->bg_state = 1;
                net_get_image(it->bg, TAG_RAIL_BG, order[k], S.rail_generation, 0, W, H);
            }
            if (it->logo[0] && it->logo_state == 0) {
                it->logo_state = 1;
                net_get_image(it->logo, TAG_RAIL_LOGO, order[k], S.rail_generation, 0, 640, 240);
            }
        }
        for (int i = 0; i < S.nrail; i++) {
            if (i == S.rail_index || i == nx || i == pv || i == S.page_rail || i == S.prev_rail || pic_rail(i)) continue;
            RailItem *it = &S.rail[i];
            if (it->bg_state == 1 && net_drop(TAG_RAIL_BG, i, S.rail_generation)) it->bg_state = 0;
            if (it->logo_state == 1 && net_drop(TAG_RAIL_LOGO, i, S.rail_generation)) it->logo_state = 0;
            if (it->bg_tex) tex_free(&it->bg_tex), it->bg_state = 0;
            if (it->logo_tex) tex_free(&it->logo_tex), it->logo_state = 0;
            tex_free(&it->title_tex);
            tex_free(&it->meta_tex);
            it->texts_made = 0;
        }
    } else {
        App *ca = item_app(S.shelf_cur);
        if (ca && ca->art[0] && ca->art_state == 0 && !rail_for(ca) && !carousel_coming(ca)) { /* its own page */
            ca->art_state = 1;
            net_get_image(ca->art, TAG_ART, S.items[S.shelf_cur], S.generation, 0, W, H);
        }
    }
    update_page(dt, &anim);
    if (!S.reduce_motion && !S.offscreen && apps_loading()) anim = 1; /* placeholders shimmer */
    if (!S.reduce_motion && !S.offscreen) /* so do tiles while their icons load */
        for (int i = 0; i < S.nitems && !anim; i++) {
            App *a = item_app(i);
            if (a && a->icon_state == 1) anim = 1;
        }
    /* while OK is down the focused thing sinks a little (1.06), then pops back */
    int pressed = S.ok_down && !S.ok_long && !S.sheet.open && !S.recents.open;
    float on = pressed ? 0.6f : 1.0f, resp = pressed ? 0.12f : 0.35f, damp = pressed ? 1.0f : 0.7f;
    for (int i = 0; i < S.nitems; i++) {
        App *a = item_app(i);
        int f = i == S.focus && S.zone == ZONE_GRID;
        spring(a ? &a->s : &special_s, a ? &a->sv : &special_v, f ? on : 0.0f, f ? resp : 0.35f, f ? damp : 0.7f, dt, &anim);
    }
    for (int i = 0; i < TOP_COUNT; i++) {
        int f = S.zone == ZONE_TOP && S.top_index == i && !S.cc.open; /* open, focus is in the Control Center */
        spring(&S.top_s[i], &S.top_v[i], f ? on : 0.0f, f ? resp : 0.35f, f ? damp : 0.7f, dt, &anim);
    }
    spring(&S.cc.t, &S.cc.tv, S.cc.open ? 1.0f : 0.0f, S.reduce_motion ? 0.15f : 0.32f, 0.9f, dt, &anim);
    if (S.cc.marquee && S.cc.open) anim = 1; /* a long name is scrolling */
    Uint32 tick = SDL_GetTicks();
    for (int i = 0; i < S.cc.n; i++) {
        CCItem *c = &S.cc.it[i];
        int f = S.cc.open && S.cc.index == i && !S.cc.list;
        spring(&c->s, &c->sv, f ? on : 0.0f, f ? resp : 0.35f, f ? damp : 0.7f, dt, &anim);
        if ((c->shake && tick - c->shake < 450) || (c->flash && tick - c->flash < 1800)) anim = 1; /* a shake; a value showing */
    }
    spring(&S.cc.plus_s, &S.cc.plus_v, S.cc.editing && S.cc.index == S.cc.n ? on : 0.0f, 0.35f, 0.7f, dt, &anim);
    spring(&S.cc.reset_s, &S.cc.reset_v, S.cc.editing && S.cc.index == S.cc.n + 1 ? on : 0.0f, 0.35f, 0.7f, dt, &anim);
    spring(&S.cc.list_t, &S.cc.list_v, S.cc.list ? 1.0f : 0.0f, S.reduce_motion ? 0.15f : 0.3f, 0.9f, dt, &anim);
    if (S.cc.open && S.cc.editing && !S.reduce_motion) anim = 1; /* they wobble */
    if (pressed) anim = 1; /* the hold bar fills */
    if (S.launching) {
        Uint32 held = SDL_GetTicks() - S.launch_at;
        if (held < 2500) anim = 1;
        if (held > 10000) {
            char msg[200];
            App *la = item_app(S.launch_item);
            snprintf(msg, sizeof msg, "%s is taking a while to open", la ? la->title : "The app");
            toast(msg, 3500);
            S.launching = 0;
        }
    }
    if (S.moving && !S.reduce_motion) anim = 1; /* wobble */
    if (S.sheet.open) {
        spring(&S.sheet.t, &S.sheet.tv, 1, 0.42f, 0.82f, dt, &anim);
        int nfoot = sheet_footer(), nlist = S.sheet.n - nfoot, rows = sheet_rows(nfoot);
        if (S.sheet.index < nlist) S.sheet.first = keep_in_view(S.sheet.first, S.sheet.index, rows, nlist, rows > 4);
        if (S.sheet.t < 0.05f) S.sheet.scroll = (float)S.sheet.first; /* opens already in place */
        spring(&S.sheet.scroll, &S.sheet.scroll_v, (float)S.sheet.first, 0.5f, 1.0f, dt, &anim);
    }
    sw_update(dt, &anim);
    if (S.ghost.on) {
        spring(&S.ghost.t, &S.ghost.tv, 0, 0.32f, 1.0f, dt, &anim);
        if (S.ghost.t <= 0.002f) S.ghost.on = 0;
    }
    spring(&S.toast_t, &S.toast_v, toast_on() ? 1.0f : 0.0f, 0.4f, 0.85f, dt, &anim);

    /* when focus lands the element sways for a moment and its highlight follows
     * a moving light (from the pointer if it moved lately), then it settles */
    int layer = S.recents.open ? 2 : S.sheet.open ? 1 : 0; /* home, a sheet, the App Switcher */
    int key = layer == 2 ? 2000000 + S.recents.index * 2 + S.recents.close_all
            : layer == 1 ? 1000000 + S.sheet.index
                         : S.zone * 100000 + S.focus * 1000 + S.top_index * 100 + S.rail_index;
    if (key != S.focus_key) {
        /* a tick when a key moved focus: not when a layer opens or closes, nor
         * when the pointer or a reset moves it */
        if (S.tick_armed && layer == S.focus_key / 1000000) snd_play(SND_TICK);
        S.focus_key = key;
        S.sway_t = 0;
    }
    S.tick_armed = 0;
    float bx = -0.6f, by = -0.8f;
    if (S.ptr_at && SDL_GetTicks() - S.ptr_at < 3000) {
        bx = S.ptr_x;
        by = S.ptr_y;
    }
    float amp = 0;
    if (S.reduce_motion) S.sway_t = 1.6f; /* no sway, the light stays put */
    if (S.sway_t < 1.6f) {
        S.sway_t += dt;
        amp = 1 - S.sway_t / 1.6f;
        amp *= amp;
        anim = 1;
    }
    float ang = 0.6f * amp * sinf(S.sway_t * 2 * (float)M_PI / 1.1f);
    S.light_x = bx * cosf(ang) - by * sinf(ang);
    S.light_y = bx * sinf(ang) + by * cosf(ang);

    float dim = S.idle_stage >= 1 ? 1.0f : 0.0f, black = S.idle_stage >= 2 ? 1.0f : 0.0f;
    approach(&S.dim, dim, dim > S.dim ? 2.5f : 10.0f, dt, &anim);
    approach(&S.black, black, black > S.black ? 2.5f : 10.0f, dt, &anim);
    return anim;
}

/* ------------------------------------------------------------------ drawing */

static void draw_text(Tex *t, int w, int h, float x, float y, Uint8 a)
{
    r_image(t, x, y, (float)w, (float)h, 255, 255, 255, a, 0);
}

/* offset of the focused element while it sways after focus lands */
static void sway(float *dx, float *dy)
{
    float amp = S.sway_t < 1.6f ? 1 - S.sway_t / 1.6f : 0;
    amp *= amp;
    *dx = 3.0f * amp * sinf(S.sway_t * 2 * (float)M_PI / 1.1f);
    *dy = 1.5f * amp * sinf(S.sway_t * 2 * (float)M_PI / 0.9f + 1);
}

/* Liquid Glass (render.h): clear unless what's on it must stay legible. In the
 * backdrop pass glass can't sample itself, so a faint flat panel stands in. */
/* Glass, or with Reduce Transparency a solid dark panel (and a light wash when focused). */
static void glass(const Glass *g)
{
    if (!S.reduce_transparency) {
        r_glass(g);
        return;
    }
    float a = g->alpha * (g->lens < 1 ? g->lens : 1);
    r_shape(g->x, g->y, g->w, g->h, g->radius, 38, 38, 44, (Uint8)(245 * a), 0, 1, 1);
    if (g->focus > 0.01f) r_shape(g->x, g->y, g->w, g->h, g->radius, 255, 255, 255, (Uint8)(60 * g->focus * a), 0, 1, 1);
    r_shape(g->x, g->y, g->w, g->h, g->radius, 255, 255, 255, (Uint8)(50 * a), 1.5f, 1, 1); /* a hairline edge */
}

static void panel(int backdrop, float x, float y, float w, float h, float radius, float legible, float focus, float alpha)
{
    if (alpha <= 0.004f) return;
    if (backdrop) {
        r_shape(x, y, w, h, radius, 255, 255, 255, (Uint8)(14 * alpha), 0, 1, 1);
        return;
    }
    int lit = focus > 0.01f; /* focused glass catches the moving light */
    Glass g = { x, y, w, h, radius, legible, 1.0f, lit ? S.light_x : -0.6f, lit ? S.light_y : -0.8f, focus, alpha, 0, 0, 0, 0 };
    glass(&g);
}

/* text is made when first shown: a big title is ~0.4 MB of GPU memory */
static void ensure_label(App *a)
{
    if (a->label) return;
    char name[ID_LEN];
    short_title(a, name, sizeof name);
    a->label = text_render(S.f_medium, 26, name, TILE_W + 60, &a->lw, &a->lh);
}

static void ensure_big(App *a)
{
    if (a->big) return;
    char name[ID_LEN];
    short_title(a, name, sizeof name);
    a->big = text_render(S.f_bold, 84, name, 1000, &a->bw, &a->bh);
}

/* a colour light enough to read on a dark tile: luminance 0.55 at least */
static void lift(const Uint8 *c, Uint8 *out)
{
    float l = (0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]) / 255, k = l < 0.55f ? 0.55f / fmaxf(l, 0.05f) : 1, v[3];
    for (int i = 0; i < 3; i++) v[i] = fminf(255, c[i] * k);
    float l2 = (0.299f * v[0] + 0.587f * v[1] + 0.114f * v[2]) / 255, w = l2 < 0.55f ? (0.55f - l2) / (1 - l2) : 0;
    for (int i = 0; i < 3; i++) out[i] = (Uint8)(v[i] + (255 - v[i]) * w + 0.5f); /* blue can't get there alone */
}

/* An app's tile and logo in the look Settings picks (Display & OLED -> App Icons): the icon's own
 * colours; a dark tile with the logo in colour (a light icon's dark ink turned light, a white logo
 * on a coloured icon in that colour); or frosted glass with the logo in white (Clear) or, on glass
 * in a colour, in a pale tint of it (Tinted). lk: the logo's scale. 0 when there's no logo (its name or a placeholder
 * goes there). In the backdrop pass glass can't sample itself: a faint flat tile stands in. */
static int draw_face(App *a, float x, float y, float w, float h, float rad, float lk, Uint8 alpha, float focus, int backdrop)
{
    int look = S.icon_look, glassy = look == LOOK_CLEAR || look == LOOK_TINTED;
    const Uint8 *t = TINTS[S.icon_tint].rgb;
    float f = fminf(fmaxf(focus, 0), 1);
    Uint8 r, g, b;
    app_color(a, &r, &g, &b);
    if (look == LOOK_DARK) r = g = 14, b = 16; /* near black: the hairline keeps its shape */
    if (glassy && backdrop) {
        r_shape(x, y, w, h, rad, 255, 255, 255, (Uint8)(24 * alpha / 255), 0, 1, 1);
    } else if (glassy) {
        /* a dark tint keeps a white logo readable over a bright picture; focused, the glass lights up */
        int lit = focus > 0.01f, tinted = look == LOOK_TINTED;
        float k = 0.55f + 0.15f * f;
        Glass gl = { x, y, w, h, rad, 0.7f, 1.0f, lit ? S.light_x : -0.6f, lit ? S.light_y : -0.8f, focus, alpha / 255.0f,
                     (Uint8)(tinted ? t[0] * k : 0), (Uint8)(tinted ? t[1] * k : 0), (Uint8)(tinted ? t[2] * k : 0),
                     (Uint8)(tinted ? 150 : 46 - 26 * f) };
        glass(&gl);
    } else {
        r_shape(x, y, w, h, rad, r, g, b, alpha, 0, 1, 1);
        if (0.299f * r + 0.587f * g + 0.114f * b < 32) /* dark tiles keep their shape */
            r_shape(x, y, w, h, rad, 255, 255, 255, (Uint8)(26 * alpha / 255), 1.5f, 1, 1);
    }
    if (!a->logo_tex) return 0;
    if (glassy && backdrop) return 1; /* glass would refract its own logo, as a halo */
    float lw = a->logo_w * lk, lh = a->logo_h * lk, lx = x + (w - lw) / 2, ly = y + (h - lh) / 2, lr = a->logo_r * lk;
    if (look == LOOK_DEFAULT || (look == LOOK_DARK && lr > 0)) { /* a picture keeps its colours */
        r_image(a->logo_tex, lx, ly, lw, lh, 255, 255, 255, alpha, lr);
    } else if (look == LOOK_DARK) {
        Uint8 c[3];
        lift(a->col, c);
        LogoLook l = { 0, a->ink_ref, 0, 0, a->ink_tint, c[0], c[1], c[2] };
        r_logo(a->logo_tex, lx, ly, lw, lh, lr, &l, alpha);
    } else {
        Uint8 c[3] = { 255, 255, 255 };
        if (look == LOOK_TINTED)
            for (int i = 0; i < 3; i++) c[i] = (Uint8)(t[i] + (255 - t[i]) * (0.6f + 0.4f * f)); /* a pale tint, white focused */
        LogoLook shade = { 1, a->ink_ref, a->ink_span, lr > 0 ? 0 : 0.45f, 0, 0, 0, 0 };
        LogoLook l = { 1, a->ink_ref, a->ink_span, lr > 0 ? 0 : 0.45f, 0, c[0], c[1], c[2] };
        r_logo(a->logo_tex, lx, ly + 2 * lk, lw, lh, lr, &shade, (Uint8)(alpha * 0.3f)); /* lifts it off the glass */
        r_logo(a->logo_tex, lx, ly, lw, lh, lr, &l, (Uint8)(alpha * (0.92f + 0.08f * f)));
    }
    return 1;
}

/* A placeholder while something loads (a skeleton): a soft bar that a slow shimmer passes over. */
static void draw_skeleton(float x, float y, float w, float h, float radius, float alpha)
{
    float wave = S.reduce_motion ? 0.5f : 0.5f + 0.5f * sinf(2 * (float)M_PI * (SDL_GetTicks() / 1400.0f - (x + 0.5f * y) / 1800.0f));
    r_shape(x, y, w, h, radius, 255, 255, 255, (Uint8)(255 * alpha * (0.08f + 0.08f * wave)), 0, 1, 1);
}

/* darkens where text sits: the bottom (dock), the left (titles), the top (top bar) */
static void draw_scrims(Uint8 alpha)
{
    r_image(S.scrim_v, 0, H * 0.55f, W, H * 0.45f, 255, 255, 255, alpha, 0);
    r_image(S.scrim_h, 0, 0, 1150, H, 255, 255, 255, alpha, 0);
    r_image(S.scrim_top, 0, 0, W, 280, 255, 255, 255, alpha, 0);
}

/* An app's tile drawn large: the Top Shelf's big tile and the App Switcher's
 * cards. Its logo grows with it (up to 3.5x); a tile without one shows its name. */
static void draw_big_tile(App *a, float x, float y, float w, float h, float radius, Uint8 alpha, int shadow, int backdrop)
{
    float k = fminf(fminf(w / TILE_W, h / TILE_H), 3.5f);
    if (shadow) r_shadow(x, y, w, h, radius, 40, 20, 0.45f * alpha / 255);
    if (draw_face(a, x, y, w, h, radius, k, alpha, 0, backdrop)) return;
    ensure_big(a);
    if (!a->big) return;
    float f = fminf(fminf(1, w * 0.8f / a->bw), k / 2.5f), bw = a->bw * f, bh = a->bh * f;
    r_image(a->big, x + (w - bw) / 2, y + (h - bh) / 2, bw, bh, 255, 255, 255, alpha, 0);
}

/* The Top Shelf is a picture (artwork, or the app's tile large on a glow, under
 * scrims) and content (titles, the Continue Watching rail). The picture stays
 * behind the grid when you scroll down, blurred; the content scrolls away. */

/* Apps without artwork also get their tile, large, on the right. */
static void draw_app_bigtile(App *a, Uint8 alpha, int backdrop)
{
    if (!a || !alpha || !a->logo_tex) return;
    /* it leaves with the shelf's content, so it never ghosts behind the grid */
    float leave = fmaxf(0, 1 - 2.5f * S.scrolled);
    if (leave <= 0) return;
    float tw = 520, th = tw * TILE_H / TILE_W, cc = 1 - fminf(fmaxf(S.cc.t, 0), 1); /* it steps aside for the Control Center */
    if (cc <= 0.004f) return;
    draw_big_tile(a, RIGHT - tw, SAFE_Y + 120, tw, th, TILE_R * tw / TILE_W, (Uint8)(alpha * leave * cc), 1, backdrop);
}

static void draw_app_text(App *a, Uint8 alpha, float dy)
{
    if (!a || !alpha) return;
    ensure_big(a);
    draw_text(a->big, a->bw, a->bh, LEFT - 4, 340 + dy, alpha);
    int first = a == item_app(0);
    Tex *sub = first ? S.sub_first_tex : S.sub_tex;
    int sw = first ? S.sfw : S.sw, sh = first ? S.sfh : S.sh, src = media_source(a);
    if (src && !S.src_connected[src]) { /* a media app: sign in from Settings */
        if (!S.connect_tex[src]) {
            char t[120];
            snprintf(t, sizeof t, "Connect %s in Settings \xE2\x86\x92 Accounts to see Continue Watching", SRC_NAME[src]);
            S.connect_tex[src] = text_render(S.f_medium, HINT_PX, t, 0, &S.connect_w[src], &S.connect_h[src]);
        }
        sub = S.connect_tex[src], sw = S.connect_w[src], sh = S.connect_h[src];
    }
    if (S.zone == ZONE_TOP) { /* while the top bar has focus, the hint is about it */
        sub = S.top_hint[S.top_index];
        sw = S.top_hw[S.top_index];
        sh = S.top_hh[S.top_index];
    }
    if (S.moving) return; /* moving it: the capsule at the top says what the keys do */
    draw_text(sub, sw, sh, LEFT, 340 + a->bh + 10 + dy,
              (Uint8)(TEXT_SECONDARY * alpha / 255 * (1 - fminf(fmaxf(S.cc.t, 0), 1)))); /* the Control Center says what OK does */
}

static void draw_pic(Pic p, Uint8 alpha)
{
    if (!alpha) return;
    App *a = p.kind == PIC_ART || p.kind == PIC_GLOW || p.kind == PIC_COVER ? &S.apps[p.i] : NULL;
    if (p.kind == PIC_ART) r_cover(a->art_tex, 0, 0, W, H, alpha, 0);
    else if (a && a->cover_tex) r_cover(a->cover_tex, 0, 0, W, H, alpha, 0);
    else if (a) {
        Uint8 r, g, b;
        app_color(a, &r, &g, &b);
        r_shape(0, 0, W, H, 0, r, g, b, (Uint8)(alpha * 0.25f), 0, 1, 1);
    } else if (p.kind == PIC_RAIL && p.i < S.nrail) r_cover(S.rail[p.i].bg_tex, 0, 0, W, H, alpha, 0);
    else if (p.kind == PIC_HELD) r_cover(S.pic_held[p.i], 0, 0, W, H, alpha, 0);
}

/* covers what's under it (a plain colour glow doesn't) */
static int pic_opaque(Pic p)
{
    return p.kind == PIC_ART || p.kind == PIC_RAIL || p.kind == PIC_HELD || ((p.kind == PIC_GLOW || p.kind == PIC_COVER) && S.apps[p.i].cover_tex);
}

/* The carousel's title, like tvOS's Top Shelf: a heading, the title's logo (or its
 * name), what it is, how far you are, where the app opens, and dots for where it
 * is among the others (brighter while you step through them). title: how far the
 * title itself is in (0..1), apart from the rest; skel: placeholders where the next
 * one goes, while it loads. */
static void draw_rail_content(RailItem *cur, Uint8 alpha, float title, float skel, int backdrop)
{
    if (!cur || !alpha) return;
    ensure_rail_texts(cur);
    /* the heading mirrors the clock: same height and top, on the dock's left edge as the
     * clock is on its right; the title sits low on the left, just above the chevron, like
     * the Apple TV app, so the picture's top stays clear */
    panel(backdrop, DOCK_X, TOP_Y, S.hw + 48, BAR, BAR / 2, 1, 0, alpha / 255.0f);
    draw_text(S.heading_tex, S.hw, S.hh, DOCK_X + 24, TOP_Y + (BAR - S.hh) / 2.0f, alpha);
    int logo = cur->logo_tex && cur->lw > 0 && cur->lh > 0, n = S.nrail < MAX_DOTS ? S.nrail : MAX_DOTS;
    float k = logo ? fminf(fminf(560.0f / cur->lw, 190.0f / cur->lh), 1.6f) : 0;
    float title_h = logo ? cur->lh * k + 18 : cur->th + 8;
    float total = title_h + cur->mh + 14 + (cur->progress > 0 ? 21 : 0) + S.rail_hint_h + (n > 1 ? 32 : 0);
    Uint8 ta = (Uint8)(alpha * title);
    float y = DOCK_Y - 64 - total;
    if (skel > 0.004f) { /* bottom-aligned with the title, then the line under it */
        draw_skeleton(LEFT, y + title_h - 82, 400, 64, 14, alpha / 255.0f * skel);
        draw_skeleton(LEFT, y + title_h + (cur->mh - 20) / 2, 250, 20, 10, alpha / 255.0f * skel);
    }
    if (logo) {
        float lw = cur->lw * k, lh = cur->lh * k;
        r_image(cur->logo_tex, LEFT, y, lw, lh, 255, 255, 255, ta, 0);
        y += lh + 18;
    } else {
        draw_text(cur->title_tex, cur->tw, cur->th, LEFT - 3, y, ta);
        y += cur->th + 8;
    }
    draw_text(cur->meta_tex, cur->mw, cur->mh, LEFT, y, (Uint8)(TEXT_STATIC * ta / 255));
    y += cur->mh + 14;
    if (cur->progress > 0) {
        const float bw = 240, bh = 5;
        r_shape(LEFT, y, bw, bh, bh / 2, 255, 255, 255, (Uint8)(ta * 0.28f), 0, 1, 1);
        r_shape(LEFT, y, bw * cur->progress, bh, bh / 2, 255, 255, 255, ta, 0, 1, 1);
        y += bh + 16;
    }
    float cc = 1 - fminf(fmaxf(S.cc.t, 0), 1); /* the Control Center, open, says what OK does */
    if (S.zone == ZONE_RAIL && S.rail_browse) draw_text(S.rail_browse, S.rail_browse_w, S.rail_browse_h, LEFT, y, (Uint8)(TEXT_SECONDARY * alpha / 255));
    else draw_text(S.rail_hint, S.rail_hint_w, S.rail_hint_h, LEFT, y, (Uint8)(TEXT_SECONDARY * alpha / 255 * cc)); /* it can't resume from here */
    y += S.rail_hint_h + 22;
    int browsing = S.zone == ZONE_RAIL;
    for (int i = 0; n > 1 && i < n; i++) {
        int on = i == (S.rail_index < n ? S.rail_index : n - 1);
        float d = on ? 10 : 8, dx = LEFT + i * 20 + (10 - d) / 2;
        r_shape(dx, y + (10 - d) / 2, d, d, d / 2, 255, 255, 255, (Uint8)(alpha * (on ? 0.95f : browsing ? 0.5f : 0.3f)), 0, 1, 1);
    }
}

/* The new picture fades in over the one on show, which stays at full strength underneath
 * (two half-faded pictures let the dark background show through), and the scrims are
 * drawn once over both. */
static void draw_shelf_art(Uint8 alpha, int backdrop)
{
    if (!alpha) return;
    float t = S.pic_a;
    if (t < 1) draw_pic(S.pic_under, pic_opaque(S.pic_top) ? alpha : (Uint8)(alpha * (1 - t)));
    draw_pic(S.pic_top, (Uint8)(alpha * t));
    draw_scrims(alpha);
    if (t < 1 && S.pic_under.kind == PIC_GLOW) draw_app_bigtile(&S.apps[S.pic_under.i], (Uint8)(alpha * (1 - smooth01(0, 0.5f, t))), backdrop);
    if (S.pic_top.kind == PIC_GLOW) draw_app_bigtile(&S.apps[S.pic_top.i], (Uint8)(alpha * smooth01(0.3f, 1, t)), backdrop);
}

/* a page's text: its app's name, or its carousel's title (-2: none) */
static void draw_page(int item, int rail, Uint8 alpha, float title, float skel, float dy, int backdrop)
{
    App *a = item_app(item);
    if (!a || !alpha) return;
    if (rail == -1) draw_app_text(a, alpha, dy);
    else if (rail >= 0 && rail < S.nrail && rail_for(a)) draw_rail_content(&S.rail[rail], alpha, title, skel, backdrop);
}

/* The Top Shelf while a carousel loads: placeholders where its heading and title go. */
static void draw_shelf_skeleton(float a, int backdrop)
{
    panel(backdrop, DOCK_X, TOP_Y, 300, BAR, BAR / 2, 1, 0, a);
    draw_skeleton(DOCK_X + 24, TOP_Y + (BAR - 14) / 2.0f, 220, 14, 7, a);
    float y = DOCK_Y - 64 - 170;
    draw_skeleton(LEFT, y, 400, 64, 14, a);
    draw_skeleton(LEFT, y + 84, 250, 20, 10, a);
    draw_skeleton(LEFT, y + 122, 330, 18, 9, a);
}

/* Stepping through a carousel, only the title changes, coming in with its picture.
 * Otherwise one page at a time: the old one leaves quickly, then the new one rises in.
 * While the next page loads, the text on show leaves (stepping through: the title). */
static void draw_shelf_content(Uint8 alpha, int backdrop)
{
    if (!alpha) return;
    int stepping = S.want_app == S.page_app && S.want_rail >= 0 && S.page_rail >= 0;
    float stay = 1 - S.page_leave;
    float skel = S.skel_a; /* the next page is taking a while */
    if (skel > 0.004f && !stepping) draw_shelf_skeleton(alpha / 255.0f * skel, backdrop);
    if (S.page_step) {
        draw_page(S.page_app, S.page_rail, stepping ? alpha : (Uint8)(alpha * stay),
                  fminf(1, 0.25f + S.page_a) * (stepping ? stay : 1), stepping ? skel : 0, 0, backdrop);
        return;
    }
    float t = S.page_a, in = smooth01(0.35f, 1, t);
    if (t < 0.4f && S.prev_vis > 0)
        draw_page(S.prev_app, S.prev_rail, (Uint8)(alpha * S.prev_vis * (1 - smooth01(0, 0.4f, t))), 1, 0, 0, backdrop);
    draw_page(S.page_app, S.page_rail, (Uint8)(alpha * in * (stepping ? 1 : stay)), stepping ? stay : 1, stepping ? skel : 0,
              S.reduce_motion ? 0 : 14 * (1 - in), backdrop);
}

/* The clock capsule, like tvOS 26's: the time, and at its end a circle (the Control
 * Center's glyph, where tvOS shows the profile picture). The whole capsule is the
 * button: focused, it lifts and turns into a white platter. With the Control Center
 * open, it's tvOS's header: the capsule's glass goes, the time stays on its own, and
 * the circle grows into the selected tab, solid white with the glyph dark. */
static void draw_clock(Uint8 alpha, int backdrop)
{
    if (!alpha) return;
    float k = S.top_s[TOP_SETTINGS], g = fminf(fmaxf(k, 0), 1), sc = 1 + 0.08f * k;
    float w0 = top_w(), pw = w0 * sc, ph = BAR * sc, cx = top_x() + w0 / 2, cy = TOP_Y + BAR / 2.0f;
    if (k > 0.01f) {
        float dx, dy;
        sway(&dx, &dy);
        cx += dx * k;
        cy += dy * k;
    }
    float x = cx - pw / 2, y = cy - ph / 2, o = fminf(fmaxf(S.cc.t, 0), 1) * (1 - g);
    if (k > 0.01f && !backdrop) r_shadow(x, y, pw, ph, ph / 2, 16, 6, 0.35f * k * alpha / 255.0f);
    panel(backdrop, x, y, pw, ph, ph / 2, 1, k, alpha / 255.0f * (1 - o));
    if (g > 0.01f) /* focused: an opaque white platter with dark contents, like the sheet buttons */
        r_shape(x, y, pw, ph, ph / 2, 245, 245, 247, (Uint8)(alpha * g), 0, 1, 1);
    Uint8 tc = (Uint8)(255 - 227 * g); /* the time stays its own size, so it stays sharp */
    r_image(S.clock_tex, x + 24 * sc, cy - S.clock_h / 2.0f, S.clock_w, S.clock_h, tc, tc, (Uint8)(255 - 225 * g),
            (Uint8)(alpha * (TEXT_STATIC + (255 - TEXT_STATIC) * g) / 255), 0);
    float dc = x + pw - 5 * sc - TOP_DOT * sc / 2; /* the circle's centre: it grows around it */
    float d = (TOP_DOT + (BAR + 2 - TOP_DOT) * o) * sc, ox = dc - d / 2, oy = cy - d / 2;
    if (o > 0.01f && !backdrop) r_shadow(ox, oy, d, d, d / 2, 14, 5, 0.3f * o * alpha / 255.0f);
    r_shape(ox, oy, d, d, d / 2, 255, 255, 255, (Uint8)(alpha * (0.16f * (1 - g) * (1 - o) + 0.97f * o)), 0, 1, 1);
    if (g > 0.01f) r_shape(ox, oy, d, d, d / 2, 0, 0, 0, (Uint8)(alpha * 0.07f * g), 0, 1, 1);
    float is = S.top_w[TOP_SETTINGS] * sc * (1 + 0.2f * o), gk = fmaxf(g, o);
    Uint8 gc = (Uint8)(255 - 227 * gk);
    r_image(S.top_tex[TOP_SETTINGS], ox + (d - is) / 2, oy + (d - is) / 2, is, is, gc, gc, (Uint8)(255 - 225 * gk),
            (Uint8)(alpha * (0.85f + 0.15f * gk)), 0);
}

static void draw_tile(int i, int backdrop)
{
    App *a = item_app(i);
    float bx = LEFT + (i % COLS) * (TILE_W + GAP_X);
    float by = GRID_TOP + (i / COLS) * ROW_H + S.scroll;
    if (by > H + 40 || by + ROW_H < -40) return;
    int focused = i == S.focus && S.zone == ZONE_GRID;
    float k = a ? a->s : special_s;
    float sc = 1 + (FOCUS_SCALE - 1) * k;
    int launching = S.launching && i == S.launch_item;
    if (launching) { /* opening: breathes a little below its focus size for 2 s, then holds */
        float lt = (SDL_GetTicks() - S.launch_at) / 1000.0f;
        k = 1;
        sc = lt < 2 ? FOCUS_SCALE - 0.015f + 0.015f * cosf(lt * 2 * (float)M_PI * 1.25f) : FOCUS_SCALE;
    }
    float w = TILE_W * sc, h = TILE_H * sc, rad = TILE_R * sc;
    float x = bx - (w - TILE_W) / 2, y = by - (h - TILE_H) / 2 - 6 * k;
    if (S.moving && focused && !S.reduce_motion) x += sinf(SDL_GetTicks() / 40.0f) * 2.0f; /* gentle wobble while moving */
    if (k > 0.01f) {
        float dx, dy;
        sway(&dx, &dy);
        x += dx * k;
        y += dy * k;
    }
    if (k > 0.01f && !backdrop) r_shadow(x, y, w, h, rad, 28, 16, 0.5f * k);
    if (!a) {
        static Tex *ht;
        static int hw, hh;
        r_shape(x, y, w, h, rad, 70, 70, 82, 34, 0, 1, 1);
        if (!ht) ht = text_render(S.f_semi, 28, "\xE2\x80\xA2 \xE2\x80\xA2 \xE2\x80\xA2   Hidden", 0, &hw, &hh);
        draw_text(ht, hw, hh, x + (w - hw) / 2, y + (h - hh) / 2, 205);
    } else if (draw_face(a, x, y, w, h, rad, sc, 255, k, backdrop)) { /* opaque, the icon's own colour (or a look's) */
    } else if (a->icon_state == 1) { /* its icon is on its way: a placeholder */
        float bw = w * 0.46f, bh = h * 0.15f;
        draw_skeleton(x + (w - bw) / 2, y + (h - bh) / 2, bw, bh, bh / 2, 1);
    } else { /* no icon: its name, kept inside the tile */
        ensure_label(a);
        if (a->label) {
            float k = fminf(1, (w - 32) / a->lw), lw = a->lw * k, lh = a->lh * k;
            r_image(a->label, x + (w - lw) / 2, y + (h - lh) / 2, lw, lh, 255, 255, 255, 230, 0);
        }
    }
    if (S.moving && focused) r_shape(x, y, w, h, rad, 255, 255, 255, 200, 2.0f, 1, 1); /* the tile you're moving */
    if (S.zone == ZONE_TOP && i == top_down_tile() && !backdrop) /* where Down from the top bar lands */
        r_shape(x, y, w, h, rad, 255, 255, 255, 128, 2.0f, 1, 1);
    if (focused && S.ok_down && !S.ok_long && !S.sheet.open && !S.recents.open && !S.moving && !backdrop) {
        float p = (SDL_GetTicks() - S.ok_down - 250) / (float)(LONG_PRESS_MS - 250);
        if (p > 0) { /* holding OK: a bar fills until the options open */
            float hw = w * 0.5f, hx = x + (w - hw) / 2, hy = y + h + 10;
            r_shape(hx, hy, hw, 4, 2, 255, 255, 255, 60, 0, 1, 1);
            r_shape(hx, hy, hw * fminf(p, 1), 4, 2, 255, 255, 255, 230, 0, 1, 1);
        }
    }
    if (a && k > 0.01f && !backdrop && (i >= COLS || launching)) { /* like tvOS, the top row has no names */
        ensure_label(a);
        float ly = i < COLS ? DOCK_Y + S.scroll + DOCK_H + 6 : by + TILE_H + 26; /* "Opening…" under the dock */
        Tex *lt = launching && S.launch_tex ? S.launch_tex : a->label;
        int lw = lt == a->label ? a->lw : S.launch_w, lh = lt == a->label ? a->lh : S.launch_h;
        draw_text(lt, lw, lh, bx + (TILE_W - lw) / 2, ly, (Uint8)(TEXT_STATIC * fminf(k, 1)));
    }
}

/* Everything below the overlays. backdrop: the copy the glass refracts. */
static void draw_scene(int backdrop)
{
    Uint8 shelf_a = (Uint8)(255 * (1 - S.scrolled));
    /* The backdrop (what glass refracts, and what's blurred below the dock) keeps the
     * picture at full strength and never holds its own blur: blurring last frame's blur
     * smeared it wider every frame. */
    draw_shelf_art(backdrop ? 255 : shelf_a, backdrop);
    if (S.scrolled > 0.004f && !backdrop) { /* below the dock the picture stays, blurred and dimmed, so the apps stand out */
        r_backdrop_image(1, (Uint8)(255 * S.scrolled));
        r_fill(0, 0, 0, (Uint8)(110 * S.scrolled));
    }
    /* the shelf's content leaves faster than the grid arrives, drifting up behind it */
    r_translate(0, 0.35f * S.scroll);
    draw_shelf_content((Uint8)(255 * fmaxf(0, 1 - 2.5f * S.scrolled)), backdrop);
    r_translate(0, 0);
    /* Scrolling down, the dock's glass grows to the screen's edges and frosts over: it
     * becomes the blurred background the grid sits on, crossfading into it at the end.
     * (Its stand-in in the backdrop stays out meanwhile: blurred, it made a white band.) */
    float dock_y = DOCK_Y + S.scroll, dock_a = 1 - S.scrolled, t = smooth01(0, 1, S.scrolled);
    if (t < 0.999f) {
        float gx = DOCK_X * (1 - t), gy = dock_y * (1 - t), gw = W - 2 * gx, gh = dock_y + DOCK_H + (H - dock_y - DOCK_H) * t - gy;
        float rad = (TILE_R + PAD) * (1 - t), ga = t < 0.75f ? 1 : 1 - (t - 0.75f) / 0.25f;
        if (!backdrop && t <= 0.004f) r_shadow(gx, gy, gw, gh, rad, 40, 14, 0.3f); /* only at rest: it'd be a big moving blur */
        /* clear while it's the dock (its icons are opaque), frosted as soon as it moves: growing
         * towards the whole screen, the clear glass's sharp, colour-split edge (four texture
         * reads a pixel against one) dropped frames on the TV */
        if (!backdrop || t <= 0.004f) panel(backdrop, gx, gy, gw, gh, rad, smooth01(0, 0.15f, t), 0, ga);
    }
    if (!backdrop && S.chevron_a > 0.004f && dock_a > 0.004f) { /* tvOS's chevron, 30 px above the dock */
        float cs = 56, cx = (W - cs) / 2, cy = dock_y - 30 - cs / 2;
        Uint8 ca = (Uint8)(S.chevron_a * dock_a * 255);
        r_image(S.chevron, cx, cy + 2, cs, cs, 0, 0, 0, (Uint8)(ca * 0.35f), 0); /* a soft shadow, for bright pictures */
        r_image(S.chevron, cx, cy, cs, cs, 255, 255, 255, (Uint8)(ca * 0.7f), 0);
    }
    int dimmed = !backdrop && S.dim > 0.004f, top = S.zone == ZONE_TOP;
    if (!(dimmed && top)) draw_clock((Uint8)(255 * (1 - S.scrolled)), backdrop);
    if (apps_loading()) /* the apps are on their way: placeholders in the dock */
        for (int c = 0; c < COLS; c++) draw_skeleton(LEFT + c * (TILE_W + GAP_X), GRID_TOP + S.scroll, TILE_W, TILE_H, TILE_R, 1);
    for (int i = 0; i < S.nitems; i++)
        if (i != S.focus) draw_tile(i, backdrop);
    if (dimmed) r_fill(0, 0, 0, (Uint8)(S.dim * 115)); /* OLED: idle dims all but what has focus */
    if (S.launching && !backdrop) { /* the rest steps back while the app opens */
        float d = (SDL_GetTicks() - S.launch_at - 250) / 200.0f;
        if (d > 0) r_fill(0, 0, 0, (Uint8)(77 * fminf(d, 1)));
    }
    if (S.focus < S.nitems) draw_tile(S.focus, backdrop);
    if (dimmed && top) draw_clock((Uint8)(255 * (1 - S.scrolled)), backdrop);
    if (S.moving) { /* how to finish, in a capsule at the top while an app is being moved */
        static Tex *hint;
        static int hw, hh;
        if (!hint) hint = text_render(S.f_medium, HINT_PX, "Arrows move it  \xC2\xB7  OK puts it down", 0, &hw, &hh);
        float pw = hw + 56, px = (W - pw) / 2;
        panel(backdrop, px, SAFE_Y, pw, BAR, BAR / 2, 1, 0, 1);
        draw_text(hint, hw, hh, px + 28, SAFE_Y + (BAR - hh) / 2.0f, TEXT_STATIC);
    }
}

/* Cancel and the bulk actions after a list stay put while a long list scrolls. */
static int sheet_footer(void)
{
    int k = 0;
    while (k < S.sheet.n) {
        Action a = S.sheet.it[S.sheet.n - 1 - k].act;
        if (a != ACT_CANCEL && a != ACT_UNHIDE_ALL) break;
        k++;
    }
    return k;
}

static float sheet_body_h(void)
{
    float h = 0;
    for (int i = 0; i < S.sheet.nbody; i++) h += S.sheet.bh[i] + 6;
    return h ? h + 18 : 0;
}

static float sheet_head(void)
{
    return PAD + S.sheet.th + 24 + sheet_body_h() + (S.sheet_qr_s ? S.sheet_qr_s + 24 : 0) + (S.sheet_big ? S.sheet_big_h + 24 : 0);
}

/* List rows on screen: all of them when they fit, else as many as fit above the
 * footer in a panel no taller than the safe area. */
static int sheet_rows(int nfoot)
{
    int fit = (int)((H - 2 * SAFE_Y - sheet_head() - PAD + SHEET_GAP) / (SHEET_BH + SHEET_GAP)) - nfoot;
    int nlist = S.sheet.n - nfoot;
    return nlist < fit ? nlist : fit < 1 ? 1 : fit;
}

static void draw_sheet_button(const SheetItem *it, int focused, float bx, float y, float bw, float k, Uint8 ca)
{
    static const Uint8 RED[2][3] = { { RED_ON_GLASS }, { RED_ON_WHITE } };
    const float BH = SHEET_BH;
    Uint8 a = (Uint8)(ca * k);
    /* Settings rows (a value or a page): label on the left, value and › on the right */
    int row = it->val || it->chevron || it->swatches;
    float tx = row ? bx + 36 : bx + (bw - it->w) / 2, ty = y + (BH - it->h) / 2;
    Uint8 lr, lg, lb, la;
    if (focused) { /* tvOS: the focused button turns opaque white with a dark label */
        r_shadow(bx, y, bw, BH, BH / 2, 18, 8, 0.4f * a / 255);
        r_shape(bx, y, bw, BH, BH / 2, 245, 245, 247, a, 0, 1, 1);
        lr = 28, lg = 28, lb = 30;
        la = a;
    } else { /* dark under the light glass: the label reads over white tiles too */
        r_shape(bx, y, bw, BH, BH / 2, 12, 12, 16, (Uint8)(95 * a / 255), 0, 1, 1);
        r_shape(bx, y, bw, BH, BH / 2, 255, 255, 255, (Uint8)(18 * a / 255), 0, 1, 1);
        lr = lg = lb = 255;
        la = (Uint8)(TEXT_STATIC * a / 255);
    }
    if (it->destructive) lr = RED[!!focused][0], lg = RED[!!focused][1], lb = RED[!!focused][2];
    r_image(it->tex, tx, ty, (float)it->w, (float)it->h, lr, lg, lb, la, 0);
    if (it->check) { /* the choice that's on: a tick on the right, in its label's colour */
        static Tex *tick;
        static int kw, kh;
        if (!tick) tick = text_render(S.f_medium, SHEET_BUTTON_PX, "\xE2\x9C\x93", 0, &kw, &kh);
        r_image(tick, bx + bw - 36 - kw, y + (BH - kh) / 2, (float)kw, (float)kh, lr, lg, lb, la, 0);
    }
    if (!row) return;
    if (it->swatches) { /* the colour that's on ringed, in the label's colour */
        for (int i = 0; i < NTINTS; i++) {
            float cx = swatch_x(bx, bw, i), cy = y + BH / 2, d = SWATCH_D;
            const Uint8 *c = TINTS[i].rgb;
            r_shape(cx - d / 2, cy - d / 2, d, d, d / 2, c[0], c[1], c[2], a, 0, 1, 1);
            if (i == S.icon_tint) r_shape(cx - d / 2 - 6, cy - d / 2 - 6, d + 12, d + 12, d / 2 + 6, lr, lg, lb, a, 2.5f, 1, 1);
        }
        return;
    }
    float right = bx + bw - 36;
    if (it->chevron) {
        static Tex *chev;
        static int cw, ch;
        if (!chev) chev = text_render(S.f_semi, 34, "\xE2\x80\xBA", 0, &cw, &ch);
        Uint8 c = focused ? 120 : 255;
        r_image(chev, right - cw, y + (BH - ch) / 2 - 2, (float)cw, (float)ch, c, c, focused ? 126 : 255, (Uint8)(focused ? a : TEXT_TERTIARY * a / 255), 0);
        right -= cw + 14;
    }
    if (it->val) {
        Uint8 c = focused ? 88 : 255;
        r_image(it->val, right - it->vw, y + (BH - it->vh) / 2, (float)it->vw, (float)it->vh, c, c, focused ? 94 : 255,
                (Uint8)(focused ? a : TEXT_TERTIARY * a / 255), 0);
    }
}

static void draw_sheet(void)
{
    if (!S.sheet.open) return;
    float t = S.sheet.t, tc = t < 0 ? 0 : t > 1 ? 1 : t;
    r_fill(0, 0, 0, (Uint8)(80 * tc));
    const float BH = SHEET_BH, BGAP = SHEET_GAP, rad = BH / 2 + PAD; /* corners concentric with the buttons */
    float big = S.sheet_big ? S.sheet_big_h + 24 : 0;
    int nfoot = sheet_footer(), nlist = S.sheet.n - nfoot, rows = sheet_rows(nfoot);
    float pw = S.sheet.wide ? 900 : SHEET_W; /* Settings: room for a label and its value */
    if (S.sheet.tw + 2 * PAD > pw) pw = S.sheet.tw + 2 * PAD;
    for (int i = 0; i < S.sheet.nbody; i++) if (S.sheet.bw[i] + 2 * PAD > pw) pw = S.sheet.bw[i] + 2 * PAD;
    float ph = sheet_head() + (rows + nfoot) * (BH + BGAP) - (S.sheet.n ? BGAP : 24) + PAD;
    float px = (W - pw) / 2, py = (H - ph) / 2;
    S.sheet.px = px; S.sheet.py = py; S.sheet.pw = pw; S.sheet.ph = ph; S.sheet.pr = rad;
    /* grows out of the button it came from on a spring, and its lensing
     * materialises as it does (Apple: glass appears by bending light) */
    float gx = S.sheet.fx + (px - S.sheet.fx) * t, gy = S.sheet.fy + (py - S.sheet.fy) * t;
    float gw = S.sheet.fw + (pw - S.sheet.fw) * t, gh = S.sheet.fh + (ph - S.sheet.fh) * t;
    float r0 = fminf(S.sheet.fw, S.sheet.fh) / 2, gr = r0 + (rad - r0) * t;
    if (gw < 1 || gh < 1) return;
    r_shadow(gx, gy, gw, gh, gr, 60, 24, 0.45f * tc);
    /* tinted dark like tvOS menus: bright tiles behind it stay quiet under the text */
    Glass g = { gx, gy, gw, gh, gr, 1, tc, -0.6f, -0.8f, 0, 1, SHEET_TINT };
    glass(&g);
    float c = (t - 0.55f) / 0.45f; /* content fades in once the panel is nearly there */
    Uint8 ca = (Uint8)(255 * (c < 0 ? 0 : c > 1 ? 1 : c));
    if (!ca) return;
    draw_text(S.sheet.title, S.sheet.tw, S.sheet.th, px + (pw - S.sheet.tw) / 2, py + PAD, (Uint8)(TEXT_STATIC * ca / 255));
    float y = py + PAD + S.sheet.th + 24;
    for (int i = 0; i < S.sheet.nbody; i++) {
        draw_text(S.sheet.body[i], S.sheet.bw[i], S.sheet.bh[i], px + (pw - S.sheet.bw[i]) / 2, y, (Uint8)(TEXT_SECONDARY * ca / 255));
        y += S.sheet.bh[i] + 6;
    }
    if (S.sheet.nbody) y += 18;
    if (S.sheet_qr_s) { /* on white, like a printed code: phones read it at any brightness */
        float q = (float)S.sheet_qr_s, qx = px + (pw - q) / 2;
        r_shape(qx, y, q, q, 24, 255, 255, 255, ca, 0, 1, 1);
        if (S.sheet_qr) r_image(S.sheet_qr, qx + 18, y + 18, q - 36, q - 36, 255, 255, 255, ca, 0);
        y += q + 24;
    }
    if (S.sheet_big) {
        draw_text(S.sheet_big, S.sheet_big_w, S.sheet_big_h, px + (pw - S.sheet_big_w) / 2, y, ca);
        y += big;
    }
    float bx = px + PAD, bw = pw - 2 * PAD, list_h = rows * (BH + BGAP);
    float more_above = fminf(fmaxf(S.sheet.scroll, 0), 1), more_below = fminf(fmaxf(nlist - rows - S.sheet.scroll, 0), 1);
    for (int i = 0; i < nlist; i++) {
        float ry = y + (i - S.sheet.scroll) * (BH + BGAP);
        /* rows sliding out of the list fade out; the rows at its edges dim while
         * more lie beyond them */
        float out = fmaxf(y - ry, ry + BH - (y + list_h - BGAP));
        float k = 1 - fmaxf(out, 0) / (BH * 0.6f);
        if (k <= 0) continue;
        float top = fminf(fmaxf((ry - y) / (BH + BGAP), 0), 1), bottom = fminf(fmaxf((y + list_h - BGAP - ry - BH) / (BH + BGAP), 0), 1);
        k *= 1 - 0.55f * more_above * (1 - top);
        k *= 1 - 0.55f * more_below * (1 - bottom);
        draw_sheet_button(&S.sheet.it[i], i == S.sheet.index, bx, ry, bw, k, ca);
    }
    y += list_h;
    for (int i = nlist; i < S.sheet.n; i++, y += BH + BGAP) draw_sheet_button(&S.sheet.it[i], i == S.sheet.index, bx, y, bw, 1, ca);
}

/* One App Switcher card: the app's tile, large, with its name above (tvOS). */
static void draw_switcher_card(int i, float t)
{
    __typeof__(S.recents.c[0]) *c = &S.recents.c[i];
    App *a = app_by_id(c->id);
    float fly = fminf(fmaxf(c->fly, 0), 1);
    float sc = (0.84f + 0.16f * c->s) * (0.92f + 0.08f * t);
    float w = SW_W * sc, h = SW_H * sc, rad = TILE_R * w / TILE_W;
    float x = W / 2.0f + c->x * SW_PITCH - w / 2, y = SW_CY + 40 * (1 - t) - fly * 760 - h / 2;
    float alpha = t * (1 - fly) * (0.75f + 0.25f * c->s);
    int zoom = S.recents.zooming && i == S.recents.index;
    if (zoom) { /* it grows to fill the screen while the app opens */
        float z = fminf(fmaxf(S.recents.zoom, 0), 1);
        x -= x * z;
        y -= y * z;
        w += (W - w) * z;
        h += (H - h) * z;
        rad *= 1 - z;
        alpha = t;
    }
    if (alpha < 0.004f || x > W || x + w < 0) return;
    Uint8 al = (Uint8)(255 * alpha);
    if (a) {
        draw_big_tile(a, x, y, w, h, rad, al, c->s > 0.01f && !zoom, 0);
    } else {
        r_shape(x, y, w, h, rad, 58, 58, 64, al, 0, 1, 1);
        draw_text(c->name, c->nw, c->nh, x + (w - c->nw) / 2, y + (h - c->nh) / 2, al);
    }
    if (!zoom) draw_text(c->name, c->nw, c->nh, x + 4, y - 18 - c->nh, (Uint8)(al * 0.9f));
}

/* The Control Center: tvOS's glass tiles under the clock, growing out of it one after
 * another; the focused one turns white, like every focused button. Round buttons have
 * only their icon: the focused one's name shows under the grid. */
/* Text in a room it may not fit, like tvOS's: what doesn't fit fades out at the room's edge,
 * and from `since` (0: never) it waits 2 s, then slides left to show the rest, a gap, then
 * the start again. Sets S.cc.marquee while it moves. */
static void draw_marquee(Tex *t, int tw, int th, float x, float y, float room, Uint8 c, Uint8 a, Uint32 since)
{
    if (!t || !a) return;
    if (tw <= room) {
        r_image(t, x, y, tw, th, c, c, c, a, 0);
        return;
    }
    const float gap = 40, speed = 36, rest = 2.0f, fade = 16; /* px, px/s, s, px */
    float period = tw + gap, off = 0;
    if (since && !S.reduce_motion) {
        float cyc = rest + period / speed, ts = fmodf((SDL_GetTicks() - since) / 1000.0f, cyc);
        off = ts > rest ? (ts - rest) * speed : 0;
        S.cc.marquee = 1;
    }
    /* the strip is the text, a gap and the text again; [s0, s1] of it is drawn at x + s0 - off */
    for (int k = 0; k < 2; k++) {
        float base = k * period, s0 = fmaxf(off, base), s1 = fminf(off + room, base + tw);
        if (s1 <= s0) continue;
        /* the middle at full strength, the edges in thin slices that fade (the left one only
         * while the text has moved) */
        float lo = off > 0.5f ? off + fade : off, hi = off + room - fade;
        for (float u = s0; u < s1;) {
            float e = u < lo ? fminf(fminf(u + 2, lo), s1) : u < hi ? fminf(hi, s1) : fminf(u + 2, s1);
            float m = (u + e) / 2, f = m < lo ? (m - off) / fade : m > hi ? (off + room - m) / fade : 1;
            r_image_part(t, x + u - off, y, e - u, th, (u - base) / tw, (e - base) / tw, c, c, c, (Uint8)(a * fmaxf(0, fminf(1, f))));
            u = e;
        }
    }
}

/* a switch that's off: its glyph on a pale disc (on a focused, white control, a grey one) */
static void cc_disc(float cx, float cy, float d, float g, float a)
{
    r_shape(cx - d / 2, cy - d / 2, d, d, d / 2, 255, 255, 255, (Uint8)(255 * a * 0.24f * (1 - g)), 0, 1, 1);
    if (g > 0.01f) r_shape(cx - d / 2, cy - d / 2, d, d, d / 2, 0, 0, 0, (Uint8)(255 * a * 0.08f * g), 0, 1, 1);
}

static Tex *cc_glyph(int icon, int big)
{
    if (icon < 0 || icon >= ICON_COUNT) icon = ICON_CONTROLS;
    Tex **t = big ? &S.cc.glyph_big[icon] : &S.cc.glyph[icon];
    if (!*t) *t = gfx_icon(icon, big ? 128 : 64);
    return *t;
}

/* A list control's list, in the Control Center's place: its name, then the choices (the one on
 * ticked; inputs with nothing behind them say so) */
static void draw_cc_list(float a)
{
    float x = cc_grid_x(), y = cc_grid_y(), w = 4 * CC_U + 3 * CC_G;
    int rows = S.cc.list_n < CC_LIST_ROWS ? S.cc.list_n : CC_LIST_ROWS;
    const float rh = CC_LIST_ROW_H, gap = CC_LIST_GAP, head = CC_LIST_HEAD;
    float h = head + (rows ? rows * (rh + gap) - gap : 40) + 14;
    if (!S.reduce_motion) y -= 12 * (1 - a); /* it settles in */
    r_shadow(x, y, w, h, 34, 30, 12, 0.35f * a);
    panel(0, x, y, w, h, 34, 1, 0, a);
    Uint8 al = (Uint8)(255 * a);
    draw_text(S.cc.list_title, S.cc.ltw, S.cc.lth, x + (w - S.cc.ltw) / 2, y + (head - S.cc.lth) / 2 + 3, al);
    for (int r = 0; r < rows; r++) {
        int i = S.cc.list_first + r;
        if (i >= S.cc.list_n) break;
        float ry = y + head + r * (rh + gap), rx = x + 14, rw = w - 28;
        int f = i == S.cc.list_index;
        if (f) r_shape(rx, ry, rw, rh, rh / 2, 245, 245, 247, al, 0, 1, 1);
        Uint8 c = f ? 28 : 255, ra = (Uint8)(f ? al : TEXT_STATIC * al / 255);
        r_image(S.cc.list_row[i], rx + 24, ry + (rh - S.cc.lrh[i]) / 2, S.cc.lrw[i], S.cc.lrh[i], c, c, c, ra, 0);
        if (S.cc.list_val[i]) /* the tick as its name; a detail dimmer */
            r_image(S.cc.list_val[i], rx + rw - 24 - S.cc.lvw[i], ry + (rh - S.cc.lvh[i]) / 2, S.cc.lvw[i], S.cc.lvh[i], c, c, c,
                    S.cc.list_kind == 1 ? ra : (Uint8)(TEXT_SECONDARY * al / 255), 0);
    }
}

/* The gallery, in the Control Center's place: its name, each group's name, and the controls on offer
 * as round buttons with their names under them; the focused one white, like the controls themselves */
static void draw_cc_gallery(float a)
{
    float x = RIGHT - GAL_W, y = cc_grid_y(), w = GAL_W, h = S.cc.gal_h;
    if (!S.reduce_motion) y -= 12 * (1 - a); /* it settles in */
    r_shadow(x, y, w, h, 34, 30, 12, 0.35f * a);
    panel(0, x, y, w, h, 34, 1, 0, a);
    Uint8 al = (Uint8)(255 * a);
    draw_text(S.cc.list_title, S.cc.ltw, S.cc.lth, x + (w - S.cc.ltw) / 2, y + (GAL_HEAD - S.cc.lth) / 2 + 3, al);
    for (int g = 0; g < S.cc.gal_nhead; g++) /* the group's name, small, on the left like iOS's */
        draw_text(S.cc.gal_head[g], S.cc.gal_hw[g], S.cc.gal_hh[g], x + GAL_PAD + 8, y + S.cc.gal_hy[g] + GAL_GROUP_TOP,
                  (Uint8)(TEXT_TERTIARY * al / 255));
    if (!S.cc.gal_n) { /* nothing left to add */
        static Tex *none;
        static int nw, nh;
        if (!none) none = text_render(S.f_medium, 20, "Every control this TV has is placed.", 0, &nw, &nh);
        draw_text(none, nw, nh, x + (w - nw) / 2, y + GAL_HEAD + 14, (Uint8)(TEXT_SECONDARY * al / 255));
    }
    for (int k = 0; k < S.cc.gal_n; k++) {
        const __typeof__(S.cc.cat[0]) *e = &S.cc.cat[S.cc.gal_cat[k]];
        int f = k == S.cc.list_index;
        float bx = x + S.cc.gal_x[k], by = y + S.cc.gal_y[k], d = GAL_BTN * (f ? 1.08f : 1), ox = bx - (d - GAL_BTN) / 2, oy = by - (d - GAL_BTN) / 2;
        if (f) r_shadow(ox, oy, d, d, d / 2, 14, 6, 0.4f * a);
        panel(0, ox, oy, d, d, d / 2, 1, f, a);
        if (f) r_shape(ox, oy, d, d, d / 2, 245, 245, 247, al, 0, 1, 1);
        Uint8 c = f ? 28 : 255;
        float is = 32 * d / GAL_BTN;
        r_image(cc_glyph(e->icon, 0), ox + (d - is) / 2, oy + (d - is) / 2, is, is, c, c, c, al, 0);
        float ly = by + GAL_BTN + 8;
        for (int l = 0; l < 2 && S.cc.gal_label[k][l]; l++) {
            draw_text(S.cc.gal_label[k][l], S.cc.gal_lw[k][l], S.cc.gal_lh[k][l], bx + GAL_BTN / 2.0f - S.cc.gal_lw[k][l] / 2.0f, ly,
                      (Uint8)(f ? al : TEXT_STATIC * al / 255));
            ly += S.cc.gal_lh[k][l] - 2;
        }
    }
}

static void draw_control_center(void)
{
    S.cc.marquee = 0;
    if (S.cc.t < 0.004f) return;
    float open = fminf(fmaxf(S.cc.t, 0), 1), lt = fminf(fmaxf(S.cc.list_t, 0), 1);
    float stagger = S.cc.n > 1 ? fminf(0.04f, 0.45f / (S.cc.n - 1)) : 0, span = 1 - stagger * (S.cc.n > 1 ? S.cc.n - 1 : 0);
    Uint32 now = SDL_GetTicks();
    float dim = cc_dim(); /* (also behind the gallery: still editing) */
    if (dim > 0.002f) r_fill(0, 0, 0, (Uint8)(255 * dim)); /* editing, or a row over the dock: the home screen steps back */
    for (int i = 0; i < S.cc.n; i++) {
        CCItem *c = &S.cc.it[i];
        float x, y, w, h;
        if (!cc_rect(i, &x, &y, &w, &h)) continue; /* no room for it */
        float ti = (open - stagger * i) / span;
        ti = (ti < 0 ? 0 : ti > 1 ? 1 : ti) * (1 - lt); /* out of the clock one after another; a list takes their place */
        if (ti <= 0.004f) continue;
        float k = c->s, g = fminf(fmaxf(k, 0), 1);
        float sc = (S.reduce_motion ? 1 : 0.86f + 0.14f * ti) * (1 + 0.06f * k) * (i == S.cc.picked ? 1.08f : 1);
        float cx = x + w / 2, cy = y + h / 2;
        if (!S.reduce_motion) { /* out of the clock: up and to the right while they grow */
            cx += (1 - ti) * (RIGHT - 60 - cx) * 0.25f;
            cy += (1 - ti) * (TOP_Y + BAR / 2.0f - cy) * 0.25f;
        }
        if (k > 0.01f) {
            float dx, dy;
            sway(&dx, &dy);
            cx += dx * k;
            cy += dy * k;
        }
        if (S.cc.editing && !S.reduce_motion && i != S.cc.picked) { /* editing: each wobbles a little, out of step */
            float ph = now / 1000.0f * 7.0f + i * 1.7f;
            cx += sinf(ph) * 1.4f;
            cy += cosf(ph * 1.3f) * 1.1f;
        }
        if (c->shake && now - c->shake < 450) { /* the TV said no */
            float p = (now - c->shake) / 450.0f;
            cx += sinf(p * 28) * 9 * (1 - p);
        }
        float rw = w * sc, rh = h * sc, rx = cx - rw / 2, ry = cy - rh / 2, rad = c->h == 2 ? 40 * sc : rh / 2;
        float lift = i == S.cc.picked ? 1 : k;
        if (lift > 0.01f) r_shadow(rx, ry, rw, rh, rad, 18, 8, 0.4f * lift * ti);
        panel(0, rx, ry, rw, rh, rad, 1, k, ti);
        if (g > 0.01f) r_shape(rx, ry, rw, rh, rad, 245, 245, 247, (Uint8)(255 * ti * g), 0, 1, 1);
        if (S.cc.editing && (S.reduce_motion || i == S.cc.picked)) /* still: an outline says it's editing */
            r_shape(rx, ry, rw, rh, rad, 255, 255, 255, (Uint8)(150 * ti), 2.0f, 1, 1);
        Uint8 col = (Uint8)(255 - 227 * g), ca = (Uint8)(255 * ti * (c->available ? 1 : 0.45f)); /* Not Available: dimmed */
        Uint8 gr = col, gg = col, gb = col; /* the glyph: tvOS 26's cyan when it's on (darker on white) */
        if (c->on && c->available) {
            gr = (Uint8)(16 + (0 - 16) * g), gg = (Uint8)(232 + (137 - 232) * g), gb = (Uint8)(244 + (173 - 244) * g);
        }
        int disc = c->stateful && !c->on && c->available && !strcmp(c->kind, "switch"); /* a switch that's off: on a pale disc */
        if (c->h == 2) { /* Large: its glyph large, its name under it */
            float is = 76 * sc;
            if (disc) cc_disc(cx, ry + rh * 0.42f, is * 1.35f, g, ti);
            r_image(cc_glyph(c->icon, 1), cx - is / 2, ry + rh * 0.42f - is / 2, is, is, gr, gg, gb, ca, 0);
            r_image(c->title, cx - c->tw / 2.0f, ry + rh - 26 * sc - c->th, c->tw, c->th, col, col, col, ca, 0);
        } else if (c->w == 2) { /* Wide: the glyph, then the name over its value, clear of the round end */
            float is = 30 * sc, ix = rx + 18 * sc, tx = ix + is + 12 * sc, room = rx + rw - 22 * sc - tx;
            if (disc) cc_disc(ix + is / 2, cy, is * 1.4f, g, ti);
            r_image(cc_glyph(c->icon, 0), ix, cy - is / 2, is, is, gr, gg, gb, ca, 0);
            float vh = c->value ? c->vh : 0, ty = cy - (c->th + vh) / 2;
            Uint32 since = i == S.cc.index && S.cc.open && !S.cc.editing ? S.cc.focus_at : 0; /* only the focused one scrolls */
            draw_marquee(c->title, c->tw, c->th, tx, ty, room, col, ca, since);
            draw_marquee(c->value, c->vw, c->vh, tx, ty + c->th, room, col, (Uint8)(TEXT_SECONDARY * ca / 255), since);
        } else { /* Small: the glyph; a stepper's new value shows under it for a moment */
            float is = 40 * sc;
            if (disc) cc_disc(cx, cy, is * 1.4f, g, ti);
            r_image(cc_glyph(c->icon, 0), cx - is / 2, cy - is / 2, is, is, gr, gg, gb, ca, 0);
            if (c->flash && now - c->flash < 1800 && c->value) {
                float f = 1 - smooth01(1200, 1800, (float)(now - c->flash));
                draw_text(c->value, c->vw, c->vh, cx - c->vw / 2.0f, ry + rh + 8, (Uint8)(TEXT_STATIC * ti * f));
            }
        }
    }
    if (S.cc.editing && lt < 0.99f) {
        float x, y, w, h, a = open * (1 - lt);
        if (cc_rect(S.cc.n, &x, &y, &w, &h)) { /* the + slot: a ring with a plus */
            float g = fminf(fmaxf(S.cc.plus_s, 0), 1);
            r_shape(x, y, w, h, h / 2, 255, 255, 255, (Uint8)(110 * a * (1 - g)), 2.0f, 1, 1);
            if (g > 0.01f) r_shape(x, y, w, h, h / 2, 245, 245, 247, (Uint8)(255 * a * g), 0, 1, 1);
            Uint8 col = (Uint8)(255 - 227 * g);
            r_image(cc_glyph(ICON_PLUS, 0), x + (w - 34) / 2, y + (h - 34) / 2, 34, 34, col, col, col, (Uint8)(255 * a), 0);
        }
        if (!S.cc.edit_hint) { /* what OK does, under the grid */
            S.cc.edit_hint = text_render(S.f_medium, HINT_PX, "OK picks it up  \xC2\xB7  Hold for options", 0, &S.cc.ehw, &S.cc.ehh);
            S.cc.move_hint = text_render(S.f_medium, HINT_PX, "Arrows move it  \xC2\xB7  OK puts it down", 0, &S.cc.mhw, &S.cc.mhh);
            S.cc.no_room = text_render(S.f_medium, HINT_PX, "No room  \xC2\xB7  Hold OK on one for options", 0, &S.cc.nrw, &S.cc.nrh);
            S.cc.reset = text_render(S.f_semi, 24, "Reset", 0, &S.cc.rsw, &S.cc.rsh);
        }
        Tex *ht = S.cc.picked >= 0 ? S.cc.move_hint : S.cc.plus_row < 0 ? S.cc.no_room : S.cc.edit_hint;
        int hw = ht == S.cc.move_hint ? S.cc.mhw : ht == S.cc.no_room ? S.cc.nrw : S.cc.ehw;
        int hh = ht == S.cc.move_hint ? S.cc.mhh : ht == S.cc.no_room ? S.cc.nrh : S.cc.ehh;
        if (!S.cc.problem) draw_text(ht, hw, hh, RIGHT - hw, cc_grid_y() + cc_rows() * (CC_U + CC_G) + 4, (Uint8)(TEXT_SECONDARY * a));
        if (cc_rect(S.cc.n + 1, &x, &y, &w, &h)) { /* Reset: red, like sheets' destructive buttons */
            static const Uint8 G[3] = { RED_ON_GLASS }, F[3] = { RED_ON_WHITE };
            float g = fminf(fmaxf(S.cc.reset_s, 0), 1);
            panel(0, x, y, w, h, h / 2, 1, g, a);
            if (g > 0.01f) r_shape(x, y, w, h, h / 2, 245, 245, 247, (Uint8)(255 * a * g), 0, 1, 1);
            r_image(S.cc.reset, x + (w - S.cc.rsw) / 2, y + (h - S.cc.rsh) / 2, S.cc.rsw, S.cc.rsh, (Uint8)(G[0] + (F[0] - G[0]) * g),
                    (Uint8)(G[1] + (F[1] - G[1]) * g), (Uint8)(G[2] + (F[2] - G[2]) * g), (Uint8)(255 * a), 0);
        }
    }
    if (S.cc.problem && lt < 0.99f) { /* what went wrong, in the hint's place under the grid */
        int rows = cc_rows() > 1 ? cc_rows() : 1;
        draw_text(S.cc.problem, S.cc.prw, S.cc.prh, RIGHT - S.cc.prw, cc_grid_y() + rows * (CC_U + CC_G) + 4, (Uint8)(TEXT_STATIC * open * (1 - lt)));
    }
    if (lt > 0.004f) {
        if (S.cc.list_kind == 3) draw_cc_gallery(lt * open);
        else draw_cc_list(lt * open);
    }
}

static void draw_switcher(void)
{
    float t = fminf(fmaxf(S.recents.t, 0), 1);
    if (t < 0.004f) return;
    r_backdrop_image(1, (Uint8)(255 * t)); /* the home screen behind, blurred */
    r_fill(0, 0, 0, (Uint8)(110 * t));
    Uint8 ca = (Uint8)(255 * t);
    if (!S.recents.n && S.recents.loading) { /* placeholders where the cards go */
        for (int k = -1; k <= 1; k++) {
            float sc = k ? 0.84f : 1, w = SW_W * sc, h = SW_H * sc, x = W / 2.0f + k * SW_PITCH - w / 2;
            float y = SW_CY + 40 * (1 - t) - h / 2, a = t * (k ? 0.75f : 1);
            draw_skeleton(x, y, w, h, TILE_R * w / TILE_W, a);
            draw_skeleton(x + 4, y - 44, 170 * sc, 22, 11, a);
        }
        return;
    }
    if (!S.recents.n) {
        static Tex *cap[2];
        static int cw[2], ch[2];
        static const char *const text[2] = { "No apps running", "Couldn\xE2\x80\x99t load recent apps" };
        int which = S.recents.failed;
        if (!cap[which]) cap[which] = text_render(S.f_medium, HINT_PX, text[which], 0, &cw[which], &ch[which]);
        draw_text(cap[which], cw[which], ch[which], (W - cw[which]) / 2.0f, SW_CY - ch[which] / 2.0f, (Uint8)(TEXT_SECONDARY * ca / 255));
        return;
    }
    for (int i = 0; i < S.recents.n; i++)
        if (i != S.recents.index) draw_switcher_card(i, t);
    draw_switcher_card(S.recents.index, t); /* the focused card on top */
    if (S.recents.zooming) return;
    static Tex *hint, *close_all;
    static int hw, hh, cw, ch;
    if (!hint) hint = text_render(S.f_medium, HINT_PX, "OK opens  \xC2\xB7  Up closes", 0, &hw, &hh);
    if (!close_all) close_all = text_render(S.f_medium, SHEET_BUTTON_PX, "Close All", 0, &cw, &ch);
    draw_text(hint, hw, hh, (W - hw) / 2.0f, SW_CY + SW_H / 2.0f + 48, (Uint8)(TEXT_SECONDARY * ca / 255));
    SheetItem it = { 0 };
    it.tex = close_all;
    it.w = cw;
    it.h = ch;
    it.destructive = 1;
    draw_sheet_button(&it, S.recents.close_all, (W - 360) / 2.0f, H - SAFE_Y - SHEET_BH, 360, 1, ca);
}

static void draw_toast(void)
{
    float t = S.toast_t < 0 ? 0 : S.toast_t > 1 ? 1 : S.toast_t;
    if (!S.toast_tex || t < 0.01f) return;
    float tw = S.toast_w + 72, th = S.toast_h + 40, tx = (W - tw) / 2, ty = H - SAFE_Y - th + (1 - S.toast_t) * 40;
    r_shadow(tx, ty, tw, th, th / 2, 30, 12, 0.4f * t);
    Glass g = { tx, ty, tw, th, th / 2, 1, t, -0.6f, -0.8f, 0, t, 0, 0, 0, 0 };
    glass(&g);
    draw_text(S.toast_tex, S.toast_w, S.toast_h, (W - S.toast_w) / 2, ty + 20, (Uint8)(245 * t));
}

extern int homeplus_screenshot(const char *path);

static void draw_ghost(void)
{
    if (!S.ghost.on) return;
    float t = S.ghost.t < 0 ? 0 : S.ghost.t;
    r_fill(0, 0, 0, (Uint8)(80 * t));
    float gx = S.ghost.fx + (S.ghost.px - S.ghost.fx) * t, gy = S.ghost.fy + (S.ghost.py - S.ghost.fy) * t;
    float gw = S.ghost.fw + (S.ghost.pw - S.ghost.fw) * t, gh = S.ghost.fh + (S.ghost.ph - S.ghost.fh) * t;
    float r0 = fminf(S.ghost.fw, S.ghost.fh) / 2, gr = r0 + (S.ghost.pr - r0) * t;
    if (gw < 1 || gh < 1) return;
    r_shadow(gx, gy, gw, gh, gr, 60, 24, 0.45f * t);
    Glass g = { gx, gy, gw, gh, gr, 1, t, -0.6f, -0.8f, 0, fminf(1, t * 1.5f), SHEET_TINT };
    glass(&g);
}

static int render(float dt, const char *shot)
{
    int anim = update(dt);
    /* the Top Shelf picture alone: what the dock and top bar refract (the icons
     * sit on the glass), and the blurred background below the dock */
    r_backdrop_begin();
    draw_shelf_art(255, 1);
    r_backdrop_end();

    r_frame_begin(8, 8, 11);
    draw_scene(0);
    if (S.sheet.open || S.ghost.on || S.toast_t > 0.01f || S.recents.t > 0.004f || S.cc.t > 0.004f) { /* overlays refract the whole scene */
        r_backdrop_begin();
        draw_scene(1);
        if (cc_dim() > 0.002f) r_fill(0, 0, 0, (Uint8)(255 * cc_dim())); /* (what the glass refracts steps back too) */
        r_backdrop_end();
    }
    draw_switcher();
    draw_control_center();
    draw_ghost();
    draw_sheet();
    draw_toast();
    if (S.black > 0.004f) r_fill(0, 0, 0, (Uint8)(S.black * 255)); /* OLED: idle fades to black */
    if (shot) homeplus_screenshot(shot);
    r_present();
    return anim;
}

/* ------------------------------------------------------------------ setup */

static int load_assets(void)
{
    char *base = SDL_GetBasePath();
    char path[1024];
    snprintf(path, sizeof path, "%sassets/fonts/Inter-Medium.ttf", base ? base : "./");
    S.f_medium = font_load(path);
    snprintf(path, sizeof path, "%sassets/fonts/Inter-SemiBold.ttf", base ? base : "./");
    S.f_semi = font_load(path);
    snprintf(path, sizeof path, "%sassets/fonts/Inter-Bold.ttf", base ? base : "./");
    S.f_bold = font_load(path);
    SDL_free(base);
    if (!S.f_medium || !S.f_semi || !S.f_bold) {
        fprintf(stderr, "homeplus: fonts missing\n");
        return 0;
    }
    S.scrim_v = gfx_gradient(1, 0.0f, 0.6f);
    S.scrim_h = gfx_gradient(0, 0.72f, 0.0f); /* text over daylight artwork stays readable */
    S.scrim_top = gfx_gradient(1, 0.45f, 0.0f);
    S.heading_tex = text_render(S.f_semi, HEADING_PX, "Continue Watching \xC2\xB7 Stremio", 0, &S.hw, &S.hh);
    S.rail_hint = text_render(S.f_medium, HINT_PX, "OK opens Stremio", 0, &S.rail_hint_w, &S.rail_hint_h);
    static const char *const top_names[TOP_COUNT] = { "Control Center" };
    for (int i = 0; i < TOP_COUNT; i++) {
        char hint[96];
        snprintf(hint, sizeof hint, "OK opens %s  \xC2\xB7  Down returns to your apps", top_names[i]);
        S.top_hint[i] = text_render(S.f_medium, HINT_PX, hint, 0, &S.top_hw[i], &S.top_hh[i]);
    }
    S.sub_tex = text_render(S.f_medium, HINT_PX, "OK opens  \xC2\xB7  Hold for options", 0, &S.sw, &S.sh);
    S.sub_first_tex = text_render(S.f_medium, HINT_PX, "OK opens  \xC2\xB7  Hold for options  \xC2\xB7  Back for Recents", 0,
                                  &S.sfw, &S.sfh); /* Back on the first app opens Recents */
    S.top_tex[TOP_SETTINGS] = gfx_icon(ICON_CONTROLS, 64); /* the Control Center's glyph: drawn at 30 px, 64 stays sharp */
    S.cc.picked = S.cc.ran = -1; /* the controls come from the service (their glyphs are made when first drawn) */
    S.chevron = gfx_icon(ICON_CHEVRON, 64);
    S.top_w[TOP_SETTINGS] = S.top_h[TOP_SETTINGS] = 30;
    return 1;
}

/* ------------------------------------------------------------------ idle wait */

/* LG's SDL 2.0.14 has no real blocking wait: SDL_WaitEventTimeout checks for
 * events every 10 ms, 100 wake-ups a second even while hidden. On the TV the
 * loop sleeps in poll() on the Wayland socket and on a pipe that queued events
 * (network results) write to. While hidden it still pumps SDL every 100 ms:
 * LG's SDL finishes SAM's relaunch, i.e. the Home button, in SDL_PumpEvents. */
#define HIDDEN_PUMP_MS 100

static int wake_pipe[2] = { -1, -1 };

/* the Wayland socket SDL reads events from, or -1 (not Wayland, e.g. the desktop preview) */
static int wayland_fd(void)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(S.win, &info) || info.subsystem != SDL_SYSWM_WAYLAND) return -1;
    void *lib = SDL_LoadObject("libwayland-client.so.0"); /* SDL has it loaded already */
    int (*get_fd)(struct wl_display *) = lib ? (int (*)(struct wl_display *))SDL_LoadFunction(lib, "wl_display_get_fd") : NULL;
    return get_fd ? get_fd(info.info.wl.display) : -1;
}

/* runs on whichever thread queues an SDL event and wakes the loop out of poll() */
static int SDLCALL wake_loop(void *userdata, SDL_Event *e)
{
    char c = 1;
    ssize_t n = write(wake_pipe[1], &c, 1); /* fails only when full: the loop is awake then */
    (void)userdata;
    (void)e;
    (void)n;
    return 0;
}

static void start_idle_wait(void)
{
    S.wl_fd = wayland_fd();
    if (S.wl_fd < 0 || pipe(wake_pipe) != 0) {
        S.wl_fd = -1;
        return;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(wake_pipe[i], F_SETFL, O_NONBLOCK);
        fcntl(wake_pipe[i], F_SETFD, FD_CLOEXEC); /* not inherited by luna-send */
    }
    SDL_AddEventWatch(wake_loop, NULL);
}

/* drew: a frame went out since the last wait (drawing can leave events read off
 * the socket but not yet handed to SDL) */
static int wait_event(SDL_Event *e, int timeout_ms, int drew)
{
    if (S.wl_fd < 0) return SDL_WaitEventTimeout(e, timeout_ms < 0 ? -1 : timeout_ms > 0 ? timeout_ms : 1);
    if (drew && SDL_PollEvent(e)) return 1;
    struct pollfd fds[2] = { { S.wl_fd, POLLIN, 0 }, { wake_pipe[0], POLLIN, 0 } };
    poll(fds, 2, timeout_ms);
    char buf[64];
    while (read(wake_pipe[0], buf, sizeof buf) > 0) {
    }
    return SDL_PollEvent(e);
}

/* How long the loop may sleep: until what's due next (a retry, a Trakt check, a
 * long press, the toast ending, the next OLED stage, closing the audio, the
 * next minute for the clock and pixel orbit while something is on screen), or
 * the hidden pump. -1 = until something happens. */
/* when the next OLED idle stage is due (0: none will come) */
static Uint32 idle_due(Uint32 dim_ms, Uint32 off_ms)
{
    Uint32 next = S.idle_stage == 0 && dim_ms != IDLE_NEVER ? dim_ms : off_ms;
    if (S.idle_stage == 0 && off_ms != IDLE_NEVER && off_ms < next) next = off_ms;
    return next == IDLE_NEVER ? 0 : S.last_input + next;
}

static int next_wake_ms(void)
{
    Uint32 now = SDL_GetTicks();
    Uint32 dim_ms = S.idle_dim_ms ? S.idle_dim_ms : IDLE_DIM_MS, off_ms = S.idle_off_ms ? S.idle_off_ms : IDLE_OFF_MS;
    int visible = !S.offscreen && S.idle_stage < 2;
    Uint32 turn = visible && S.rail_next ? (S.rail_next - 2500 > now ? S.rail_next - 2500 : S.rail_next) : 0;
    int waiting = visible && (S.want_app != S.page_app || S.want_rail != S.page_rail); /* the Top Shelf's next page */
    Uint32 page = waiting ? S.want_at + (now - S.want_at < SKEL_MS ? SKEL_MS : PAGE_WAIT_MS) : 0;
    /* the Top Shelf turns, and shows its next page, in a frame: one due before the last frame was that
     * frame's to do, and isn't waited for again (a turn waiting for its next picture happens when the
     * picture comes, which wakes the loop) */
    if (turn && (Sint32)(turn - S.drawn_at) <= 0) turn = 0;
    if (page && (Sint32)(page - S.drawn_at) <= 0) page = 0;
    Uint32 due[11] = { S.setup_retry, S.next_retry, S.trakt_poll, S.media_poll, S.ok_down && !S.ok_long ? S.ok_down + LONG_PRESS_MS : 0, turn,
                      S.toast_tex && S.toast_until > now ? S.toast_until : 0,
                      S.offscreen || S.idle_stage >= 2 ? 0 : idle_due(dim_ms, off_ms),
                      snd_active() ? now + 1000 : 0, visible ? now + (60000 - now % 60000) : 0, page };
    int wait = -1;
    for (int i = 0; i < 11; i++) {
        if (!due[i]) continue;
        int d = (Sint32)(due[i] - now) > 0 ? (int)(due[i] - now) : 0;
        if (wait < 0 || d < wait) wait = d;
    }
    if (visible) { /* the clock's next minute */
        time_t tt = time(NULL);
        int d = (60 - (int)(tt % 60)) * 1000 + 5;
        if (wait < 0 || d < wait) wait = d;
    }
    if (S.offscreen && (wait < 0 || wait > HIDDEN_PUMP_MS)) wait = HIDDEN_PUMP_MS;
    return wait;
}

/* Another app is in front: free the big pictures except the one on show (they
 * come back on demand; ~8 MB each on this GPU). */
static void release_images(void)
{
    App *keep = item_app(S.shelf_cur);
    for (int i = 0; i < S.napps; i++) {
        App *a = &S.apps[i];
        int shown = (S.pic_under.kind == PIC_ART && S.pic_under.i == i) || (S.pic_top.kind == PIC_ART && S.pic_top.i == i);
        if (a == keep || shown || !a->art_tex) continue;
        tex_free(&a->art_tex);
        a->art_state = 0;
    }
    for (int i = 0; i < S.nrail; i++) {
        RailItem *it = &S.rail[i];
        if (i == S.rail_index || i == S.page_rail || pic_rail(i)) continue;
        if (it->bg_tex) tex_free(&it->bg_tex), it->bg_state = 0;
        if (it->logo_tex) tex_free(&it->logo_tex), it->logo_state = 0;
    }
}

/* Another app is in front (LG's SDL may say so as HIDDEN, MINIMIZED or
 * DIDENTERBACKGROUND): stop drawing, release the audio and the big pictures. */
static void go_offscreen(void)
{
    if (S.offscreen) return;
    S.offscreen = 1;
    S.keys_until = 0;
    sw_reset(); /* an app opened from it is in front now */
    S.launching = 0;
    snd_suspend();
    release_images();
}

static void come_back(void)
{
    if (S.away) go_home(); /* back from another app (or the Home button): start at the top */
    S.away = 0;
    if (S.asked) open_asked(S.asked); /* ...unless the Control Center over that app asked for something */
    S.asked = 0;
    S.offscreen = 0;
    S.last_input = SDL_GetTicks();
    S.idle_stage = 0;
    if (S.setup == SETUP_DONE) request_data();
    /* only a setup that stopped on an error starts over; one in progress carries on */
    else if (S.setup == SETUP_NO_HBC || S.setup == SETUP_DENIED || S.setup == SETUP_FAILED) setup_retry_now();
}

/* Any input resets the OLED idle timer. Returns 1 when it only woke a dark
 * screen, so that press does nothing else (key: its release is dropped too). */
static int wake(int key)
{
    int dark = S.black > 0.01f;
    S.last_input = SDL_GetTicks();
    S.idle_stage = 0;
    if (dark && key) S.swallow_up = 1;
    return dark;
}

/* The remote's Settings key while Home+ is in front (LG Input Hook runs our script, which asks the
 * service): the Control Center, as from the clock, over whatever was open; pressed again, it closes. */
static void settings_key(void)
{
    wake(0); /* a dimmed or dark screen comes back too */
    if (S.cc.open) {
        cc_close();
        return;
    }
    if (S.sheet.open) sheet_dismiss();
    if (S.recents.open) sw_reset();
    if (S.moving) { /* moving an app: it stays where it is now */
        S.moving = 0;
        save_layout();
    }
    if (S.zone != ZONE_TOP) enter_top(S.focus % COLS);
    cc_open();
}

/* The remote's Input key (LG Input Hook), while Home+ is in front: the Control Center's inputs, as
 * over other apps. Opened for them alone, Back closes them; the key again closes them either way. */
static void input_key(void)
{
    wake(0); /* a dimmed or dark screen comes back too */
    if (S.cc.open && S.cc.list == 2) {
        cc_close();
        return;
    }
    int was_open = S.cc.open;
    if (!was_open) settings_key(); /* opens the Control Center, as the Settings key does */
    cc_list_open(2);
    S.cc.list_only = !was_open;
}

/* window -> logical coordinates (the GL drawable may not be 1920x1080) */
static void to_logical(int *x, int *y)
{
    int ww, wh;
    SDL_GetWindowSize(S.win, &ww, &wh);
    if (ww > 0 && wh > 0) {
        *x = *x * W / ww;
        *y = *y * H / wh;
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    /* LG's SDL: let the app handle Back/Exit itself instead of the system closing it. */
    SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_BACK", "true");
    SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_EXIT", "true");
    SDL_SetHint("SDL_WEBOS_CURSOR_SLEEP_TIME", "5000");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "homeplus: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    r_gl_attributes();
    S.win = SDL_CreateWindow("Home+", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, W, H,
                             SDL_WINDOW_OPENGL | (getenv("HOMEPLUS_SCREENSHOT") ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP) | SDL_WINDOW_SHOWN);
    if (!S.win) {
        fprintf(stderr, "homeplus: window: %s\n", SDL_GetError());
        return 1;
    }
    if (!r_init(S.win, W, H)) {
        fprintf(stderr, "homeplus: renderer init failed\n");
        return 1;
    }
    if (!load_assets()) return 1;
    snd_init();

    net_start();
    S.ptr_ax = S.ptr_ay = -1;
    start_idle_wait();
    {
        int dw, dh, ww, wh;
        SDL_GL_GetDrawableSize(S.win, &dw, &dh);
        SDL_GetWindowSize(S.win, &ww, &wh);
        log_remote("native up: renderer=gles2 drawable=%dx%d window=%dx%d idle=%s", dw, dh, ww, wh, S.wl_fd >= 0 ? "poll" : "sdl");
    }
    S.shelf_cur = S.page_app = S.prev_app = S.want_app = -1;
    S.page_rail = S.prev_rail = S.want_rail = -2;
    S.feat_req = -1;
    S.last_input = SDL_GetTicks();
    S.orbit_k = -1;
    setup_check(); /* loads the data once the service is up and elevated */

    const char *shot = getenv("HOMEPLUS_SCREENSHOT"); /* host preview: render then save a PNG */
    /* tests: run as on the TV for this long, then say how often the loop woke and drew in its last 10 s */
    Uint32 rest_ms = getenv("HOMEPLUS_REST_REPORT_MS") ? (Uint32)atoi(getenv("HOMEPLUS_REST_REPORT_MS")) : 0;
    unsigned rest_wakes = 0, rest_frames = 0;
    Uint32 shot_at = shot ? SDL_GetTicks() + (Uint32)atoi(getenv("HOMEPLUS_SCREENSHOT_MS") ? getenv("HOMEPLUS_SCREENSHOT_MS") : "2500") : 0;
    extern void homeplus_preview_input(Uint32 elapsed);

    int running = 1, animating = 1, drew = 1;
    Uint32 last = SDL_GetTicks(), start = last;
    while (running) {
        SDL_Event e;
        /* sleep while idle or hidden; wake on input, a queued event or what's due next */
        int got;
        if (shot || (animating && !S.offscreen)) got = SDL_PollEvent(&e);
        else if ((Sint32)(S.keys_until - SDL_GetTicks()) > 0) got = SDL_WaitEventTimeout(&e, 10);
        else got = wait_event(&e, next_wake_ms(), drew);
        drew = 0;
        int dirty = got;
        while (got) {
            switch (e.type) {
            case SDL_QUIT: running = 0; break;
            case SDL_KEYDOWN:
                if (e.key.keysym.sym == WEBOS_KEY_CURSOR_HIDE) { /* the pointer fell asleep: not input */
                    keys_take_over();
                    S.cursor_up = 0;
                    break;
                }
                S.keys_until = SDL_GetTicks() + 600;
                if (wake(1)) break;
                on_key_down(&e.key);
                break;
            case SDL_KEYUP:
                if (S.swallow_up) {
                    S.swallow_up = 0;
                    break;
                }
                on_key_up(&e.key);
                break;
            case SDL_MOUSEMOTION:
                /* the Magic Remote pointer: what it points at gets focus */
                if (wake(0)) break;
                {
                    int mx = e.motion.x, my = e.motion.y;
                    to_logical(&mx, &my);
                    if (pointer_moved(mx, my) && !S.moving && !S.ok_down) pointer_at(mx, my, 1);
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (wake(0)) { S.ptr_press = 0; break; }
                if (e.button.button == SDL_BUTTON_LEFT && !S.ok_down) {
                    int mx = e.button.x, my = e.button.y, outside = 0;
                    to_logical(&mx, &my);
                    S.click_x = mx;
                    if (!pointer_moved(mx, my)) { /* the keys are in charge: it's their OK */
                        S.click_x = -1;
                        S.ptr_press = 1;
                        S.ptr_dismiss = 0;
                        S.ok_down = SDL_GetTicks();
                        S.ok_long = 0;
                        break;
                    }
                    S.ptr_press = pointer_at(mx, my, 1);
                    /* a click beside a sheet closes it, as Back would */
                    S.ptr_dismiss = !S.ptr_press && S.sheet.open && !S.sheet.locked && !S.recents.open &&
                                    (sheet_button_at(mx, my, &outside), outside);
                    if (!S.ptr_press && S.cc.open && !S.sheet.open) S.ptr_dismiss = 1; /* and beside the Control Center */
                    if (S.ptr_press) { /* held, it opens options like a held OK */
                        S.ok_down = SDL_GetTicks();
                        S.ok_long = 0;
                    }
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button != SDL_BUTTON_LEFT) break;
                if (S.ptr_press && S.ok_down) ok_release();
                else if (S.ptr_dismiss && S.sheet.open) sheet_dismiss();
                else if (S.ptr_dismiss && S.cc.open) cc_close();
                S.ptr_press = S.ptr_dismiss = 0;
                break;
            case SDL_MOUSEWHEEL:
                if (wake(0)) break;
                if (S.recents.open) sw_key(e.wheel.y < 0 ? SDLK_RIGHT : SDLK_LEFT);
                else if (S.sheet.open) {
                    if (e.wheel.y < 0 && S.sheet.index < S.sheet.n - 1) S.sheet.index++;
                    else if (e.wheel.y > 0 && S.sheet.index > 0) S.sheet.index--;
                } else move_focus(0, e.wheel.y < 0 ? 1 : -1);
                break;
            case SDL_APP_DIDENTERBACKGROUND:
                go_offscreen();
                break;
            case SDL_APP_DIDENTERFOREGROUND:
                come_back();
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || e.window.event == SDL_WINDOWEVENT_SHOWN ||
                    e.window.event == SDL_WINDOWEVENT_RESTORED)
                    come_back();
                else if (e.window.event == SDL_WINDOWEVENT_HIDDEN || e.window.event == SDL_WINDOWEVENT_MINIMIZED)
                    go_offscreen();
                break;
            default:
                if (e.type == net_event) {
                    Result r;
                    while (net_poll(&r)) {
                        handle_result(&r);
                        net_result_free(&r);
                    }
                }
            }
            got = SDL_PollEvent(&e);
        }

        Uint32 now = SDL_GetTicks();
        if (rest_ms && now - start >= rest_ms - 10000) rest_wakes++;
        if (rest_ms && now - start >= rest_ms) {
            printf("at rest: %.1f wakes/s, %.1f frames/s\n", rest_wakes / 10.0, rest_frames / 10.0);
            fflush(stdout);
            running = 0;
        }
        snd_idle();
        if (S.ok_down && !S.ok_long && now - S.ok_down >= LONG_PRESS_MS) {
            S.ok_long = 1;
            if (!S.sheet.open && !S.recents.open && !S.moving && S.zone == 0) {
                snd_play(SND_SELECT);
                open_app_sheet();
            } else if (S.cc.open && !S.sheet.open) {
                snd_play(SND_SELECT);
                cc_long_press();
            }
            dirty = 1;
        }
        if (S.trakt_poll && now >= S.trakt_poll) {
            S.trakt_poll = 0;
            net_get_json("/api/trakt/status", TAG_TRAKT_STATUS, 0, 0);
        }
        if (S.media_poll && now >= S.media_poll) {
            char path[48];
            S.media_poll = 0;
            snprintf(path, sizeof path, "/api/%s/status", SRC_ID[S.media_poll_src]);
            net_get_json(path, TAG_MEDIA_STATUS, 0, 0);
        }
        if (S.setup_retry && now >= S.setup_retry) {
            S.setup_retry = 0;
            net_wake_service();
            setup_check();
        }
        if (S.next_retry && now >= S.next_retry) {
            S.next_retry = 0;
            net_get_json("/api/apps", TAG_APPS, 0, 0);
            rail_request(SRC_STREMIO);
            if (!S.layout_loaded) net_get_json("/api/layout", TAG_LAYOUT, 0, 0);
        }
        if (shot || rest_ms) homeplus_preview_input(now - start);
        if (S.toast_tex && now < S.toast_until + 100) dirty = 1;
        {
            time_t tt = time(NULL);
            struct tm *lt = localtime(&tt);
            if (lt->tm_hour * 60 + lt->tm_min != S.clock_min) dirty = 1;
        }
        /* OLED: idle stages, and a 1 px step of the pixel orbit every minute (within +-4 px) */
        {
            Uint32 idle = now - S.last_input;
            Uint32 dim_ms = S.idle_dim_ms ? S.idle_dim_ms : IDLE_DIM_MS, off_ms = S.idle_off_ms ? S.idle_off_ms : IDLE_OFF_MS;
            int stage = off_ms != IDLE_NEVER && idle >= off_ms ? 2 : dim_ms != IDLE_NEVER && idle >= dim_ms ? 1 : 0;
            if (stage != S.idle_stage && !shot) {
                S.idle_stage = stage;
                dirty = 1;
            }
            int k = (int)(now / 60000);
            if (k != S.orbit_k) {
                S.orbit_k = k;
                int still = shot || S.orbit_off; /* Settings -> Display & OLED -> Pixel Shift */
                r_set_offset(still ? 0 : roundf(4 * sinf(k * 0.61f)), still ? 0 : roundf(4 * sinf(k * 0.37f + 1.0f)));
                dirty = 1;
            }
        }
        /* woken for what a frame does (the Top Shelf turning, its next page): draw one */
        if (!dirty && !animating && next_wake_ms() == 0) dirty = 1;
        /* nothing to draw while hidden behind another app or once the screen is black */
        if ((S.offscreen || (S.idle_stage == 2 && S.black >= 0.999f)) && !shot) dirty = animating = 0;

        if (dirty || animating || shot) {
            float dt = (now - last) / 1000.0f;
            if (dt > 0.1f) dt = 0.1f;
            int take = shot && now >= shot_at;
            animating = render(dt, take ? shot : NULL);
            drew = 1;
            S.drawn_at = now;
            if (rest_ms && now - start >= rest_ms - 10000) rest_frames++;
            if (take) running = 0;
        }
        last = now;
    }
    net_stop();
    r_shutdown();
    SDL_Quit();
    return 0;
}
