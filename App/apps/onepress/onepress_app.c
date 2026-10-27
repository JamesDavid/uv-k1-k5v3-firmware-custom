/*
 * OnePress - type a message with one key press per letter (UV-K1 / UV-K5 V3).
 *
 * The resident OnePress decoder (API level 4, APP_CAP_ONEPRESS) picks the
 * letters from the whole sentence: 4663 is "good", "home" or "gone", and the
 * words around it say which. Its word model sits in external flash, written
 * there by tools/onepress_upload.py. Code: github.com/JamesDavid/OnePress-Keyboard
 * (PolyForm Noncommercial 1.0.0).
 *
 * Keys:
 *   2-9      a letter (one press each, whichever of the key's letters)
 *   0 / 1    a space / punctuation (the decoder picks . , ? ! or an apostrophe)
 *   held     a digit key held down types the digit
 *   *        the next guess for the selected word (the last, unless UP/DOWN
 *            moved); on a word still being typed, take the completion shown
 *   UP/DOWN  select the word before / after, to fix it with *; typing or
 *            deleting goes back to the end
 *   F        ABC: spell exactly with multi-tap (call signs, names), F again back
 *   EXIT     delete; held, delete words; on an empty message, quit
 *   MENU     send: 1 APRS message, 2 CW, 3 clear
 *   PTT      send the way the message was last sent
 * The message survives leaving the app (until power off); a sender app hands
 * back here when it is done.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../app_api.h"

#define LOOP_MS     20u
#define HOLD_MS     600u    /* a held key: a digit, or words deleted       */
#define REPEAT_MS   300u    /* ... one more word every 300 ms              */
#define TAP_MS      1000u   /* multi-tap: the same key again cycles         */
#define MAX_LOCKS   8u
#define MAX_GUESS   8u
#define COLS        18      /* small font: 7 px a character                 */
#define ROWS        5u      /* lines 0-4 of the frame buffer                */
#define APRS_MAX    67u     /* an APRS message's text                       */
#define CFG_MAGIC   0x4Fu

static const char APRS_APP[] = "OP APRS";
static const char CW_APP[] = "OP CW";
/* Multi-tap letters per key 2-9 (each group ends with the digit). */
static const char *const TAPS[8] = { "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz" };
static const char KEY_OF[26] = "22233344455566677778889999";

enum { MODE_OP = 0, MODE_ABC = 1 };
enum { SCR_TYPE = 0, SCR_MENU = 1 };

static struct {
    const app_api_t *api;
    char *keys;                         /* the resident draft (api->op_keys)        */
    uint8_t n;                          /* its length                               */
    uint8_t mode, screen, sender;       /* sender: 1 APRS, 2 CW (PTT sends that way) */
    uint8_t key, held;                  /* the key down, and what holding it did    */
    uint8_t tapKey, tapAt;              /* multi-tap: key, letter index             */
    uint8_t nLocks, nGuess, guess;      /* guesses for the selected slot (0: not listed) */
    int16_t sel;                        /* the selected slot; -1: the last (typing)   */
    int16_t slots;                      /* op_decode's result                        */
    bool running, redraw, ok;
    uint32_t downAt, tapMs, repeatAt;
    app_op_lock_t locks[MAX_LOCKS];
    uint16_t ids[MAX_GUESS];
    char text[APP_OP_TEXT_MAX + 1];
    char line[34];
} g;
#define A (g.api)

/* GCC may turn small loops into these calls. */
void *memset(void *d, int c, size_t n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *p = d; const uint8_t *q = s; while (n--) *p++ = *q++; return d; }

static bool isLetter(char c) { return (c >= '2' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'J'); }
static uint8_t slen(const char *s) { uint8_t n = 0; while (s[n]) n++; return n; }
static char *put(char *o, const char *s) { while (*s) *o++ = *s++; return o; }
__attribute__((noinline)) static char *putu(char *o, uint32_t v) { return A->append_u32(o, v); }

/* ---- the decode ---- */

static void decode(void)
{
    g.keys[g.n] = 0;
    g.nGuess = 0;
    g.slots = A->op_decode(g.keys, g.locks, g.nLocks);
    g.text[0] = 0;
    if (g.slots >= 0 && A->op_render(g.text, sizeof g.text) < 0) g.text[0] = 0;
    uint8_t k = 0;                      /* locks past the end no longer apply */
    for (uint8_t i = 0; i < g.nLocks; i++) if ((int16_t)g.locks[i].slot < g.slots) g.locks[k++] = g.locks[i];
    g.nLocks = k;
    g.redraw = true;
}

static void lock(uint16_t slot, uint16_t id)
{
    uint8_t i = 0;
    while (i < g.nLocks && g.locks[i].slot != slot) i++;
    if (i == g.nLocks) {
        if (i == MAX_LOCKS) { memcpy(g.locks, g.locks + 1, sizeof g.locks - sizeof *g.locks); i--; }
        else g.nLocks++;
    }
    g.locks[i].slot = slot;
    g.locks[i].id = id;
}

static void unlockLast(void)
{
    uint8_t k = 0;
    for (uint8_t i = 0; i < g.nLocks; i++) if ((int16_t)g.locks[i].slot < g.slots - 1) g.locks[k++] = g.locks[i];
    g.nLocks = k;
}

static int16_t selected(void) { return g.sel >= 0 && g.sel < g.slots ? g.sel : g.slots - 1; }

static void add(char c)
{
    if (g.n >= APP_OP_KEYS_MAX) return;
    g.sel = -1;
    if (isLetter(c) && g.n && (isLetter(g.keys[g.n - 1]) || g.keys[g.n - 1] == '#'))
        unlockLast();                   /* the last word changes: its guess goes */
    g.keys[g.n++] = c;
    decode();
}

static void back(bool word)
{
    if (word) {                         /* trailing spaces and marks, then the word */
        while (g.n && !isLetter(g.keys[g.n - 1])) g.n--;
        while (g.n && isLetter(g.keys[g.n - 1])) g.n--;
    } else if (g.n) g.n--;
    g.tapKey = 0;
    g.sel = -1;
    decode();
}

/* The next (dir 1) or previous (-1) guess for the selected slot, best first. */
static void nextGuess(int8_t dir)
{
    if (g.slots <= 0) return;
    uint8_t slot = (uint8_t)selected();
    if (!g.nGuess) {
        g.nGuess = A->op_guesses(g.keys, g.locks, g.nLocks, slot, g.ids, MAX_GUESS);
        uint16_t cur = A->op_choice(slot, NULL);
        g.guess = 0;
        for (uint8_t i = 0; i < g.nGuess; i++) if (g.ids[i] == cur) g.guess = i;
    }
    if (g.nGuess < 2) return;
    g.guess = dir > 0 ? (g.guess + 1 >= g.nGuess ? 0 : g.guess + 1) : (g.guess ? g.guess - 1 : g.nGuess - 1);
    lock(slot, g.ids[g.guess]);
    uint8_t n = g.nGuess, at = g.guess;
    decode();
    g.nGuess = n;                       /* the list stays until the keys change */
    g.guess = at;
}

/* * on a word still being typed: take the completion shown (its keys, then a space). */
static bool accept(void)
{
    uint8_t kind;
    if (g.slots <= 0) return false;
    uint16_t slot = (uint16_t)(g.slots - 1), id = A->op_choice((uint8_t)slot, &kind);
    if (kind != APP_OP_PARTIAL) return false;
    char w[40];
    uint8_t wn = A->op_word(id, w, sizeof w), typed = 0, i = 0;
    while (typed < g.n && (isLetter(g.keys[g.n - 1 - typed]) || g.keys[g.n - 1 - typed] == '#')) typed++;
    for (uint8_t t = 0; i < wn && t < typed; i++) {        /* the letters already typed */
        if (w[i] == '\'' && g.keys[g.n - typed + t] != '#') continue;
        t++;
    }
    for (; i < wn && g.n < APP_OP_KEYS_MAX - 1; i++) {
        char c = w[i] >= 'A' && w[i] <= 'Z' ? (char)(w[i] + 32) : w[i];
        if (c >= 'a' && c <= 'z') g.keys[g.n++] = KEY_OF[c - 'a'];
        else if (c == '\'' || c == '-') g.keys[g.n++] = '#';
    }
    lock(slot, id);
    g.keys[g.n++] = '0';
    decode();
    return true;
}

/* ---- keys ---- */

static void digit(uint8_t d)
{
    if (d == 0) { g.tapKey = 0; add('0'); return; }
    if (g.mode == MODE_ABC && d >= 2) {         /* multi-tap */
        const char *t = TAPS[d - 2];
        if (g.tapKey == d && A->ticks_ms() - g.tapMs < TAP_MS && g.n) {
            g.tapAt = t[g.tapAt + 1] ? g.tapAt + 1 : 0;
            g.n--;
        } else g.tapAt = 0;
        g.tapKey = d;
        g.tapMs = A->ticks_ms();
        add(t[g.tapAt]);
        return;
    }
    g.tapKey = 0;
    add(d == 1 ? '#' : (char)('0' + d));
}

static void send(uint8_t how)
{
    if (!g.text[0]) return;
    memcpy(A->op_text, g.text, sizeof g.text);
    g.sender = how;
    if (A->handoff(how == 1 ? APRS_APP : CW_APP, NULL, 0)) g.running = false;
    else { g.screen = SCR_MENU; g.redraw = true; g.line[0] = 1; }  /* line[0] = 1: not installed */
}

static void press(uint8_t key)
{
    g.redraw = true;
    if (g.screen == SCR_MENU) {
        g.screen = SCR_TYPE;
        if (key == APP_KEY_1 || key == APP_KEY_2) send((uint8_t)(key - APP_KEY_0));
        else if (key == APP_KEY_3) { g.n = 0; g.nLocks = 0; decode(); }
        return;
    }
    if (key <= APP_KEY_9) { digit(key); return; }
    if (key != APP_KEY_F) g.tapKey = 0;
    int8_t dir = A->nav_dir(key);
    if (key == APP_KEY_STAR) { if (g.sel >= 0 || !accept()) nextGuess(1); }
    else if (dir && g.slots > 0) {              /* UP: the word before, DOWN: after */
        int16_t j = selected() - dir;
        g.sel = j >= g.slots - 1 ? -1 : j < 0 ? 0 : j;
        g.nGuess = 0;
    }
    else if (key == APP_KEY_F) { g.mode ^= MODE_ABC; g.tapKey = 0; }
    else if (key == APP_KEY_MENU) { g.screen = SCR_MENU; g.line[0] = 0; }
    else if (key == APP_KEY_PTT) send(g.sender ? g.sender : 1);
    else if (key == APP_KEY_EXIT && !g.n) g.running = false;
}

/* Called every loop with the key down (APP_KEY_INVALID for none). */
static void keys(void)
{
    uint8_t key = A->get_key();
    if (key == APP_KEY_SAVER) return;
    if (key == APP_KEY_WAKE) { g.redraw = true; return; }
    uint32_t now = A->ticks_ms();
    if (key != g.key) {
        if (g.key == APP_KEY_EXIT && !g.held && g.n && g.screen == SCR_TYPE) back(false);   /* EXIT let go */
        g.key = key;
        g.held = 0;
        g.downAt = now;
        if (key == APP_KEY_INVALID || key == APP_KEY_EXIT) return;   /* EXIT acts when let go or held */
        A->backlight_on();
        press(key);
        return;
    }
    if (key == APP_KEY_INVALID || now - g.downAt < HOLD_MS || g.screen != SCR_TYPE) return;
    if (key <= APP_KEY_9 && !g.held && g.n) {   /* held: the digit itself */
        g.held = 1;
        g.n--;
        g.tapKey = 0;
        add((char)('A' + key));
    } else if (key == APP_KEY_EXIT && g.n && (!g.held || now - g.repeatAt >= REPEAT_MS)) {
        g.held = 1;
        g.repeatAt = now;
        back(true);
    }
}

/* ---- display ---- */

static void tiny(uint8_t x, uint8_t y, char *end) { *end = 0; A->print_tiny(g.line, x, y, false, true); }

static void draw(void)
{
    A->display_clear();
    A->status_clear();
    A->print_inverse("ONEPRESS", 2, 0, true, true, 2 + 8 * 4);
    if (g.mode == MODE_ABC) A->print_inverse("ABC", 40, 0, true, true, 40 + 3 * 4);
    A->draw_battery();
    if (!g.ok) {
        A->print_normal("No word model", 0, 127, 1);
        A->print_tiny("UPLOAD IT FROM A PC WITH", 16, 30, false, true);
        A->print_tiny("TOOLS/ONEPRESS_UPLOAD.PY", 16, 38, false, true);
    } else if (g.screen == SCR_MENU) {
        A->print_normal(g.line[0] ? "App missing" : "Send", 0, 127, 0);
        A->print_normal("1 APRS message", 4, 0, 2);
        A->print_normal("2 CW", 4, 0, 3);
        A->print_normal("3 Clear", 4, 0, 4);
    } else {
        /* the text, wrapped at spaces, its last ROWS lines; the selected word underlined */
        uint8_t start[24], nl = 0, len = slen(g.text), s = 0;
        while (nl < sizeof start) {
            start[nl++] = s;
            if (len - s <= (int)COLS) break;
            uint8_t e = s + COLS;
            while (e > s && g.text[e] != ' ') e--;
            s = (uint8_t)(e > s ? e + 1 : s + COLS);
        }
        /* where the selected slot is: words (letters, digits, ' - : and a . or , between
         * digits) and marks are slots in order, spaces aren't */
        uint8_t first = nl > ROWS ? nl - ROWS : 0, word = len, wend = len;
        int16_t want = selected();
        for (uint8_t i = 0, j = 0; i < len;) {
            char ch = g.text[i];
            if (ch == ' ') { i++; continue; }
            uint8_t e = i + 1;
            if (ch != '.' && ch != ',' && ch != '?' && ch != '!')
                while (e < len && g.text[e] != ' ' && g.text[e] != '?' && g.text[e] != '!' &&
                       !((g.text[e] == '.' || g.text[e] == ',') && !(e + 1 < len && g.text[e + 1] >= '0' && g.text[e + 1] <= '9')))
                    e++;
            if (j++ == want) { word = i; wend = e; break; }
            i = e;
        }
        for (uint8_t l = first; l < nl; l++) {
            uint8_t a = start[l], b = l + 1 < nl ? start[l + 1] : len, c = 0;
            while (a + c < b && c < COLS) { g.line[c] = g.text[a + c]; c++; }
            g.line[c] = 0;
            A->print_normal(g.line, 0, 0, l - first);
            if (word < len && word >= a && word < b) {
                uint8_t x = (uint8_t)((word - a) * 7), e = (uint8_t)(((wend < b ? wend : b) - a) * 7);
                A->draw_line(A->fb, x, (int16_t)((l - first) * 8 + 7), (int16_t)(e - 2), (int16_t)((l - first) * 8 + 7), true);
            }
        }
        /* bottom: length (of the 67 an APRS message takes), guess number, completion */
        char *o = putu(g.line, len);
        o = put(o, len > APRS_MAX ? "/67 LONG" : "/67");
        if (g.nGuess > 1) { o = put(o, "  GUESS "); o = putu(o, g.guess + 1u); *o++ = '/'; o = putu(o, g.nGuess); }
        uint8_t kind = APP_OP_WORD;
        if (g.slots > 0) A->op_choice((uint8_t)(g.slots - 1), &kind);
        if (g.sel >= 0) o = put(o, "  * FIXES");
        else if (kind == APP_OP_PARTIAL) o = put(o, "  * TAKES IT");
        tiny(0, 50, o);
    }
    A->blit_status();
    A->blit_full();
}

__attribute__((section(".text.entry"), used))
void app_main(const app_api_t *api)
{
    uint8_t c[2];
    g.api = api;
    g.keys = api->op_keys;
    g.key = APP_KEY_INVALID;
    g.sel = -1;
    g.ok = api->op_open() == 0;
    api->cfg_load(c, sizeof c);
    if (c[0] == CFG_MAGIC && c[1] <= 2) g.sender = c[1];
    uint8_t sent = 0;                   /* back from a sender: 1 when it sent the message */
    if (api->handoff_read(&sent, 1, NULL) >= 1 && sent == 1) g.keys[0] = 0;
    while (g.n < APP_OP_KEYS_MAX && g.keys[g.n]) g.n++;
    if (g.ok) decode();
    api->backlight_on();
    g.running = true;
    g.redraw = true;
    while (g.running) {
        keys();
        if (!g.running) break;
        if (g.redraw) { g.redraw = false; draw(); }
        api->battery_sample();
        api->delay_ms(LOOP_MS);
        api->backlight_update();
    }
    g.keys[g.n] = 0;
    c[0] = CFG_MAGIC;
    c[1] = g.sender;
    api->cfg_save(c, sizeof c);
}
