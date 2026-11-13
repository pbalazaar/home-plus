/* Input handoff on return: real input.c/idle.c, with the service and perf calls stood in. */
#include <stdio.h>
#include <string.h>
#include "home.h"

State S;
void request_data(void) {}
void setup_retry_now(void) {}
void perf_check(void) {}
void perf_back(void) {}

#ifdef INPUT_WEBOS_CURSOR
static int visible = 1;
SDL_bool SDL_webOSCursorVisibility(SDL_bool show)
{
    visible = show;
    return SDL_TRUE;
}
static void cursor_set(int show) { visible = show; }
static int cursor_hidden(void) { return !visible; }
#else
static void cursor_set(int show) { SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE); }
static int cursor_hidden(void) { return SDL_ShowCursor(SDL_QUERY) == SDL_DISABLE; }
#endif

static int failures;
static void check(int good, const char *what)
{
    if (!good) { printf("  FAIL: %s\n", what); failures++; }
}

static void returning(void)
{
    memset(&S, 0, sizeof S);
    S.setup = SETUP_DONE;
    S.offscreen = 1;
    S.ptr_mode = S.cursor_up = 1;
    S.ptr_ax = 200; S.ptr_ay = 800;
    cursor_set(1);
    come_back();
}

int main(void)
{
    SDL_setenv("SDL_VIDEODRIVER", "d" "ummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO)) { printf("SDL: %s\n", SDL_GetError()); return 1; }
    returning();
    check(!S.ptr_mode, "returning puts the keys in charge");
    check(cursor_hidden(), "returning hides the inherited system cursor");
    check(!pointer_moved(1500, 900), "the first position after another app is a baseline, not movement");
    check(!pointer_click(400, 200), "an inherited click uses D-pad focus regardless of its coordinates");
    check(!pointer_moved(1510, 900), "small movement does not take over");
    check(pointer_moved(1550, 900), "deliberate movement still enables the pointer");
    check(!cursor_hidden(), "deliberate motion restores cursor visibility");
    come_back();
    check(S.ptr_mode, "duplicate foreground notifications do not reset an active pointer");
    pointer_moved(200, 800);
    keys_take_over();
    cursor_set(0); S.cursor_up = 0; /* the arrow hid it; movement must show it again */
    check(pointer_moved(250, 800), "ordinary pointer takeover still works after an arrow");
    check(!cursor_hidden(), "ordinary pointer takeover restores visibility too");
    memset(&S, 0, sizeof S);
    cursor_set(1);
    pointer_home();
    check(cursor_hidden() && !S.ptr_mode && S.ptr_entry, "cold entry starts with D-pad too");
    returning();
    S.zone = ZONE_GRID;
    S.focus = 3;
    check(!pointer_moved(200, 800), "inherited position before cursor notification stays a baseline");
    cursor_set(1); /* webOS has made its pointer available again */
    pointer_visibility_changed(SDL_TRUE);
    check(!cursor_hidden() && S.cursor_up, "cursor-show notification is observed without hiding it again");
    check(S.ptr_entry && !S.ptr_mode && S.focus == 3, "cursor-show keeps D-pad entry and focus");
    check(!pointer_moved(1500, 900), "cursor-show starts a fresh movement baseline");
    check(S.cursor_up, "entry motion preserves the observed cursor visibility");
    check(!pointer_click(400, 200), "cursor-show alone does not redirect OK to pointer coordinates");
    pointer_visibility_changed(SDL_TRUE);
    check(!pointer_moved(1510, 900), "repeated cursor-show still ignores small movement");
    pointer_visibility_changed(SDL_TRUE);
    check(pointer_moved(1550, 900), "repeated cursor-show does not reset deliberate movement progress");
    check(!cursor_hidden() && S.cursor_up && !S.ptr_entry, "pointer handoff remains visible and ends entry mode");
    cursor_set(0);
    pointer_visibility_changed(SDL_FALSE);
    check(!S.cursor_up && cursor_hidden(), "cursor-hide notification records the system visibility");
    SDL_Quit();
    printf(failures ? "input: %d failed\n" : "input: all passed\n", failures);
    return failures != 0;
}
