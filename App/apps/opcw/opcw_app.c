/*
 * OP CW - sends the message typed in OnePress in Morse on the TX VFO: a tone
 * over FM (MCW, heard by any FM radio) or, in CARR mode, the carrier keyed as
 * well (heard as CW by an SSB/CW receiver). Keying as the Beacon app's
 * (Copyright 2026 Armel F4HWN, Apache License 2.0).
 *
 * Keys: PTT or MENU send · EXIT stop sending, or back to OnePress · UP/DOWN or
 *   2/8 speed (5-40 WPM) · 3/9 tone (400-1200 Hz) · 1 MCW / CARR.
 * Settings: speed, tone and mode.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../app_api.h"

#define LEAD_MS     300u    /* carrier up before the first element */
#define LOOP_MS     40u
#define CFG_MAGIC   0xC7u
#define TEXT_MAX    APP_OP_TEXT_MAX

/* Morse with a leading 1, then the elements MSB first (0 dit, 1 dah): A-Z, 0-9. */
static const uint8_t MORSE[36] = {
    0x05, 0x18, 0x1A, 0x0C, 0x02, 0x12, 0x0E, 0x10, 0x04, 0x17, 0x0D, 0x14, 0x07,
    0x06, 0x0F, 0x16, 0x1D, 0x0A, 0x08, 0x03, 0x09, 0x11, 0x0B, 0x19, 0x1B, 0x1C,
    0x3F, 0x2F, 0x27, 0x23, 0x21, 0x20, 0x30, 0x38, 0x3C, 0x3E,
};
static const char MARKS[] = "./?,-'!";
static const uint8_t MARK_CODE[] = { 0x55, 0x32, 0x4C, 0x73, 0x61, 0x5E, 0x6B };

static struct {
    const app_api_t *api;
    uint8_t prevKey, wpm, carr, sending, done;
    uint16_t tone, unit;
    bool running, redraw, stop, denied;
    char str[34];
} g;
#define A (g.api)

void *memset(void *d, int c, size_t n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *p = d; const uint8_t *q = s; while (n--) *p++ = *q++; return d; }

static char *put(char *o, const char *s) { while (*s) *o++ = *s++; return o; }
__attribute__((noinline)) static char *putu(char *o, uint32_t v) { return A->append_u32(o, v); }

static uint8_t code(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c >= 'A' && c <= 'Z') return MORSE[c - 'A'];
    if (c >= '0' && c <= '9') return MORSE[26 + c - '0'];
    for (uint8_t i = 0; MARKS[i]; i++) if (MARKS[i] == c) return MARK_CODE[i];
    return 0;
}

static void draw(void)
{
    char *o;
    A->display_clear();
    A->status_clear();
    A->print_inverse("CW", 2, 0, true, true, 2 + 2 * 4);
    if (g.sending) A->print_inverse("TRANSMIT", 40, 0, true, true, 40 + 8 * 4);
    A->draw_battery();
    const char *t = A->op_text;
    for (uint8_t r = 0, i = 0; r < 4u && t[i]; r++) {          /* the message, sent part inverted */
        o = g.str;
        uint8_t x = 0;
        for (uint8_t c = 0; c < 32u && t[i] && i < TEXT_MAX; c++) *o++ = t[i++];
        *o = 0;
        A->print_tiny(g.str, x, (uint8_t)(2u + r * 7u), false, true);
    }
    o = putu(g.str, g.wpm);
    o = put(o, " WPM  ");
    o = putu(o, g.tone);
    o = put(o, " HZ  ");
    o = put(o, g.carr ? "CARR" : "MCW");
    *o = 0;
    A->print_tiny(g.str, 0, 30, false, true);
    A->draw_frequency(A->tx_freq(), 0, 4);
    o = g.str;
    if (g.denied) o = put(o, "TX NOT ALLOWED HERE");
    else if (g.sending) { o = put(o, "SENDING "); o = putu(o, g.done); o = put(o, "  EXIT STOPS"); }
    else o = put(o, "PTT SEND  2/8 WPM 3/9 HZ 1 MODE");
    *o = 0;
    A->print_tiny(g.str, 0, 50, false, true);
    A->blit_status();
    A->blit_full();
}

/* Wait ms, a little at a time; EXIT stops sending. */
static void wait(uint16_t ms)
{
    while (ms && !g.stop) {
        uint16_t s = ms > 10u ? 10u : ms;
        A->delay_ms(s);
        ms = (uint16_t)(ms - s);
        A->backlight_update();
        if (A->get_key() == APP_KEY_EXIT) g.stop = true;
    }
}
static void keyOn(void) { A->tx_mute(false); if (g.carr) A->tx_carrier(true); }
static void keyOff(void) { A->tx_mute(true); if (g.carr) A->tx_carrier(false); }

static void send(void)
{
    const char *t = A->op_text;
    g.denied = A->tx_state() != 0;
    if (g.denied || !t[0]) return;
    g.unit = (uint16_t)A->uidivmod(1200u, g.wpm);   /* PARIS: a dit is 1200 / WPM ms */
    g.sending = 1;
    g.stop = false;
    g.done = 0;
    draw();
    A->tx_set_params();
    A->tx_tone(g.tone);
    A->tx_mute(true);
    wait(LEAD_MS);
    if (g.carr) A->tx_carrier(false);
    for (uint8_t i = 0; t[i] && !g.stop; i++) {
        uint8_t c = code(t[i]);
        if (!c) wait((uint16_t)(g.unit * 4u));       /* a space: 7 units with the 3 after a letter */
        else {
            uint8_t bit = 0x80;
            while (!(c & bit)) bit >>= 1;
            for (bit >>= 1; bit && !g.stop; bit >>= 1) {
                keyOn();
                wait((uint16_t)((c & bit) ? g.unit * 3u : g.unit));
                keyOff();
                wait(g.unit);
            }
            wait((uint16_t)(g.unit * 2u));
        }
        g.done = (uint8_t)(i + 1);
        if ((i & 7u) == 7u) draw();
    }
    A->tx_mute(true);
    A->tx_end();
    g.sending = 0;
    if (!g.stop) g.done = 0xFF;
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
    if (key == APP_KEY_2) d = 1;
    if (key == APP_KEY_8) d = -1;
    g.denied = false;
    if (key == APP_KEY_EXIT) g.running = false;
    else if (key == APP_KEY_MENU || key == APP_KEY_PTT) { send(); g.prevKey = A->get_key(); }
    else if (d) { int16_t w = (int16_t)(g.wpm + d); if (w >= 5 && w <= 40) g.wpm = (uint8_t)w; }
    else if (key == APP_KEY_3 && g.tone < 1200u) g.tone = (uint16_t)(g.tone + 50u);
    else if (key == APP_KEY_9 && g.tone > 400u) g.tone = (uint16_t)(g.tone - 50u);
    else if (key == APP_KEY_1) g.carr ^= 1u;
}

__attribute__((section(".text.entry"), used))
void app_main(const app_api_t *api)
{
    uint8_t c[5];
    g.api = api;
    g.prevKey = APP_KEY_INVALID;
    g.wpm = 18;
    g.tone = 700;
    api->handoff_read(NULL, 0, NULL);
    api->cfg_load(c, sizeof c);
    if (c[0] == CFG_MAGIC && c[1] >= 5 && c[1] <= 40) {
        g.wpm = c[1];
        uint16_t t = (uint16_t)(c[2] | c[3] << 8);
        if (t >= 400u && t <= 1200u) g.tone = t;
        g.carr = c[4] & 1u;
    }
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
    c[0] = CFG_MAGIC; c[1] = g.wpm; c[2] = (uint8_t)g.tone; c[3] = (uint8_t)(g.tone >> 8); c[4] = g.carr;
    api->cfg_save(c, sizeof c);
    const uint8_t sent = g.done == 0xFF ? 1u : 0u;  /* back to OnePress: 1 clears the message */
    api->handoff("OnePress", &sent, 1);
}
