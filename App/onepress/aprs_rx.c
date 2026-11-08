/*
 * Background APRS message receiver, see aprs_rx.h.
 *
 * The demodulator is the APRS RX app's (App/apps/aprsrx, modelled in its
 * test/model_rx.py), Copyright 2026 Armel F4HWN, Apache License 2.0: DC
 * removal, a band-pass at 1625 Hz, 1200 / 2200 Hz sliding correlators over one
 * bit, per-tone AGC, then slicers with their own DPLL, NRZI, HDLC and FCS.
 * Here the frames are kept only when they are APRS messages to this station,
 * so a frame buffer holds a message's longest frame, not any frame.
 *
 * aprs_rx_sample() runs in an interrupt at 9.6 kHz: it must stay well inside
 * a sample period (5000 cycles; ~440 on average, 1400 at most on a Cortex-M0+,
 * tools/aprs_rx_test). A message is parsed there into pend; aprs_rx_poll()
 * moves it to the inbox from the main loop.
 */
#include <string.h>
#include "aprs_rx.h"

#define FRAME_MAX   160u   /* 8 path addresses + ctl/PID + ":ADDRESSEE:" + 67 + "{12345" + FCS */
#define FRAME_MIN   18u
#define FCS_GOOD    0xF0B8u
#define BIAS_CODE   2048
#define BP_B0       5356   /* band-pass 1625 Hz, Q 0.9, Q14 (b2 = -b0, b1 = 0) */
#define BP_A1     (-10714)
#define BP_A2       5673
#define AGC_ATT     5
#define AGC_DEC     10
#define PK_MIN      16
#define PLL_STEP    8192
#define PLL_CTR     36864
#define PLL_SHIFT   2

/* 127 cos(2 pi f n / 9600): 1200 Hz in 8 samples, 2200 Hz in 48 */
static const int8_t COS1200[8] = { 127, 90, 0, -90, -127, -90, 0, 90 };
static const int8_t COS2200[48] = {
    127, 17, -123, -49, 110, 77, -90, -101, 64, 117, -33, -126, 0, 126, 33, -117,
    -63, 101, 90, -77, -110, 49, 123, -17, -127, -17, 123, 49, -110, -77, 90, 101,
    -64, -117, 33, 126, 0, -126, -33, 117, 63, -101, -90, 77, 110, -49, -123, 17 };

typedef struct {
    int32_t  dc, x1, x2, y1, y2, im, qm, is, qs, pm, ps;
    uint8_t  r, ks;
    int16_t  ring[8][4];
} dem_t;

typedef struct {
    int32_t  dprev, phase;
    uint16_t crc;
    uint8_t  n, wa, wb, last, sr, ones, byte, nb, inframe;
    uint8_t  buf[FRAME_MAX];
} sl_t;

static dem_t d;
static sl_t sl[APRS_RX_SLICERS];
static aprs_msg_t inbox[APRS_RX_INBOX];
static uint8_t nInbox;
static char me[10], meBase[10];          /* addressees matched: "CALL-SSID", "CALL", padded to 9 */
static aprs_msg_t pend;                  /* a message to this station, for aprs_rx_poll */
static volatile uint8_t pendFull, pendBase;   /* pendBase: sent to CALL, not CALL-SSID */
static struct { char to[10], id[6], as[10]; bool due; } ack;   /* the ack to send */
static uint8_t msgId, msgHash, delivered;     /* the last number given, its text's hash */
static volatile uint8_t heard;                /* a heard ack (1) or rej (2) for msgId, from the interrupt */
uint16_t aprs_rx_frames;

static uint8_t putCall(char *o, const char *call, uint8_t ssid)
{
    uint8_t i = 0;
    while (call[i] && i < 6u) { o[i] = call[i]; i++; }
    if (ssid) {
        o[i++] = '-';
        if (ssid >= 10u) { o[i++] = '1'; ssid -= 10u; }
        o[i++] = (char)('0' + ssid);
    }
    return i;
}

void aprs_rx_init(const char *call, uint8_t ssid)
{
    static const uint8_t W[3][2] = { { 1, 1 }, { 2, 3 }, { 3, 2 } };   /* 1:1 first */
    memset(&d, 0, sizeof d);
    memset(sl, 0, sizeof sl);
    pendFull = 0;
    d.dc = BIAS_CODE << 4;
    d.pm = d.ps = 64;
    for (uint8_t k = 0; k < APRS_RX_SLICERS; k++) { sl[k].wa = W[k][0]; sl[k].wb = W[k][1]; }
    memset(me, ' ', 9); memset(meBase, ' ', 9);
    me[9] = meBase[9] = 0;
    putCall(me, call, ssid);
    putCall(meBase, call, 0);
}

void aprs_rx_put(const aprs_msg_t *m)
{
    memmove(&inbox[1], &inbox[0], sizeof inbox[0] * (APRS_RX_INBOX - 1u));
    inbox[0] = *m;
    if (nInbox < APRS_RX_INBOX) nInbox++;
}

/* Moves pend to the inbox, unless the newest one from the same station has the
 * same text (sent again: our ack, if any, was not heard). */
aprs_msg_t *aprs_rx_poll(void)
{
    aprs_msg_t *m = 0;
    if (!pendFull) return 0;
    if (pend.id[0]) {                    /* numbered: ack it, a repeat too */
        memcpy(ack.to, pend.from, sizeof ack.to);
        memcpy(ack.id, pend.id, sizeof ack.id);
        memcpy(ack.as, pendBase ? meBase : me, sizeof ack.as);
        for (uint8_t i = 9; i && ack.as[i - 1] == ' '; i--) ack.as[i - 1] = 0;
        ack.due = true;
    }
    if (!nInbox || strcmp(inbox[0].from, pend.from) || strcmp(inbox[0].text, pend.text)) {
        pend.unread = 1;
        aprs_rx_put(&pend);
        m = &inbox[0];
    }
    pendFull = 0;
    return m;
}

bool aprs_rx_ack_due(char *to, char *id, char *as)
{
    if (!ack.due) return false;
    memcpy(to, ack.to, sizeof ack.to);
    memcpy(id, ack.id, sizeof ack.id);
    memcpy(as, ack.as, sizeof ack.as);
    ack.due = false;
    return true;
}

void aprs_rx_seed(uint8_t n) { msgId = (uint8_t)(n % 99u); }

uint8_t aprs_rx_msgid(const char *to, const char *text)
{
    uint8_t h = 0x5A;
    for (const char *c = to; *c; c++) h = (uint8_t)((h << 1 | h >> 7) ^ (uint8_t)*c);
    h ^= 0xA5;
    for (const char *c = text; *c; c++) h = (uint8_t)((h << 1 | h >> 7) ^ (uint8_t)*c);
    if (!msgId || h != msgHash) {
        msgId = (uint8_t)(msgId % 99u + 1u);
        msgHash = h;
        delivered = heard = 0;
    }
    return msgId;
}

uint8_t aprs_rx_delivered(void)
{
    if (heard) delivered = heard;
    return delivered;
}

uint8_t aprs_rx_count(void) { return nInbox; }
aprs_msg_t *aprs_rx_msg(uint8_t n) { return n < nInbox ? &inbox[n] : 0; }
uint8_t aprs_rx_unread(void)
{
    uint8_t u = 0;
    for (uint8_t k = 0; k < nInbox; k++) u += inbox[k].unread;
    return u;
}

bool aprs_rx_busy(void)
{
    for (uint8_t k = 0; k < APRS_RX_SLICERS; k++) if (sl[k].inframe && sl[k].n >= 2u) return true;
    return false;
}

/* A good UI frame f[0..n): parsed into pend when it is an APRS message to this
 * station (addressed to CALL-SSID or CALL) and pend is free. */
static void frame(const uint8_t *f, uint8_t n)
{
    uint8_t e = 13;                                      /* the last address's SSID byte */
    while (!(f[e] & 1u) && e + 7u < n) e += 7;
    if (pendFull || !(f[e] & 1u) || e + 3u >= n || f[e + 1] != 0x03u || f[e + 2] != 0xF0u) return;
    const uint8_t *info = f + e + 3, *end = f + n - 2;
    if (end - info < 11 || info[0] != ':' || info[10] != ':') return;
    if (memcmp(info + 1, me, 9) && memcmp(info + 1, meBase, 9)) return;
    const uint8_t *t = info + 11;
    if (end - t >= 4 && (t[0] == 'a' || t[0] == 'r') && t[1] == (t[0] == 'a' ? 'c' : 'e') &&
        t[2] == (t[0] == 'a' ? 'k' : 'j')) {       /* "ack12", "rej12" (or "ack12}..."): an answer */
        uint8_t v = 0, i = 3;
        while (t + i < end && t[i] >= '0' && t[i] <= '9' && i < 6u) v = (uint8_t)(v * 10u + (t[i++] - '0'));
        if (i > 3u && (t + i == end || t[i] == '}')) {
            if (msgId && v == msgId) heard = t[0] == 'a' ? 1u : 2u;
            return;
        }
    }
    memset(&pend, 0, sizeof pend);
    pendBase = memcmp(info + 1, me, 9) != 0;
    uint8_t o = 0;
    for (uint8_t i = 7; i < 13u; i++) if (f[i] >> 1 != ' ') pend.from[o++] = (char)(f[i] >> 1);
    uint8_t ssid = (uint8_t)((f[13] >> 1) & 15u);
    if (ssid) {
        pend.from[o++] = '-';
        if (ssid >= 10u) { pend.from[o++] = '1'; ssid -= 10u; }
        pend.from[o++] = (char)('0' + ssid);
    }
    o = 0;
    while (t < end && *t != '{' && o < APRS_RX_TEXT) { uint8_t c = *t++; pend.text[o++] = (char)(c < ' ' || c > '~' ? '?' : c); }
    if (t < end && *t == '{') for (o = 0, t++; t < end && o < 5u && *t != '}'; ) pend.id[o++] = (char)*t++;
    pendFull = 1;
}

static void hdlcBit(sl_t *m, uint8_t b)
{
    m->sr = (uint8_t)((m->sr >> 1) | (b << 7));
    if (m->sr == 0x7Eu) {
        if (m->inframe && m->n >= FRAME_MIN && m->crc == FCS_GOOD) {
            aprs_rx_frames++;
            frame(m->buf, m->n);
            /* the other slicers have the same frame: not twice */
            for (uint8_t k = 0; k < APRS_RX_SLICERS; k++) sl[k].inframe = 0;
        }
        m->inframe = 1; m->n = 0; m->crc = 0xFFFFu;
        m->ones = m->byte = m->nb = 0;
        return;
    }
    if (b) {
        if (++m->ones > 6u) { m->ones = 7; m->inframe = 0; return; }
    } else {
        if (m->ones == 5u) { m->ones = 0; return; }
        m->ones = 0;
    }
    if (!m->inframe) return;
    m->byte |= (uint8_t)(b << m->nb);
    if (++m->nb == 8u) {
        if (m->n < FRAME_MAX) {
            uint16_t c = m->crc ^ m->byte;
            for (uint8_t i = 0; i < 8u; i++) c = (c & 1u) ? (uint16_t)((c >> 1) ^ 0x8408u) : (uint16_t)(c >> 1);
            m->crc = c;
            m->buf[m->n++] = m->byte;
        } else m->inframe = 0;
        m->byte = m->nb = 0;
    }
}

static int32_t tone(int32_t i, int32_t q, int32_t *pk)
{
    if (i < 0) i = -i;
    if (q < 0) q = -q;
    if (i < q) { int32_t t = i; i = q; q = t; }
    int32_t mt = i + (q >> 2) + (q >> 3), p = *pk;
    p += mt > p ? (mt - p) >> AGC_ATT : -(p >> AGC_DEC);
    if (p < PK_MIN) p = PK_MIN;
    *pk = p;
    return mt;
}

void aprs_rx_sample(uint16_t adc)
{
    d.dc += (((int32_t)adc << 4) - d.dc) >> 6;
    int32_t x = (int32_t)adc - (d.dc >> 4);
    int32_t y = (BP_B0 * (x - d.x2) - BP_A1 * d.y1 - BP_A2 * d.y2) >> 14;
    d.x2 = d.x1; d.x1 = x; d.y2 = d.y1; d.y1 = y;
    int16_t *o = d.ring[d.r];
    uint8_t k = (uint8_t)(d.ks + 12u); if (k >= 48u) k -= 48u;
    int32_t p;
    p = (y * COS1200[d.r]) >> 8;                d.im += p - o[0]; o[0] = (int16_t)p;
    p = (y * COS1200[(d.r + 6u) & 7u]) >> 8;    d.qm += p - o[1]; o[1] = (int16_t)p;
    p = (y * COS2200[d.ks]) >> 8;               d.is += p - o[2]; o[2] = (int16_t)p;
    p = (y * COS2200[k]) >> 8;                  d.qs += p - o[3]; o[3] = (int16_t)p;
    d.r = (uint8_t)((d.r + 1u) & 7u);
    if (++d.ks >= 48u) d.ks = 0;
    int32_t mm = tone(d.im, d.qm, &d.pm), ms = tone(d.is, d.qs, &d.ps);
    int32_t a = mm * d.ps, b = ms * d.pm;
    for (uint8_t s = 0; s < APRS_RX_SLICERS; s++) {
        sl_t *m = &sl[s];
        int32_t dd = a * m->wa - b * m->wb;
        m->phase += PLL_STEP;
        if ((dd > 0) != (m->dprev > 0)) m->phase += (PLL_CTR - m->phase) >> PLL_SHIFT;
        m->dprev = dd;
        if (m->phase >= 65536) {
            m->phase -= 65536;
            uint8_t lv = dd > 0;
            hdlcBit(m, lv == m->last);
            m->last = lv;
        }
    }
}
