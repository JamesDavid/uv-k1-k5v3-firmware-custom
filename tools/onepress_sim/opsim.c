/*
 * opsim - runs the OnePress, OP APRS and OP CW overlay apps on a PC against a
 * simulated app_api_t: the real resident service (App/app/onepress_svc.c) and
 * decoder, the model file as the external flash at APP_OP_MODEL_ADDR, keys from
 * a script, the screen printed as text, and every APRS transmission decoded
 * back from its tones (NRZI, bit stuffing, FCS) into TNC2 text.
 *
 *   opsim model.bin "keys..."
 *
 * Keys, separated by spaces: 0-9 * F M (MENU) X (EXIT) U D (up, down) P (PTT),
 * each pressed for 80 ms, or held with :ms (6:700); w:ms waits; s prints the
 * screen. Build: tools/onepress_sim/build.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "apps/app_api.h"
#include "app/onepress_svc.h"

void onepress_main(const app_api_t *api);
void opaprs_main(const app_api_t *api);
void opcw_main(const app_api_t *api);

/* ---- the external flash: the model file at APP_OP_MODEL_ADDR ---- */
static uint8_t *model;
static size_t model_size;
static unsigned long flash_reads;
void PY25Q16_ReadBuffer(uint32_t a, void *buf, uint32_t n)
{
    flash_reads++;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t o = a + i - APP_OP_MODEL_ADDR;
        ((uint8_t *)buf)[i] = a + i >= APP_OP_MODEL_ADDR && o < model_size ? model[o] : 0xFF;
    }
}

/* ---- time: ms for the loop, 48 MHz cycles for the AFSK ---- */
static uint64_t cyc;                        /* 48 MHz core cycles since start */
static uint32_t now_ms(void) { return (uint32_t)(cyc / 48000u); }
uint32_t sim_systval(void)                  /* SysTick counts down from 479999, 10 ms a turn */
{
    cyc += 41;
    return 479999u - (uint32_t)(cyc % 480000u);
}

/* ---- keys from the script ---- */
typedef struct { uint8_t key; uint32_t at, until; } press_t;
static press_t presses[512];
static int n_presses, shown;
static uint32_t screen_at[64];
static int n_screens;
static uint8_t get_key(void)
{
    uint32_t t = now_ms();
    for (int i = 0; i < n_presses; i++) if (t >= presses[i].at && t < presses[i].until) return presses[i].key;
    return APP_KEY_INVALID;
}
static uint32_t script_end;
static jmp_buf done;            /* the script is over: back to main */

/* ---- the screen as text ---- */
static char rows[8][40][64];                /* per frame-buffer line: drawn strings */
static int n_rows[8];
static char status[256];
static int underline[8][2], has_ul[8];
static void clear(void) { memset(n_rows, 0, sizeof n_rows); memset(has_ul, 0, sizeof has_ul); }
static void add_row(int line, int x, const char *s)
{
    if (line < 0 || line > 7 || n_rows[line] >= 40) return;
    snprintf(rows[line][n_rows[line]++], 64, "%*s%s", x / 7, "", s);
}
static void dump(void)
{
    printf("+----------------------------------+  t=%u ms\n| %-32.32s |\n", now_ms(), status);
    for (int l = 0; l < 7; l++) {
        char out[200] = "";
        for (int i = 0; i < n_rows[l]; i++) { if (i) strcat(out, " / "); strncat(out, rows[l][i], 60); }
        printf("| %-32.32s |", out);
        if (has_ul[l]) printf("  underlined: cols %d-%d", underline[l][0] / 7, underline[l][1] / 7);
        printf("\n");
    }
    printf("+----------------------------------+\n");
}
static void display_clear(void) { clear(); }
static void status_clear(void) { status[0] = 0; }
static void draw_line(app_fb_t fb, int16_t x1, int16_t y1, int16_t x2, int16_t y2, bool black)
{
    (void)fb; (void)x2; (void)black;
    if (y1 == y2 && y1 / 8 < 8) { has_ul[y1 / 8] = 1; underline[y1 / 8][0] = x1; underline[y1 / 8][1] = x2; }
}
static void draw_rect(app_fb_t fb, int16_t a, int16_t b, int16_t c, int16_t d, bool e) { (void)fb; (void)a; (void)b; (void)c; (void)d; (void)e; }
static void print_line(const char *s, uint8_t start, uint8_t end, uint8_t line) { (void)end; add_row(line, start, s); }
static void print_tiny(const char *s, uint8_t x, uint8_t y, bool sb, bool fill)
{
    (void)fill;
    if (sb) { strcat(status, s); strcat(status, " "); return; }
    char t[64];
    snprintf(t, sizeof t, "~%s", s);        /* ~: the tiny font */
    add_row(y / 8, x * 7 / 4, t);
}
static void print_inverse(const char *s, uint8_t x, uint8_t line, bool sb, bool fill, uint8_t end)
{
    (void)x; (void)line; (void)fill; (void)end;
    if (sb) { strcat(status, "["); strcat(status, s); strcat(status, "] "); }
}
static void blit(void)
{
    while (shown < n_screens && now_ms() >= screen_at[shown]) { shown++; dump(); }
}
static void blit_line(unsigned l) { (void)l; }
static void delay_ms(uint32_t ms) { cyc += (uint64_t)ms * 48000u; blit(); if (now_ms() > script_end) longjmp(done, 1); }
static void nothing(void) {}
static void nothing_b(bool b) { (void)b; }
static void draw_frequency(uint32_t f, uint8_t x, uint8_t line) { char s[20]; snprintf(s, sizeof s, "%u.%05u", f / 100000, f % 100000); add_row(line, x, s); }
static uint32_t tx_freq(void) { return 14439000; }
static uint32_t ticks_ms(void) { return now_ms() / 10 * 10; }
static char *append_u32(char *o, uint32_t v) { return o + sprintf(o, "%u", v); }
static char *append_i32(char *o, int32_t v) { return o + sprintf(o, "%d", v); }
static uint64_t uidivmod(uint32_t n, uint32_t d) { return d ? (uint64_t)(n % d) << 32 | (n / d) : n; }
static int8_t nav_dir(uint8_t k) { return k == APP_KEY_UP ? -1 : k == APP_KEY_DOWN ? 1 : 0; }
static void boot_callsign(char *buf, uint8_t len) { snprintf(buf, len, "N0CALL"); }

/* ---- settings, hand-off ---- */
static uint8_t cfg[3][16];
static int running_app;                     /* 0 OnePress, 1 OP APRS, 2 OP CW */
static void cfg_load(uint8_t *b, uint8_t n) { memcpy(b, cfg[running_app], n > 16 ? 16 : n); }
static void cfg_save(const uint8_t *b, uint8_t n) { memcpy(cfg[running_app], b, n > 16 ? 16 : n); }
static char handoff_to[17];
static uint8_t handoff_msg[32], handoff_len, handoff_pending, handoff_have;
static bool handoff(const char *name, const void *msg, uint8_t len)
{
    if (strcmp(name, "OnePress") && strcmp(name, "OP APRS") && strcmp(name, "OP CW")) return false;
    snprintf(handoff_to, sizeof handoff_to, "%s", name);
    handoff_len = len > 32 ? 32 : len;
    if (len) memcpy(handoff_msg, msg, handoff_len);
    handoff_pending = 1;
    return true;
}
static int16_t handoff_read(void *buf, uint8_t max, char *from)
{
    (void)from;
    if (!handoff_have) return -1;
    handoff_have = 0;
    if (buf) memcpy(buf, handoff_msg, max < handoff_len ? max : handoff_len);
    return handoff_len;
}

/* ---- the radio: AFSK tones decoded back ---- */
static uint64_t tone_at[20000];
static int n_tones, tx_on;
static uint16_t tone_freq;
static void bk_write(uint8_t reg, uint16_t v)
{
    if (reg == 0x71 && tx_on && v != tone_freq && n_tones < 20000) tone_at[n_tones++] = cyc;
    if (reg == 0x71) tone_freq = v;
}
static uint16_t bk_read(uint8_t r) { (void)r; return 0; }
static uint8_t tx_state(void) { return 0; }
static void tx_set_params(void) { tx_on = 1; n_tones = 0; tone_freq = 0; printf("  [TX on 144.390]\n"); }
static void tx_tone(uint16_t hz) { (void)hz; }
static int keyed, morse_on;
static uint64_t key_since, key_off_at;
static char morse[4096];
static void tx_mute(bool on)                /* MCW: the tone keyed on and off */
{
    if (!tx_on) return;
    if (!on && !keyed) {
        size_t n = strlen(morse);       /* the gap before: a letter (3 units) or a word (7) */
        uint64_t gap = (cyc - key_off_at) / 48000u;
        if (n && n < sizeof morse - 3 && gap > 120) { strcat(morse, gap > 330 ? " / " : " "); }
        keyed = 1; key_since = cyc;
    }
    else if (on && keyed) {
        keyed = 0;
        size_t n = strlen(morse);
        if (n < sizeof morse - 2) { morse[n] = (cyc - key_since) / 48000u > 120 ? '-' : '.'; morse[n + 1] = 0; }
        key_off_at = cyc;
        morse_on = 1;
    }
}
static void decode_afsk(void)
{
    if (n_tones < 16) return;
    /* transitions -> bit cells (40000 cycles), NRZI: a transition is a 0 */
    static uint8_t bits[40000];
    int nb = 0;
    for (int i = 1; i + 1 < n_tones && nb < 39000; i++) {      /* [0]: the tone set up before the first bit */
        int cells = (int)((tone_at[i + 1] - tone_at[i] + 20000) / 40000);
        bits[nb++] = 0;
        for (int k = 1; k < cells; k++) bits[nb++] = 1;
    }
    /* HDLC: between flags, five 1s then a stuffed 0 */
    int i = 0;
    while (i + 8 <= nb && memcmp(bits + i, "\0\1\1\1\1\1\1\0", 8)) i++;     /* the first flag */
    while (i + 8 <= nb && !memcmp(bits + i, "\0\1\1\1\1\1\1\0", 8)) i += 8;  /* and the rest */
    uint8_t frame[400];
    int nf = 0, ones = 0, nbit = 0;
    uint8_t cur = 0;
    for (; i < nb; i++) {
        if (ones == 5) { if (bits[i] == 0) { ones = 0; continue; } break; }    /* 6 ones: a flag */
        ones = bits[i] ? ones + 1 : 0;
        cur |= (uint8_t)(bits[i] << nbit);
        if (++nbit == 8) { frame[nf++] = cur; cur = 0; nbit = 0; if (nf == 400) break; }
    }
    if (nf < 18) { printf("  [AFSK: no frame]\n"); return; }
    uint16_t crc = 0xFFFF;
    for (int k = 0; k < nf - 2; k++) {
        crc ^= frame[k];
        for (int b = 0; b < 8; b++) crc = crc & 1 ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    }
    crc ^= 0xFFFF;
    int ok = frame[nf - 2] == (crc & 0xFF) && frame[nf - 1] == crc >> 8;
    char tnc[600], *o = tnc;
    int a = 7, last = 0;
    #define CALL(p) do { for (int c = 0; c < 6; c++) if (frame[(p) + c] >> 1 != ' ') *o++ = (char)(frame[(p) + c] >> 1); \
                         if ((frame[(p) + 6] >> 1) & 15) o += sprintf(o, "-%d", (frame[(p) + 6] >> 1) & 15); } while (0)
    CALL(7); *o++ = '>'; CALL(0);
    last = frame[13] & 1;
    while (!last) { a += 7; *o++ = ','; CALL(a); last = frame[a + 6] & 1; }
    *o++ = ':';
    for (int k = a + 9; k < nf - 2; k++) *o++ = (char)frame[k];
    *o = 0;
    printf("  [AFSK decoded, %d bytes, FCS %s] %s\n", nf, ok ? "ok" : "BAD", tnc);
}
static void tx_end(void)
{
    if (tx_on && n_tones) decode_afsk();
    if (tx_on && morse_on) { printf("  [CW sent] %s\n", morse); morse[0] = 0; morse_on = 0; }
    tx_on = 0;
}
static void tx_carrier(bool on) { (void)on; }
static void get_battery(void) {}

static app_api_t api = {
    .abi_major = APP_ABI_MAJOR, .api_level = APP_API_LEVEL, .api_size = sizeof(app_api_t),
    .display_clear = display_clear, .status_clear = status_clear, .draw_line = draw_line, .draw_rect = draw_rect,
    .print_bold = print_line, .print_tiny = print_tiny, .blit_full = blit, .blit_line = blit_line,
    .blit_status = nothing, .get_key = get_key, .delay_ms = delay_ms, .print_normal = print_line,
    .print_inverse = print_inverse, .bk_read = bk_read, .bk_write = bk_write, .cfg_load = cfg_load,
    .cfg_save = cfg_save, .draw_battery = get_battery, .battery_sample = nothing, .backlight_on = nothing,
    .backlight_update = nothing, .tx_state = tx_state, .tx_set_params = tx_set_params, .tx_tone = tx_tone,
    .tx_mute = tx_mute, .tx_end = tx_end, .tx_carrier = tx_carrier, .tx_freq = tx_freq,
    .boot_callsign = boot_callsign, .print_string = NULL, .nav_dir = nav_dir, .ticks_ms = ticks_ms,
    .uidivmod = uidivmod, .append_i32 = append_i32, .draw_frequency = draw_frequency,
    .append_u32 = append_u32, .handoff = handoff, .handoff_read = handoff_read,
    .op_open = OP_Open, .op_decode = OP_Decode, .op_render = OP_Render, .op_guesses = OP_Guesses,
    .op_choice = OP_Choice, .op_word = OP_Word, .op_keys = gOpKeys, .op_text = gOpText,
};

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: opsim model.bin \"keys...\"\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); model_size = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    model = malloc(model_size);
    if (fread(model, 1, model_size, f) != model_size) return 1;
    fclose(f);
    static uint8_t fb[7][128];
    api.fb = fb;
    /* the script: presses on a timeline, starting at 200 ms */
    uint32_t t = 200;
    for (char *tok = strtok(argv[2], " "); tok; tok = strtok(NULL, " ")) {
        char *c = strchr(tok, ':');
        uint32_t ms = c ? (uint32_t)atoi(c + 1) : 80;
        if (tok[0] == 'w') { t += ms; continue; }
        if (tok[0] == 's') { screen_at[n_screens++] = t; continue; }
        uint8_t k = tok[0] >= '0' && tok[0] <= '9' ? (uint8_t)(tok[0] - '0') : tok[0] == '*' ? APP_KEY_STAR :
                    tok[0] == 'F' ? APP_KEY_F : tok[0] == 'M' ? APP_KEY_MENU : tok[0] == 'X' ? APP_KEY_EXIT :
                    tok[0] == 'U' ? APP_KEY_UP : tok[0] == 'D' ? APP_KEY_DOWN : tok[0] == 'P' ? APP_KEY_PTT : APP_KEY_INVALID;
        presses[n_presses].key = k;
        presses[n_presses].at = t;
        presses[n_presses++].until = t + ms;
        t += ms + 80;
    }
    script_end = t + 500;
    void (*apps[3])(const app_api_t *) = { onepress_main, opaprs_main, opcw_main };
    const char *names[3] = { "OnePress", "OP APRS", "OP CW" };
    if (setjmp(done)) { printf("== (script over)\n"); dump(); return 0; }
    for (int hop = 0; hop < 20; hop++) {
        printf("== %s\n", names[running_app]);
        unsigned long r0 = flash_reads;
        apps[running_app](&api);
        printf("== %s returned (%lu flash reads)\n", names[running_app], flash_reads - r0);
        if (!handoff_pending || now_ms() >= script_end) break;
        handoff_pending = 0;
        handoff_have = 1;
        while (get_key() != APP_KEY_INVALID) cyc += 480000;  /* the loader waits for every key to be let go */
        for (int i = 0; i < 3; i++) if (!strcmp(handoff_to, names[i])) running_app = i;
    }
    while (shown < n_screens) { shown++; dump(); }
    return 0;
}
