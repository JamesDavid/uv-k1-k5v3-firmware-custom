/*
 * OP APRS - sends the message typed in OnePress as an APRS message (AX.25 UI
 * frame, Bell 202 AFSK 1200 bauds) on the TX VFO: "SRC>APZOP,path::TO       :text".
 * No message number, so no acknowledgement is expected (this app does not
 * receive). APRS-IS iGates forward it to aprs.fi and to the addressee.
 *
 * The AX.25 frame, its FCS and the AFSK keying (the BK4829 tone generator's
 * frequency rewritten at each NRZI transition, bits timed from SysTick) are
 * those of the APRS TX app, Copyright 2026 Armel F4HWN, Apache License 2.0.
 *
 * Keys: PTT or MENU send · 1 edit the addressee · 2 edit your callsign (empty:
 *   the boot message's) · 3 SSID · 4 path (DIRECT, WIDE1-1, WIDE1-1,WIDE2-1) ·
 *   EXIT back to OnePress. Editing: 2-9 multi-tap A-Z and the digit, held: the
 *   digit; 0 and 1 digits (1 again: '-'), EXIT deletes, MENU done.
 * Settings (16 bytes): your callsign, the addressee, SSID and path.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../app_api.h"

#ifndef SYST_VAL            /* SysTick (core at 48 MHz, 10 ms period); a simulator brings its own */
#define SYST_LOAD   (*(volatile uint32_t *)0xE000E014u)
#define SYST_VAL    (*(volatile uint32_t *)0xE000E018u)
#endif
#define CYC_PER_BIT (48000000u / 1200u)
#define REG_51      0x51
#define REG_70      0x70
#define REG_71      0x71
#define F_MARK      12389u
#define F_SPACE     22714u
#define LVL         66u     /* APRS TX's default tone level: decoded first try */
#define TXDELAY_FLAGS 40u
#define TAIL_FLAGS  3u
#define TEXT_MAX    67u     /* APRS message text */
#define CALL_MAX    6u
#define TO_MAX      9u
#define FRAME_MAX   (7u * 4u + 2u + 11u + TEXT_MAX + 2u)
#define LOOP_MS     40u
#define TAP_MS      1000u
#define HOLD_MS     600u    /* a key held while editing: its digit */

enum { ST_IDLE, ST_TX, ST_SENT, ST_DENIED, ST_NOCALL, ST_NOTO };
enum { ED_NONE, ED_TO, ED_CALL };

static const char DEST[] = "APZOP";             /* APZ: experimental software */
static const char *const PATHS[3] = { "", "WIDE1-1", "WIDE1-1,WIDE2-1" };
static const char *const TAPS[10] = { "0", "1-", "ABC2", "DEF3", "GHI4", "JKL5", "MNO6", "PQRS7", "TUV8", "WXYZ9" };

static struct {
    const app_api_t *api;
    uint8_t flen, prevKey, space, ones, status, edit, tapKey, tapAt, sent;
    bool running, redraw;
    uint32_t tPrev, tCyc, next, tapMs, downAt;
    bool held;
    uint8_t cfg[16];        /* call[6], to[9], route: 0x80 (saved) | path << 4 | SSID */
    uint8_t frm[FRAME_MAX];
    char str[34];
} g;
#define A (g.api)
#define CALL (g.cfg)
#define TO (g.cfg + CALL_MAX)
#define ROUTE (g.cfg[15])

void *memset(void *d, int c, size_t n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *p = d; const uint8_t *q = s; while (n--) *p++ = *q++; return d; }

static uint8_t flen(const uint8_t *s, uint8_t max) { uint8_t n = 0; while (n < max && s[n]) n++; return n; }
static char *put(char *o, const char *s) { while (*s) *o++ = *s++; return o; }
__attribute__((noinline)) static char *putu(char *o, uint32_t v) { return A->append_u32(o, v); }
static bool callChar(char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

/* The source callsign: the one set here, else the boot message's. */
static uint8_t myCall(char *c)
{
    uint8_t n = flen(CALL, CALL_MAX);
    if (n) memcpy(c, CALL, n);
    else { A->boot_callsign(c, 12); while (c[n]) n++; }
    if (n > CALL_MAX) return 0;
    for (uint8_t i = 0; i < n; i++) if (!callChar(c[i])) return 0;
    return n;
}

/* An AX.25 address: 6 characters shifted left, then the SSID byte. */
static uint8_t *addr(uint8_t *f, const char *c, uint8_t n, uint8_t ssid, bool last)
{
    for (uint8_t i = 0; i < 6u; i++) *f++ = (uint8_t)((i < n ? c[i] : ' ') << 1);
    *f++ = (uint8_t)(0x60u | (ssid << 1) | (last ? 1u : 0u));
    return f;
}

static void build(void)
{
    char c[12];
    uint8_t n = myCall(c), path = (uint8_t)((ROUTE >> 4) & 7u), *f = g.frm;
    g.flen = 0;
    if (!n) { g.status = ST_NOCALL; return; }
    if (!flen(TO, TO_MAX)) { g.status = ST_NOTO; return; }
    f = addr(f, DEST, 5, 0, false);
    f = addr(f, c, n, (uint8_t)(ROUTE & 15u), path == 0);
    if (path) f = addr(f, "WIDE1", 5, 1, path == 1);
    if (path == 2) f = addr(f, "WIDE2", 5, 1, true);
    *f++ = 0x03; *f++ = 0xF0;                       /* UI frame, no layer 3 */
    *f++ = ':';
    for (uint8_t i = 0; i < TO_MAX; i++) *f++ = TO[i] ? TO[i] : ' ';
    *f++ = ':';
    const char *t = A->op_text;
    for (uint8_t i = 0; i < TEXT_MAX && t[i]; i++)  /* | ~ { are not allowed in a message */
        *f++ = (uint8_t)(t[i] == '|' || t[i] == '~' || t[i] == '{' ? ' ' : t[i] < ' ' || t[i] > '~' ? '?' : t[i]);
    uint8_t k = (uint8_t)(f - g.frm);
    uint16_t crc = 0xFFFFu;                         /* CRC-16/X.25 */
    for (uint8_t i = 0; i < k; i++) {
        crc ^= g.frm[i];
        for (uint8_t b = 0; b < 8u; b++) crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408u) : (uint16_t)(crc >> 1);
    }
    crc ^= 0xFFFFu;
    g.frm[k++] = (uint8_t)crc;
    g.frm[k++] = (uint8_t)(crc >> 8);
    g.flen = k;
}

/* ---- AFSK, as APRS TX ---- */
static void clkStart(void) { g.tPrev = SYST_VAL; g.tCyc = 0; }
static uint32_t clkCyc(void)
{
    uint32_t v = SYST_VAL;
    g.tCyc += (v <= g.tPrev) ? g.tPrev - v : g.tPrev + (SYST_LOAD + 1u - v);
    g.tPrev = v;
    return g.tCyc;
}
static void setTone(void)
{
    A->bk_write(REG_71, (uint16_t)(g.space ? F_SPACE : F_MARK));
    A->bk_write(REG_70, (uint16_t)(0x8000u | (LVL << 8)));
}
static void sendBit(uint8_t b)
{
    while ((int32_t)(clkCyc() - g.next) < 0) {}
    if (!b) { g.space ^= 1u; setTone(); }
    g.next += CYC_PER_BIT;
}
static void sendByte(uint8_t v, bool stuff)
{
    for (uint8_t i = 0; i < 8u; i++) {
        uint8_t b = (uint8_t)(v & 1u);
        v >>= 1;
        sendBit(b);
        if (stuff) {
            if (!b) g.ones = 0;
            else if (++g.ones == 5u) { sendBit(0); g.ones = 0; }
        }
    }
}

static void draw(void);
static void send(void)
{
    build();
    if (!g.flen) return;
    if (A->tx_state()) { g.status = ST_DENIED; return; }
    g.status = ST_TX;
    draw();
    A->tx_set_params();
    A->bk_write(REG_51, 0);                         /* no CTCSS/DCS under the AFSK */
    A->tx_tone(1200);
    g.space = 0;
    setTone();
    clkStart();
    g.next = CYC_PER_BIT;
    for (uint8_t i = 0; i < TXDELAY_FLAGS; i++) sendByte(0x7E, false);
    g.ones = 0;
    for (uint8_t i = 0; i < g.flen; i++) sendByte(g.frm[i], true);
    for (uint8_t i = 0; i < TAIL_FLAGS; i++) sendByte(0x7E, false);
    while ((int32_t)(clkCyc() - g.next) < 0) {}
    A->tx_mute(true);
    A->tx_end();
    if (g.sent < 99u) g.sent++;
    g.status = ST_SENT;
}

/* ---- display ---- */
static void tiny(uint8_t y, char *end) { *end = 0; A->print_tiny(g.str, 0, y, false, true); }

static void draw(void)
{
    char *o;
    A->display_clear();
    A->status_clear();
    A->print_inverse("APRS MSG", 2, 0, true, true, 2 + 8 * 4);
    if (g.status == ST_TX) A->print_inverse("TRANSMIT", 40, 0, true, true, 40 + 8 * 4);
    A->draw_battery();
    o = put(g.str, g.edit == ED_CALL ? "From " : "To ");
    const uint8_t *fld = g.edit == ED_CALL ? CALL : TO;
    uint8_t n = flen(fld, g.edit == ED_CALL ? CALL_MAX : TO_MAX);
    memcpy(o, fld, n);
    o += n;
    if (g.edit) *o++ = '_';
    *o = 0;
    A->print_bold(g.str, 0, 0, 0);
    /* the message, 32 tiny characters a row */
    const char *t = A->op_text;
    for (uint8_t r = 0, i = 0; r < 3u && t[i]; r++) {
        o = g.str;
        for (uint8_t c = 0; c < 32u && t[i] && i < TEXT_MAX; c++) *o++ = t[i++];
        tiny((uint8_t)(10u + r * 6u), o);
    }
    char c[12];
    o = put(g.str, "FROM ");
    n = myCall(c);
    memcpy(o, c, n); o += n;
    if (ROUTE & 15u) { *o++ = '-'; o = putu(o, ROUTE & 15u); }
    if ((ROUTE >> 4) & 7u) { *o++ = ' '; o = put(o, PATHS[(ROUTE >> 4) & 7u]); }
    tiny(28, o);
    A->draw_frequency(A->tx_freq(), 0, 4);
    o = g.str;
    if (g.edit) o = put(o, "2-9 ABC  EXIT DEL  MENU OK");
    else if (g.status == ST_DENIED) o = put(o, "TX NOT ALLOWED HERE");
    else if (g.status == ST_NOCALL) o = put(o, "SET YOUR CALLSIGN: KEY 2");
    else if (g.status == ST_NOTO) o = put(o, "SET THE ADDRESSEE: KEY 1");
    else if (g.status == ST_SENT) { o = put(o, "SENT "); o = putu(o, g.sent); o = put(o, "  PTT AGAIN"); }
    else o = put(o, "PTT SEND 1TO 2CALL 3SSID 4PATH");
    tiny(50, o);
    A->blit_status();
    A->blit_full();
}

/* ---- keys ---- */
static void editKey(uint8_t key)
{
    uint8_t *fld = g.edit == ED_CALL ? CALL : TO, max = g.edit == ED_CALL ? CALL_MAX : TO_MAX, n = flen(fld, max);
    if (key == APP_KEY_MENU) { g.edit = ED_NONE; return; }
    if (key == APP_KEY_EXIT) { if (n) fld[n - 1] = 0; else g.edit = ED_NONE; g.tapKey = 0xFF; return; }
    if (key > APP_KEY_9) return;
    const char *t = TAPS[key];
    if (g.tapKey == key && A->ticks_ms() - g.tapMs < TAP_MS && n) {
        g.tapAt = t[g.tapAt + 1] ? (uint8_t)(g.tapAt + 1) : 0;
        n--;
    } else g.tapAt = 0;
    g.tapKey = key;
    g.tapMs = A->ticks_ms();
    if (g.edit == ED_CALL && !callChar(t[g.tapAt])) g.tapAt = 0;   /* no '-' in a callsign */
    if (n < max) fld[n] = (uint8_t)t[g.tapAt];
}

static void keys(void)
{
    uint8_t key = A->get_key();
    if (key == APP_KEY_SAVER) return;
    if (key != APP_KEY_INVALID && key == g.prevKey && g.edit && key <= APP_KEY_9 && !g.held &&
        A->ticks_ms() - g.downAt >= HOLD_MS) {     /* held while editing: the digit */
        uint8_t *fld = g.edit == ED_CALL ? CALL : TO, n = flen(fld, g.edit == ED_CALL ? CALL_MAX : TO_MAX);
        if (n) fld[n - 1] = (uint8_t)('0' + key);
        g.held = true;
        g.tapKey = 0xFF;
        g.redraw = true;
        return;
    }
    if (key == APP_KEY_INVALID || key == g.prevKey) { g.prevKey = key; return; }
    g.downAt = A->ticks_ms();
    g.held = false;
    g.prevKey = key;
    g.redraw = true;
    A->backlight_on();
    if (key == APP_KEY_WAKE) return;
    if (g.edit) { editKey(key); return; }
    g.status = ST_IDLE;
    if (key == APP_KEY_EXIT) g.running = false;
    else if (key == APP_KEY_MENU || key == APP_KEY_PTT) send();
    else if (key == APP_KEY_1) { g.edit = ED_TO; g.tapKey = 0xFF; }
    else if (key == APP_KEY_2) { g.edit = ED_CALL; g.tapKey = 0xFF; }
    else if (key == APP_KEY_3) ROUTE = (uint8_t)((ROUTE & 0xF0u) | ((ROUTE + 1u) & 15u));
    else if (key == APP_KEY_4) ROUTE = (uint8_t)(0x80u | (ROUTE & 15u) | (((ROUTE >> 4) & 7u) >= 2u ? 0u : (((ROUTE >> 4) & 7u) + 1u) << 4));
}

__attribute__((section(".text.entry"), used))
void app_main(const app_api_t *api)
{
    g.api = api;
    g.prevKey = APP_KEY_INVALID;
    api->handoff_read(NULL, 0, NULL);
    api->cfg_load(g.cfg, sizeof g.cfg);
    for (uint8_t i = 0; i < 15u; i++) if (g.cfg[i] && !callChar((char)g.cfg[i]) && g.cfg[i] != '-') g.cfg[i] = 0;
    if (!(ROUTE & 0x80u) || ((ROUTE >> 4) & 7u) > 2u) ROUTE = 0x97u;   /* first use: SSID 7, WIDE1-1 */
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
    api->cfg_save(g.cfg, sizeof g.cfg);
    const uint8_t sent = g.sent ? 1u : 0u;          /* back to OnePress: 1 clears the message */
    api->handoff("OnePress", &sent, 1);
}
