/*
 * OP Location - the 4 saved locations OP APRS can send after a message, as an
 * APRS position report: home, a club, the spot you are at now. They live in
 * the resident service (loc, API level 5), kept over power off with the inbox.
 *
 * Keys: UP/DOWN the slot · MENU edit it · 5 attach it to OP APRS's messages
 *   (again: none) · EXIT back (to OP APRS when started from it).
 * Editing: the digits in order, degrees, minutes and hundredths of a minute,
 *   latitude then longitude (49 03.50 N, 072 01.75 W: 4903500720175); * N/S,
 *   F E/W; EXIT deletes (on none: the slot cleared); MENU done.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../app_api.h"

#define LOOP_MS     40u
#define DIGITS      13u     /* DDMMmm DDDMMmm */

static struct {
    const app_api_t *api;
    app_loc_t *loc;
    uint8_t prevKey, slot, edit, n, south, west;
    bool running, redraw, changed, fromAprs;
    char dig[DIGITS];
    char str[34];
} g;
#define A (g.api)

void *memset(void *d, int c, size_t n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }

static char *put(char *o, const char *s) { while (*s) *o++ = *s++; return o; }
/* v as n digits, into o; returns past them. */
static char *dig(char *o, uint32_t v, uint8_t n)
{
    for (uint8_t i = n; i--;) {
        const uint64_t d = A->uidivmod(v, 10u);
        o[i] = (char)('0' + (uint32_t)(d >> 32));
        v = (uint32_t)d;
    }
    return o + n;
}
/* The slot's latitude or longitude (DD or DDD), as digits: true when set. */
static bool split(int32_t v, uint8_t degs, char *o)
{
    if (v == APP_LOC_NONE) return false;
    if (v < 0) v = -v;
    const uint64_t d = A->uidivmod((uint32_t)v, 6000u);
    o = dig(o, (uint32_t)d, degs);
    dig(o, (uint32_t)(d >> 32), 4);                 /* minutes and hundredths */
    return true;
}
static uint32_t num(const char *s, uint8_t n) { uint32_t v = 0; while (n--) v = v * 10u + (uint32_t)(*s++ - '0'); return v; }

static void startEdit(void)
{
    g.edit = 1;
    const int32_t la = g.loc->lat[g.slot], lo = g.loc->lon[g.slot];
    g.n = split(la, 2, g.dig) && split(lo, 3, g.dig + 6) ? DIGITS : 0;
    g.south = la != APP_LOC_NONE && la < 0;
    g.west = lo == APP_LOC_NONE || lo < 0;          /* most users: the western hemisphere */
}
static void endEdit(void)
{
    g.edit = 0;
    if (g.n < DIGITS) return;                       /* incomplete: unchanged */
    const uint32_t dla = num(g.dig, 2), mla = num(g.dig + 2, 4), dlo = num(g.dig + 6, 3), mlo = num(g.dig + 9, 4);
    if (dla > 90u || dlo > 180u || mla >= 6000u || mlo >= 6000u) return;
    const int32_t la = (int32_t)(dla * 6000u + mla), lo = (int32_t)(dlo * 6000u + mlo);
    g.loc->lat[g.slot] = g.south ? -la : la;
    g.loc->lon[g.slot] = g.west ? -lo : lo;
    g.changed = true;
}

static void tiny(uint8_t y, char *end) { *end = 0; A->print_tiny(g.str, 0, y, false, true); }

/* "LAT 49 03.50 N" from digits d (degs of them for the degrees). */
static char *coord(char *o, const char *d, uint8_t degs, uint8_t have, char hemi)
{
    for (uint8_t i = 0; i < degs + 4u; i++) {
        if (i == degs) *o++ = ' ';
        if (i == degs + 2u) *o++ = '.';
        *o++ = i < have ? d[i] : '_';
    }
    *o++ = ' ';
    *o++ = hemi;
    return o;
}

static void draw(void)
{
    char *o, d[DIGITS];
    A->display_clear();
    A->status_clear();
    A->print_inverse("LOCATION", 2, 0, true, true, 2 + 8 * 4);
    A->draw_battery();
    o = put(g.str, "Slot ");
    *o++ = (char)('1' + g.slot);
    if (g.loc->attach == g.slot + 1u) o = put(o, " SENT");
    *o = 0;
    A->print_bold(g.str, 0, 0, 0);
    uint8_t have = DIGITS;
    bool south = g.south, west = g.west;
    if (g.edit) { for (uint8_t i = 0; i < DIGITS; i++) d[i] = g.dig[i]; have = g.n; }
    else if (split(g.loc->lat[g.slot], 2, d) && split(g.loc->lon[g.slot], 3, d + 6)) {
        south = g.loc->lat[g.slot] < 0;
        west = g.loc->lon[g.slot] < 0;
    } else have = 0;
    if (!g.edit && !have) { o = put(g.str, "NOT SET: MENU TO ENTER IT"); tiny(16, o); }
    else {
        o = coord(put(g.str, "LAT "), d, 2, have, south ? 'S' : 'N');
        tiny(14, o);
        o = coord(put(g.str, "LON "), d + 6, 3, have > 6u ? (uint8_t)(have - 6u) : 0u, west ? 'W' : 'E');
        tiny(22, o);
    }
    o = put(g.str, g.loc->attach ? "OP APRS SENDS SLOT " : "OP APRS SENDS NONE");
    if (g.loc->attach) *o++ = (char)('0' + g.loc->attach);
    tiny(36, o);
    o = put(g.str, g.edit ? "0-9 *N/S FE/W EXIT DEL MENU OK" : "< > SLOT  MENU EDIT  5 SEND IT");
    tiny(50, o);
    A->blit_status();
    A->blit_full();
}

static void keys(void)
{
    uint8_t key = A->get_key();
    if (key == APP_KEY_SAVER) return;
    if (key == APP_KEY_INVALID || key == g.prevKey) { g.prevKey = key; return; }
    g.prevKey = key;
    g.redraw = true;
    A->backlight_on();
    if (key == APP_KEY_WAKE) return;
    if (g.edit) {
        if (key <= APP_KEY_9 && g.n < DIGITS) g.dig[g.n++] = (char)('0' + key);
        else if (key == APP_KEY_STAR) g.south ^= 1u;
        else if (key == APP_KEY_F) g.west ^= 1u;
        else if (key == APP_KEY_MENU) endEdit();
        else if (key == APP_KEY_EXIT) {
            if (g.n) g.n--;
            else {                                  /* nothing left: the slot cleared */
                g.loc->lat[g.slot] = g.loc->lon[g.slot] = APP_LOC_NONE;
                if (g.loc->attach == g.slot + 1u) g.loc->attach = 0;
                g.changed = true;
                g.edit = 0;
            }
        }
        return;
    }
    int8_t d = A->nav_dir(key);
    if (key == APP_KEY_EXIT) g.running = false;
    else if (key == APP_KEY_MENU) startEdit();
    else if (key == APP_KEY_5 && g.loc->lat[g.slot] != APP_LOC_NONE) {
        g.loc->attach = g.loc->attach == g.slot + 1u ? 0u : (uint8_t)(g.slot + 1u);
        g.changed = true;
    } else if (d) g.slot = (uint8_t)((g.slot + (d > 0 ? 1u : 3u)) & 3u);
}

__attribute__((section(".text.entry"), used))
void app_main(const app_api_t *api)
{
    char from[17];
    g.api = api;
    g.prevKey = APP_KEY_INVALID;
    g.loc = api->loc();
    g.fromAprs = api->handoff_read(NULL, 0, from) >= 0 && from[0] == 'O' && from[3] == 'A';
    if (g.loc->attach) g.slot = (uint8_t)((g.loc->attach - 1u) & 3u);
    api->backlight_on();
    g.running = true;
    g.redraw = true;
    while (g.running) {
        keys();
        if (g.redraw) { g.redraw = false; draw(); }
        api->battery_sample();
        api->delay_ms(LOOP_MS);
        api->backlight_update();
    }
    if (g.changed) api->loc_save();
    if (g.fromAprs) api->handoff("OP APRS", NULL, 0);
}
