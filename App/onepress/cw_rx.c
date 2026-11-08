/*
 * Background CW receiver, see cw_rx.h.
 *
 * Goertzel filters over blocks of 96 samples (10 ms, 100 Hz bins) at 600, 700,
 * 800, 900 and 1000 Hz. The key is down while the two strongest bins (a tone
 * between two bins splits) carry more than 4 times the other three (once, to
 * stay down) and are above the floor of a closed squelch. Two blocks in a
 * row change the key: to go down, two with the same strongest bin (noise
 * wanders from bin to bin). Marks shorter
 * than two dits are dits; the dit follows the marks. A silence of two dits
 * ends a character, five a word. A word that is a lone E or T (one element:
 * what noise makes) is dropped. Up to ~30 WPM.
 */
#include <string.h>
#include "cw_rx.h"

#define N           96u     /* samples a block: 10 ms */
#define BINS        5u
#define P_MIN       2000    /* a tone's power, well above a muted squelch's */
#define DIT0        (6u * 16u)   /* 20 WPM to start, in blocks x16 */

static const int16_t COEF[BINS] = { 30274, 29389, 28378, 27246, 25997 };   /* 2 cos(2 pi k / 96), Q14, k 6-10 */
/* The Morse tree: from 1, a dit doubles, a dah doubles and adds 1. */
static const char TREE[64] = "**ETIANMSURWDKGOHVF*L*PJBXCYZQ**54*3***2*******16=/*****7***8*90";

char cw_rx_text[CW_RX_TEXT + 1];
static struct {
    int32_t dc, s1[BINS], s2[BINS];
    uint16_t dit;           /* blocks x16 */
    uint8_t n, run, flip, code, len, wpm, bin;
    char held;              /* a word's first character when it is E or T */
    bool key, spaced, tone;
} c;

void cw_rx_init(void)
{
    memset(&c, 0, sizeof c);
    memset(cw_rx_text, 0, sizeof cw_rx_text);
    c.dc = 2048 << 4;
    c.dit = DIT0;
    c.code = 1;
    c.spaced = true;
}

uint8_t cw_rx_wpm(void) { return c.wpm; }

static void emit(char ch)
{
    if (c.len == CW_RX_TEXT) memmove(cw_rx_text, cw_rx_text + 1, CW_RX_TEXT - 1u);
    else c.len++;
    cw_rx_text[c.len - 1u] = ch;
    cw_rx_text[c.len] = 0;
}

/* One 10 ms block decided: the key down or up. */
static void block(bool down)
{
    if (down != c.key) {
        if (++c.flip < (down ? 1u : 2u)) return;    /* not yet: two blocks (down: the caller's two) */
        if (c.key) {                                /* a mark ended, run blocks long */
            const uint16_t l = (uint16_t)(c.run * 16u);
            if (l < 2u * c.dit) { c.code = (uint8_t)(c.code << 1); c.dit = (uint16_t)((3u * c.dit + l) / 4u); }
            else { c.code = (uint8_t)(c.code << 1 | 1u); c.dit = (uint16_t)((3u * c.dit + l / 3u) / 4u); }
            if (c.dit < 2u * 16u) c.dit = 2u * 16u;
            if (c.dit > 30u * 16u) c.dit = 30u * 16u;
            if (c.code >= 128u) c.code = 0;         /* too long: not a character */
        }
        c.key = down;
        c.run = 2;                                  /* the two blocks that made it */
        c.flip = 0;
        return;
    }
    c.flip = 0;
    if (c.run < 255u) c.run++;
    if (c.key) return;
    const uint16_t gap = (uint16_t)(c.run * 16u);
    if (c.code != 1u && gap >= 2u * c.dit) {        /* a character */
        const char ch = c.code < 64u ? TREE[c.code] : c.code == 76u ? '?' : c.code == 85u ? '.' : c.code == 115u ? ',' : '*';
        if (c.spaced && c.code < 4u) c.held = ch;   /* E or T first: kept until the word goes on */
        else {
            if (c.held) emit(c.held);
            c.held = 0;
            emit(ch);
            c.wpm = (uint8_t)(1920u / c.dit);       /* PARIS: a dit is 1200 / WPM ms */
        }
        c.code = 1;
        c.spaced = false;
    }
    if (!c.spaced && gap >= 5u * c.dit) {
        if (c.held) c.held = 0;                     /* a lone E or T: noise, most likely */
        else emit(' ');
        c.spaced = true;
    }
}

void cw_rx_sample(uint16_t adc, bool quiet)
{
    c.dc += (((int32_t)adc << 4) - c.dc) >> 6;
    const int32_t x = ((int32_t)adc - (c.dc >> 4)) >> 2;
    for (uint8_t k = 0; k < BINS; k++) {
        const int32_t s = x + ((COEF[k] * c.s1[k]) >> 14) - c.s2[k];
        c.s2[k] = c.s1[k];
        c.s1[k] = s;
    }
    if (++c.n < N) return;
    c.n = 0;
    int32_t m = 0, m2 = 0, sum = 0;
    uint8_t bin = 0;
    for (uint8_t k = 0; k < BINS; k++) {
        const int32_t a = c.s1[k] >> 4, b = c.s2[k] >> 4;
        const int32_t p = a * a + b * b - ((COEF[k] * a) >> 14) * b;
        sum += p;
        if (p > m) { m2 = m; m = p; bin = k; } else if (p > m2) m2 = p;
        c.s1[k] = c.s2[k] = 0;
    }
    const int32_t top = m + m2, others = sum - top;
    const bool tone = top > 6 * others && top > P_MIN, prev = c.tone && bin == c.bin;
    c.tone = tone;
    c.bin = bin;
    block(!quiet && (c.key ? top > others && top > P_MIN / 2 : tone && prev));
}
