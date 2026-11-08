/*
 * OP Inbox - the APRS messages to this station that the radio heard in the
 * background while OnePress (or OP APRS, OP Location) ran: newest first, the last 3
 * since power on. Opening one marks it read.
 *
 * Keys: UP/DOWN the next / previous message · MENU or PTT reply: OnePress,
 *   with the sender as the addressee of OP APRS · EXIT back to OnePress (or to
 *   the menu when started from it) · * the receiver's diagnostics line on/off
 *   (saved) · F listening on the main screen too (ALWAYS) or only in these
 *   apps (saved with the locations).
 * The receiver keeps listening while this app is open (API level 5).
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../app_api.h"

#define LOOP_MS     40u

static struct {
    const app_api_t *api;
    uint8_t prevKey, cur, count, unread;
    bool running, redraw, fromOnePress;
    uint32_t statAt, statSamples;
    app_rx_stats_t st;
    uint16_t rate;
    uint8_t diag;           /* the diagnostics line shown (cfg: 1 on; erased 0xFF: off) */
    char str[34];
} g;
#define A (g.api)

void *memset(void *d, int c, size_t n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }

static char *put(char *o, const char *s) { while (*s) *o++ = *s++; return o; }
static void tiny(uint8_t y, char *end) { *end = 0; A->print_tiny(g.str, 0, y, false, true); }

static void draw(void)
{
    char *o;
    A->display_clear();
    A->status_clear();
    A->print_inverse("INBOX", 2, 0, true, true, 2 + 5 * 4);
    A->draw_battery();
    app_rx_msg_t *m = A->rx_msg(g.cur);
    if (m) {
        o = A->append_u32(g.str, g.cur + 1u);
        *o++ = '/';
        o = A->append_u32(o, g.count);
        *o = 0;
        A->print_inverse(g.str, 28, 0, true, true, 28 + 3 * 4);
        A->print_bold(m->from, 0, 0, 0);
        if (m->unread) A->print_normal("NEW", 104, 0, 0);
        m->unread = 0;
        /* the text, 32 tiny characters a row */
        const char *t = m->text;
        for (uint8_t r = 0, i = 0; r < 3u && t[i]; r++) {
            o = g.str;
            for (uint8_t c = 0; c < 32u && t[i]; c++) *o++ = t[i++];
            tiny((uint8_t)(12u + r * 7u), o);
        }
        o = put(g.str, "MENU REPLY  < > MORE  EXIT BACK");
    } else {
        A->print_normal("No messages yet", 0, 127, 1);
        o = put(g.str, "EXIT BACK");
    }
    tiny(50, o);
    /* who the radio listens for */
    char call[7];
    uint8_t ssid;
    if (A->rx_station(call, &ssid)) {
        o = put(g.str, "LISTENING FOR ");
        o = put(o, call);
        if (ssid & 15u) { *o++ = '-'; o = A->append_u32(o, ssid & 15u); }   /* bits 4-5: the ack path */
        if (!(A->loc()->flags & APP_LOC_F_NO_MAIN)) o = put(o, " ALWAYS");
    } else o = put(g.str, "NOT LISTENING: OPEN ONEPRESS");
    tiny(38, o);
    /* the receiver at work: samples a second (9600), packets from anyone, the
     * audio's lowest-highest code, the battery code */
    if (g.diag == 1u) {
    o = A->append_u32(g.str, g.rate);
    o = put(o, "/S ");
    o = A->append_u32(o, g.st.frames);
    o = put(o, "PKT A");
    o = A->append_u32(o, g.st.lo > g.st.hi ? 0u : g.st.lo);
    *o++ = '-';
    o = A->append_u32(o, g.st.hi);
    o = put(o, " B");
    o = A->append_u32(o, g.st.battery);
    if (g.st.state == 2u) o = put(o, " P");
    tiny(44, o);
    }
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
    int8_t d = A->nav_dir(key);
    if (key == APP_KEY_EXIT) {
        if (g.fromOnePress) A->handoff("OnePress", NULL, 0);
        g.running = false;
    } else if ((key == APP_KEY_MENU || key == APP_KEY_PTT) && g.count) {
        const char *from = A->rx_msg(g.cur)->from;
        uint8_t n = 0;
        while (from[n]) n++;
        if (A->handoff("OnePress", from, (uint8_t)(n + 1u))) g.running = false;
    } else if (key == APP_KEY_F) {               /* the main screen too, or not */
        A->loc()->flags ^= APP_LOC_F_NO_MAIN;
        A->loc_save();
    } else if (key == APP_KEY_STAR) {
        g.diag = g.diag == 1u ? 0u : 1u;
    } else if (d && g.count) {
        g.cur = (uint8_t)(d > 0 ? (g.cur + 1u < g.count ? g.cur + 1u : 0u) : (g.cur ? g.cur - 1u : g.count - 1u));
    }
}

__attribute__((section(".text.entry"), used))
void app_main(const app_api_t *api)
{
    char from[17];
    g.api = api;
    g.prevKey = APP_KEY_INVALID;
    g.fromOnePress = api->handoff_read(NULL, 0, from) >= 0 && from[0] == 'O' && from[1] == 'n';
    api->cfg_load(&g.diag, 1);
    api->backlight_on();
    g.running = true;
    g.redraw = true;
    while (g.running) {
        keys();
        uint8_t n = api->rx_count(), u = api->rx_unread();
        if (n != g.count || u > g.unread) {      /* a new one: show it */
            g.cur = 0;
            g.count = n;
            g.redraw = true;
        }
        uint32_t now = api->ticks_ms();
        if (now - g.statAt >= 1000u) {          /* the diagnostics line, every second */
            api->rx_stats(&g.st);
            g.rate = (uint16_t)((g.st.samples - g.statSamples) * 1000u / (now - g.statAt));
            g.statSamples = g.st.samples;
            g.statAt = now;
            g.redraw |= g.diag == 1u;
        }
        if (g.redraw) { g.redraw = false; draw(); g.unread = api->rx_unread(); }
        api->battery_sample();
        api->delay_ms(LOOP_MS);
        api->backlight_update();
    }
    api->cfg_save(&g.diag, 1);
}
