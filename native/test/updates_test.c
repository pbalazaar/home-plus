#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "home.h"
State S;
static char sent[2048];
static int gen, posts, gets, opened, settings_refreshed;
static Uint32 ticks = 100;
Uint32 SDL_GetTicks(void) { return ticks; }
int snd_active(void) { return 0; }
const char *jstr(cJSON *o, const char *k) { cJSON *v = cJSON_GetObjectItem(o, k); return cJSON_IsString(v) ? v->valuestring : ""; }
void net_post(const char *p, const char *b, int tag) { assert(!strcmp(p, "/api/updates")); snprintf(sent, sizeof sent, "%s", b); posts++; }
void net_get_json(const char *p, int tag, int index, int generation) { gen = generation; gets++; }
void sheet_open(const char *t) { S.sheet.open = 1; S.sheet.n = S.sheet.index = 0; opened++; }
void sheet_body(const char *t) {}
void sheet_add(const char *l, Action a, int arg, int d) { S.sheet.it[S.sheet.n].act=a; S.sheet.it[S.sheet.n++].arg=arg; }
void sheet_dismiss(void) { S.sheet.open = 0; S.sheet.page = 0; }
void toast(const char *s, int n) {}
void settings_again(void) { settings_refreshed++; }
static void answer(const char *body, int tag, int generation)
{
    Result r = { .tag=tag, .status=200, .body=(char *)body, .generation=generation };
    updates_result(&r);
}
#define READY "{\"returnValue\":true,\"phase\":\"ready\",\"upstreamVersion\":\"0.26.0\",\"confirmation\":\"hash-a\",\"canInstallPreserved\":true,\"prompt\":true,\"automatic\":true}"
static void regression(const char *which)
{
    S.setup = SETUP_DONE; S.onboarded = 1;
    if (!strcmp(which, "pending")) {
        updates_action(ACT_UPDATE_AUTO, 0);
        ticks = 10000; updates_tick();
        assert(posts == 1 && gets == 0);
        assert(next_wake_ms() > 0); /* slow POST must not leave an expired deadline */
        answer(READY, TAG_UPDATE_ACTION, 0);
        assert(S.updates_poll > ticks);
    } else if (!strcmp(which, "getting")) {
        updates_tick();
        ticks = 70000; updates_tick();
        assert(gets == 1);
        assert(next_wake_ms() > 0); /* the same invariant applies to an in-flight GET */
        answer(READY, TAG_UPDATES, gen);
        assert(S.updates_poll > ticks);
    } else if (!strcmp(which, "hidden")) {
        updates_open(1);
        int before = opened;
        S.offscreen = 1;
        answer(READY, TAG_UPDATE_ACTION, 0);
        assert(opened == before);
        S.offscreen = 0; S.ok_down = ticks; S.ok_long = 0;
        updates_tick();
        assert(opened == before + 1 && S.sheet.it[1].act == ACT_UPDATE_INSTALL);
        assert(S.sheet.index == 0 && S.ok_long); /* reconcile without transferring a held OK */
        updates_tick(); assert(opened == before + 1);
    } else if (!strcmp(which, "about")) {
        S.sheet.open = S.sheet.split = 1;
        S.sheet.cat_page[0] = PAGE_ABOUT;
        S.offscreen = 1;
        answer(READY, TAG_UPDATES, gen);
        assert(!settings_refreshed);
        S.offscreen = 0; updates_tick();
        assert(settings_refreshed == 1 && !opened);
        updates_tick(); assert(settings_refreshed == 1);
    } else assert(0);
    printf("updates regression %s: passed\n", which);
}
int main(int argc, char **argv)
{
    SDL_Init(0);
    if (argc > 1) { regression(argv[1]); SDL_Quit(); return 0; }
    S.setup=SETUP_DONE; S.onboarded=1;
    updates_tick(); assert(gets==1);
    answer(READY,TAG_UPDATES,gen);
    S.sheet.open=1; updates_tick(); assert(!opened);
    S.sheet.open=0; S.offscreen=1; updates_tick(); assert(!opened);
    S.offscreen=0; updates_tick(); assert(opened==1 && S.sheet.index==0);
    assert(S.sheet.it[0].act==ACT_UPDATE_KEEP);
    updates_keep(); assert(posts==1 && strstr(sent,"keep") && strstr(sent,"0.26.0"));
    answer(READY,TAG_UPDATES,gen);
    updates_tick(); assert(!S.sheet.open);
    answer("{\"returnValue\":true,\"phase\":\"ready\",\"prompt\":false}",TAG_UPDATE_ACTION,0);
    updates_open(0); updates_action(ACT_UPDATE_INSTALL,0); assert(posts==1);
    answer(READY,TAG_UPDATES,gen+1);
    updates_open(0); S.offscreen=1; updates_action(ACT_UPDATE_INSTALL,0); assert(posts==1);
    S.offscreen=0; updates_action(ACT_UPDATE_INSTALL,0);
    assert(posts==2 && strstr(sent,"hash-a") && strstr(sent,"preserve"));
    updates_action(ACT_UPDATE_INSTALL,0); assert(posts==2);
    puts("updates: foreground, suppression, stale GET, explicit token, fail closed and repeat checks passed");
    SDL_Quit(); return 0;
}
