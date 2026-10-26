/* OnePress decoder. Mirrors web/decoder.js; see onepress.h. */
#include "onepress.h"

#include <stdlib.h>
#include <string.h>

enum { TOK_WORD, TOK_PUNCT, TOK_SPACE, TOK_NUM };
enum { N_SECTIONS = 15, N_ALL_SECTIONS = 31 };     /* sections 15..30 are optional */

static const char KEY_OF[26] = "22233344455566677778889999";
static const char FIRST_LETTER[8] = "adgjmptw";     /* first letter of keys 2-9 */
#define DEFAULT_LAYOUT "qwertyuiop|asdfghjkl|zxcvbnm"   /* start->layout when NULL */
#define FLAG_I 1                /* params[OP_P_FLAGS]: capitalize "i" (English) */
#define FLAG_INVERTED 2         /* open a question / exclamation with an inverted mark (Spanish) */
#define FLAG_GREEK_Q 4          /* show the question mark as ; (Greek) */
#define FLAG_FR_SPACE 8         /* a narrow no-break space (U+202F) before ? ! : (French) */
#define FLAG_ARABIC 16          /* show ? and , as Arabic U+061F and U+060C */
#define AFTER_NUMBER 0xFFFE     /* beam state after a number (the model has no number token) */
#define AFTER_DECIMAL 0xFFFC    /* after a number that could have been a time but was read as 5.30 */
#define TIME_BONUS 8            /* nats for "am"/"pm" right after a number or a time */
/* A number that reads as H:MM has a decimal and a time candidate; see decoder.js TIME_PREV. */
#define TIME_PREV 2
#define TIME_ALONE 2
#define TIME_LITERAL 14
static const char *const TIME_BEFORE[OP_TIME_BEFORE] = { "at", "by", "until", "til", "till", "around", "before", "after", "from", "since" };
static const char *const TIME_AFTER[] = { "am", "pm" };

#define USER_ANY 0xFFFD         /* op_user_pair prev for a token's own count */
/* Learning bonus by count (capped at 15), as decoder.js USER_UNI / USER_PAIR. */
static const int16_t USER_UNI[16] = { 0, 710, 1125, 1420, 1648, 1835, 1993, 2129, 2250, 2358, 2455, 2545, 2627, 2702, 2773, 2839 };
static const int16_t USER_PAIR[16] = { 0, 532, 844, 1065, 1236, 1376, 1494, 1597, 1687, 1768, 1842, 1908, 1970, 2027, 2080, 2129 };
#define USER_WORD_BASE (-2560)  /* log P of a learned word before its bonuses (-10 nats) */

static int find_word(const op_model *m, const char *w);

/* ---- model ---- */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* Element i of a model array (OP_REF): read in place, or from storage with OP_EXT_MODEL. */
#ifdef OP_EXT_MODEL
static uint32_t ext_rd(uint32_t a, uint32_t n) {
    uint8_t b[4] = { 0, 0, 0, 0 };
    op_ext_read(a, b, n);
    return rd32(b);
}
#define AU16(p, i) ((uint16_t)ext_rd((p) + 2u * (uint32_t)(i), 2))
#define AI16(p, i) ((int16_t)ext_rd((p) + 2u * (uint32_t)(i), 2))
#define AU32(p, i) ext_rd((p) + 4u * (uint32_t)(i), 4)
#define AI8(p, i) ((int8_t)ext_rd((p) + (uint32_t)(i), 1))
#define ACH(p, i) ((char)ext_rd((p) + (uint32_t)(i), 1))
#else
#define AU16(p, i) ((p)[i])
#define AI16(p, i) ((p)[i])
#define AU32(p, i) ((p)[i])
#define AI8(p, i) ((p)[i])
#define ACH(p, i) ((p)[i])
#endif

/* Text of word `id` (not NUL-terminated) and its length: in place, or copied into buf
 * (OP_WORD_BUF bytes) with OP_EXT_MODEL. */
static int word_at(const op_model *m, uint16_t id, const char **text, char *buf) {
    uint32_t a = AU32(m->word_off, id), n = AU32(m->word_off, id + 1) - a;
#ifdef OP_EXT_MODEL
    if (n > OP_WORD_BUF) n = OP_WORD_BUF;
    op_ext_read(m->word_text + a, buf, n);
    *text = buf;
#else
    (void)buf;
    *text = m->word_text + a;
#endif
    return (int)n;
}

int op_model_open(op_model *m, const void *data, size_t size) {
    OP_REF(uint8_t) sec[N_ALL_SECTIONS];
    uint32_t len[N_ALL_SECTIONS];
#ifdef OP_EXT_MODEL
    /* The header and section table, read from storage; every section is required. */
    uint8_t b[16 + 8 * N_ALL_SECTIONS];
    uint32_t base = (uint32_t)(uintptr_t)data;
    if (!m || !base || (base & 3) || size < sizeof b) return OP_ERR_MODEL;
    op_ext_read(base, b, sizeof b);
    if (memcmp(b, "KPD2", 4) != 0 || rd16(b + 14) < N_ALL_SECTIONS) return OP_ERR_MODEL;
    #define SEC(T, i) sec[i]
#else
    const uint8_t *b = (const uint8_t *)data;
    static const uint32_t one_zero = 0;      /* an empty triStart / altOff */
    if (!m || !b || ((uintptr_t)b & 3) || size < 16 || memcmp(b, "KPD2", 4) != 0) return OP_ERR_MODEL;
    #define SEC(T, i) ((const T *)sec[i])
#endif
    if (rd16(b + 4) != 1 || rd16(b + 14) < N_SECTIONS || size < 16 + 8u * N_SECTIONS) return OP_ERR_MODEL;
    m->scale = rd16(b + 6);
    m->n_words = rd16(b + 8);
    m->n_punct = rd16(b + 10);
    m->bos = rd16(b + 12);
    /* Optional sections: older files stop after the 15th; files with three-word context have
     * at least 25 (capNextKey, the 26th, came last). Newer sections beyond these are ignored. */
    int n_sec = rd16(b + 14) < N_ALL_SECTIONS ? rd16(b + 14) : N_ALL_SECTIONS;
    if (n_sec < 25) n_sec = N_SECTIONS;
    if (size < 16 + 8u * n_sec) return OP_ERR_MODEL;
#ifndef OP_EXT_MODEL
    for (int i = n_sec; i < N_ALL_SECTIONS; i++) { sec[i] = (const uint8_t *)&one_zero; len[i] = 0; }
#endif
    for (int i = 0; i < n_sec; i++) {
        uint32_t off = rd32(b + 16 + 8 * i);
        len[i] = rd32(b + 20 + 8 * i);
        if ((off & 3) || off > size || len[i] > size - off) return OP_ERR_MODEL;
#ifdef OP_EXT_MODEL
        sec[i] = base + off;
#else
        sec[i] = b + off;
#endif
    }
    m->word_text = SEC(char, 0);
    m->word_off = SEC(uint32_t, 1);
    m->key_idx = SEC(uint16_t, 2);
    m->strip_idx = SEC(uint16_t, 3);
    m->n_strip = len[3] / 2;
    m->disp_id = SEC(uint16_t, 4);
    m->n_disp = len[4] / 2;
    m->disp_off = SEC(uint32_t, 5);
    m->disp_text = SEC(char, 6);
    m->uni = SEC(int16_t, 7);
    m->row_start = SEC(uint32_t, 8);
    m->bow = SEC(int16_t, 9);
    m->next = SEC(uint16_t, 10);
    m->lp = SEC(int16_t, 11);
    m->q_start = SEC(uint16_t, 12);
    m->n_q = len[12] / 2;
    m->punct = SEC(char, 13);
    if (len[13] != m->n_punct || len[14] < 2 * OP_P_SLIP_SUB || m->bos != m->n_words + m->n_punct ||
        len[1] != 4u * (m->n_words + 1u) || len[2] != 2u * m->n_words || len[8] != 4u * (m->bos + 2u))
        return OP_ERR_MODEL;
    m->param[OP_P_SLIP_SUB] = (int16_t)(-12 * m->scale);     /* defaults for files without them */
    m->param[OP_P_SLIP_DUP] = (int16_t)(-8 * m->scale);
    m->param[OP_P_FLAGS] = FLAG_I;
    for (int i = 0; i < OP_N_PARAMS && 2u * i + 2 <= len[14]; i++) m->param[i] = AI16(SEC(int16_t, 14), i);
    m->q_skip = SEC(uint16_t, 15);
    m->n_skip = len[15] / 2;
    m->tri_key = SEC(uint32_t, 16);
    m->n_tri = len[16] / 4;
    m->tri_start = SEC(uint32_t, 17);
    m->tri_bow = SEC(int16_t, 18);
    m->tri_next = SEC(uint16_t, 19);
    m->tri_lp = SEC(int16_t, 20);
    m->cap_key = SEC(uint32_t, 21);
    m->n_cap = len[21] / 4;
    m->alt_id = SEC(uint16_t, 22);
    m->n_alt = len[22] / 2;
    m->alt_off = SEC(uint32_t, 23);
    m->alt_text = SEC(char, 24);
    m->cap_next = SEC(uint32_t, 25);
    m->n_cap_next = len[25] / 4;
    m->cap_keep = SEC(uint32_t, 26);
    m->n_cap_keep = len[26] / 4;
    m->cmp_head = len[27] == m->n_words ? SEC(int8_t, 27) : 0;
    m->cmp_tail = len[28] == m->n_words ? SEC(int8_t, 28) : 0;
    m->cmp_known = SEC(uint32_t, 29);
    m->elided = 0;
    for (uint16_t i = 0; i < m->n_words && !m->elided; i++)
        m->elided = AU32(m->word_off, i + 1) > AU32(m->word_off, i) &&
                    (ACH(m->word_text, AU32(m->word_off, i + 1) - 1) == '\'' || ACH(m->word_text, AU32(m->word_off, i)) == '\'' ||
                     ACH(m->word_text, AU32(m->word_off, i)) == '-');
    m->n_known = len[29] / 4;
    m->cmp_bloom = len[30] >= 12 && len[30] >= 8 + AU32(SEC(uint32_t, 30), 0) / 8 ? SEC(uint32_t, 30) : 0;
    #undef SEC
    if (m->n_tri >= 0xFFFF || (n_sec > N_SECTIONS && (len[17] != 4u * (m->n_tri + 1) || len[18] != 2u * m->n_tri ||
        len[20] != len[19] || len[23] != 4u * (m->n_alt + 1))))
        return OP_ERR_MODEL;
    for (int i = 0; i < 2; i++) m->time_id[i] = (uint16_t)find_word(m, TIME_AFTER[i]);
    for (int i = 0; i < OP_TIME_BEFORE; i++) m->time_before[i] = (uint16_t)find_word(m, TIME_BEFORE[i]);
    return OP_OK;
}

int op_word(const op_model *m, uint16_t id, const char **text) {
    static char buf[OP_WORD_BUF];
    return word_at(m, id, text, buf);
}

/* log P(cur | prev) in model units; prev OP_NONE (unknown word) or AFTER_NUMBER falls back to
 * the unigram, and right after a number "am" and "pm" get a bonus. */
static int32_t lp(const op_model *m, uint16_t prev, uint16_t cur) {
    if (prev == AFTER_NUMBER)
        return AI16(m->uni, cur) + (cur == m->time_id[0] || cur == m->time_id[1] ? TIME_BONUS * m->scale : 0);
    if (prev > m->bos) return AI16(m->uni, cur);      /* unknown, or a learned word */
    uint32_t lo = AU32(m->row_start, prev), hi = AU32(m->row_start, prev + 1);
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        uint16_t x = AU16(m->next, mid);
        if (x == cur) return AI16(m->lp, mid);
        if (x < cur) lo = mid + 1; else hi = mid;
    }
    return AI16(m->bow, prev) + AI16(m->uni, cur);
}

static int in_u16(OP_REF(uint16_t) a, uint32_t n, uint16_t id) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        uint16_t x = AU16(a, mid);
        if (x == id) return 1;
        if (x < id) lo = mid + 1; else hi = mid;
    }
    return 0;
}
static int is_q_start(const op_model *m, uint16_t id) { return in_u16(m->q_start, m->n_q, id); }

/* Sentence/clause opener state: 0 none yet, 1 question word, 2 anything else. */
static uint8_t q_state(const op_model *m, uint16_t id) {
    return id != OP_NONE && is_q_start(m, id) ? 1 : 2;
}

/* Index of key k in a sorted u32 array, or -1. */
static int32_t find_u32(OP_REF(uint32_t) a, uint32_t n, uint32_t k) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (AU32(a, mid) < k) lo = mid + 1; else hi = mid;
    }
    return lo < n && AU32(a, lo) == k ? (int32_t)lo : -1;
}

/* Three-word context (p2, p1): its index in tri_key, or OP_NONE (also for numbers, unknown and
 * learned words, which no context includes). See decoder.js triFind. */
static uint16_t tri_find(const op_model *m, uint16_t p2, uint16_t p1) {
    if (p2 > m->bos || p1 > m->bos) return OP_NONE;
    int32_t i = find_u32(m->tri_key, m->n_tri, (uint32_t)p2 << 16 | p1);
    return i < 0 ? OP_NONE : (uint16_t)i;
}
/* log P(cur | context t, prev): the triple if stored, else the context's backoff + the pair. */
static int32_t lp3(const op_model *m, uint16_t t, uint16_t prev, uint16_t cur) {
    if (t == OP_NONE) return lp(m, prev, cur);
    uint32_t lo = AU32(m->tri_start, t), hi = AU32(m->tri_start, t + 1);
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        uint16_t x = AU16(m->tri_next, mid);
        if (x == cur) return AI16(m->tri_lp, mid);
        if (x < cur) lo = mid + 1; else hi = mid;
    }
    return AI16(m->tri_bow, t) + lp(m, prev, cur);
}

/* Letters beyond a-z (accented Latin, Greek, Cyrillic: U+00C0-U+045F): each has its keypad key
 * ('0' = not a letter), the plain letter an exact or QWERTY letter matches ('0' = none: Greek,
 * Cyrillic) and the offset to its capital. Generated by model/chartable.py, as in decoder.js. */
/* CHARTABLE generated by model/chartable.py: begin */
static const signed char CT_DELTA0[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,0,-32,-32,-32,-32,-32,-32,-32,121,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,0,0,-1,0,-1,0,-1,0,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,0,-1,0,-1,0,-1,0,0,0,0,-1,0,0,0,0,-1,0,0,0,-1,0,0,0,0,0,-1,0,0,0,0,0,0,-1,0,0,0,0,0,0,0,-1,0,0,0,-1,0,0,0,0,0,0,0,-1,0,0,-1,0,0,0,-1,0,-1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,0,-1,0,-1,0,0,0,-1,0,-1,0,-1,0,-1,0,-1,0,0,0,0,0,0,0,-1,0,0,0,-1,0,-1,0,0,0,0,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,0,0,-1,0,0,0,0,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,-1,0,-1,0,-1,0,-1,0,-1};
static const signed char CT_LDELTA0[] = {32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,0,32,32,32,32,32,32,32,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,0,0,1,0,1,0,1,0,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,-121,1,0,1,0,1,0,0,0,0,1,0,0,0,0,1,0,0,0,1,0,0,0,0,0,1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,1,0,0,0,0,0,0,0,1,0,0,1,0,0,0,1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,0,1,0,1,0,0,0,1,0,1,0,1,0,1,0,1,0,0,0,0,0,0,0,1,0,0,0,1,0,1,0,0,0,0,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,0,0,1,0,0,0,0,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,1,0,1,0,1,0,1,0};
static const signed char CT_PDELTA0[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
static const signed char CT_DELTA1[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,-38,-37,-37,-37,0,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-31,-32,-32,-32,-32,-32,-32,-32,-32,-32,-64,-63,-63,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
static const signed char CT_LDELTA1[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,38,0,37,37,37,0,64,0,63,63,0,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,0,32,32,32,32,32,32,32,32,32,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,-60,0,0,0,0,0,0,0,0,0,0,0};
static const signed char CT_PDELTA1[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,11,0,13,14,15,0,19,0,23,26,41,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,-17,-6,5,8,9,10,21,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,-17,-6,-13,-8,-5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
static const signed char CT_DELTA2[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-32,-80,-80,0,-80,-80,0,-80,-80,0,0,0,0,-80,-80,-80,0};
static const signed char CT_LDELTA2[] = {80,80,0,80,80,0,80,80,0,0,0,0,80,80,80,0,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
static const signed char CT_PDELTA2[] = {21,20,0,16,0,0,0,-1,0,0,0,0,14,11,21,0,0,0,0,0,0,0,0,0,0,-1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,-1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,-27,-28,0,-32,0,0,0,-1,0,0,0,0,-34,-37,-27,0};
static const signed char CT_DELTA3[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
static const signed char CT_LDELTA3[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
static const signed char CT_PDELTA3[] = {0,0,5,4,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0};
static const struct { uint16_t lo, hi; const char *keys, *base; const signed char *delta, *ldelta, *pdelta; } CHARTABLE[] = {
    { 0x00C0, 0x0250,
      "2222222233334444366666606888898722222222333344443666666068888989222222222222223333333333333344444444444444444444044455555555555555566666666666666666777777777777778888888888888888889999999999972022000220033000033000005550006066007700000888088009999000000000000000000000022446688888888880222200444455666600500044006622000022223333444466667777888877880044600099223366666666990000000005079002003355777799",
      "aaaaaaaceeeeiiiidnooooo0ouuuuytsaaaaaaaceeeeiiiidnooooo0ouuuuytyaaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiii0iiijjkkkllllllllllnnnnnnnnnoooooooorrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzzsb0bb000cc00dd0000ff00000kkl000n0oo00pp00000ttt0uu00yyzz0000000000000000000000aaiioouuuuuuuuuu0aaaa00ggggkkoooo00j000gg00nnaa0000aaaaeeeeiiiioooorrrruuuusstt00hhn000zzaaeeooooooooyy000000000l0sz00b00eejjqqrryy", CT_DELTA0, CT_LDELTA0, CT_PDELTA0 },
    { 0x0370, 0x0400,
      "000000000000000000000020344060894222333444555666770788899948234482223334445556667777888999486890000000000000000000000000000000000000400000000000",
      "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000", CT_DELTA1, CT_LDELTA1, CT_PDELTA1 },
    { 0x0400, 0x0460,
      "330230440000446022223333444455556666777788889999222233334444555566667777888899993302304400004460",
      "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000", CT_DELTA2, CT_LDELTA2, CT_PDELTA2 },
    { 0x0620, 0x064B,
      "0333333322226665555444499990000008888877777",
      "0000000000000000000000000000000000000000000", CT_DELTA3, CT_LDELTA3, CT_PDELTA3 },
};
/* CHARTABLE generated by model/chartable.py: end */

/* The code point of w at *i (UTF-8, up to U+07FF; 0xFFFD for longer ones); moves *i past it. */
static int next_cp(const char *w, int wn, int *i) {
    unsigned char c = (unsigned char)w[(*i)++];
    if (c < 0x80) return c;
    if (c >= 0xC2 && c < 0xE0 && *i < wn) return (c & 0x1F) << 6 | ((unsigned char)w[(*i)++] & 0x3F);
    while (*i < wn && ((unsigned char)w[*i] & 0xC0) == 0x80) (*i)++;
    return 0xFFFD;
}
/* Index into CHARTABLE[t] of letter cp, or -1. */
static int ct_find(int cp, int *t) {
    for (*t = 0; *t < (int)(sizeof CHARTABLE / sizeof CHARTABLE[0]); (*t)++)
        if (cp >= CHARTABLE[*t].lo && cp < CHARTABLE[*t].hi && CHARTABLE[*t].keys[cp - CHARTABLE[*t].lo] != '0')
            return cp - CHARTABLE[*t].lo;
    return -1;
}
/* Small letter of cp (a-z, the table's), else cp itself. */
static int lower_cp(int cp) {
    int t, k;
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    k = cp >= 0xC0 ? ct_find(cp, &t) : -1;
    return k >= 0 ? cp + CHARTABLE[t].ldelta[k] : cp;
}
/* The plain letter of cp: Latin ones fold to a-z (é -> e), Greek and Cyrillic to their own
 * (ά -> α); other characters are themselves (decoder.js plainOf). */
static int plain_cp(int cp) {
    int t, k = cp >= 0xC0 ? ct_find(cp, &t) : -1;
    if (k < 0) return cp;
    return CHARTABLE[t].base[k] != '0' ? CHARTABLE[t].base[k] : cp + CHARTABLE[t].pdelta[k];
}
/* Does a word's character agree with an exact (or tapped) letter (a code point, lowercase)? The
 * same letter, or a plain one matches its accented forms (n matches ñ, α matches ά). See
 * decoder.js litEq. */
static int lit_ok(uint16_t lit, int cp) {
    cp = lower_cp(cp);
    return lit == cp || (plain_cp(lit) == lit && plain_cp(cp) == lit);
}
/* Keypad key of the character of w at *i ('#' for an apostrophe, '?' if none). */
static int next_key(const char *w, int wn, int *i, int *apostrophe) {
    int cp = next_cp(w, wn, i), t, k;
    *apostrophe = cp == '\'';
    if (cp >= 'A' && cp <= 'Z') cp += 32;
    if (cp >= 'a' && cp <= 'z') return KEY_OF[cp - 'a'];
    if (cp == '\'' || cp == '-') return '#';       /* a hyphen inside a word too (est-ce) */
    k = ct_find(cp, &t);
    return k >= 0 ? CHARTABLE[t].keys[k] : '?';
}
/* Characters in w[0..wn), and the bytes taken by its first `chars` characters. */
static int u8_len(const char *w, int wn) {
    int n = 0;
    for (int i = 0; i < wn; i++) n += ((unsigned char)w[i] & 0xC0) != 0x80;
    return n;
}
static int u8_prefix(const char *w, int wn, int chars) {
    int i = 0;
    while (i < wn && chars-- > 0) {
        i++;
        while (i < wn && ((unsigned char)w[i] & 0xC0) == 0x80) i++;
    }
    return i;
}

/* Compare the keys of word `id` (apostrophes dropped if strip) with q[0..qn).
 * Returns <0, 0, >0 like strcmp; *starts is set when the word's keys begin with q. */
static int key_cmp(const op_model *m, uint16_t id, int strip, const char *q, int qn, int *starts) {
    const char *w;
    char wb[OP_WORD_BUF];
    int wn = word_at(m, id, &w, wb), i = 0;
    for (int j = 0;; j++) {
        int c = -1;
        while (i < wn) {
            int ap;
            c = next_key(w, wn, &i, &ap);
            if (ap && strip) { c = -1; continue; }
            break;
        }
        if (j == qn) { *starts = 1; return c < 0 ? 0 : 1; }
        *starts = 0;
        if (c < 0) return -1;
        if (c != q[j]) return c < q[j] ? -1 : 1;
    }
}

/* First position in an index whose key is >= q. */
static uint32_t lower_bound(const op_model *m, OP_REF(uint16_t) ix, uint32_t n, int strip, const char *q, int qn) {
    uint32_t lo = 0, hi = n;
    int starts;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (key_cmp(m, AU16(ix, mid), strip, q, qn, &starts) < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}

/* Id of dictionary word w (UTF-8, NUL-terminated, under 64 bytes), or OP_NONE. */
static int next_key(const char *w, int wn, int *i, int *apostrophe);
static int find_word(const op_model *m, const char *w) {
    char k[64];
    int wn = (int)strlen(w), kn = 0, starts, ap;
    for (int i = 0; i < wn && kn < 64;) k[kn++] = (char)next_key(w, wn, &i, &ap);
    for (uint32_t i = lower_bound(m, m->key_idx, m->n_words, 0, k, kn);
         i < m->n_words && key_cmp(m, AU16(m->key_idx, i), 0, k, kn, &starts) == 0; i++) {
        const char *t;
        char wb[OP_WORD_BUF];
        int tn = word_at(m, AU16(m->key_idx, i), &t, wb);
        if (tn == wn && memcmp(t, w, (size_t)wn) == 0) return AU16(m->key_idx, i);
    }
    return OP_NONE;
}

/* Does word w (apostrophes dropped if strip) agree with the exact letters in lit[0..n)? */
static int lit_match_text(const char *w, int wn, int strip, const uint16_t *lit, int n) {
    int t = 0, i = 0;
    while (i < wn && t < n) {
        int cp = next_cp(w, wn, &i);        /* learned words may be cased */
        if (strip && cp == '\'') continue;
        if (lit[t] && !lit_ok(lit[t], cp)) return 0;
        t++;
    }
    for (; t < n; t++) if (lit[t]) return 0;    /* word shorter than the exact letters */
    return 1;
}
static int lit_match(const op_model *m, uint16_t id, int strip, const uint16_t *lit, int n) {
    const char *w;
    char wb[OP_WORD_BUF];
    int wn = word_at(m, id, &w, wb);
    return lit_match_text(w, wn, strip, lit, n);
}

/* ---- learned words ---- */

/* Text of a dictionary or learned word (buf: OP_WORD_BUF bytes, see word_at). */
static int tok_text(const op_model *m, const op_user *u, uint16_t id, const char **text, char *buf) {
    if (id > m->bos) {
        *text = u->word[id - m->bos - 1];
        return (int)strlen(*text);
    }
    return word_at(m, id, text, buf);
}

/* Index of pair (prev, cur), or -(insertion point) - 1. */
static int user_find(const op_user *u, uint16_t prev, uint16_t cur) {
    uint32_t k = (uint32_t)prev << 16 | cur;
    int lo = 0, hi = u->n_pairs;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        uint32_t x = (uint32_t)u->pair[mid].prev << 16 | u->pair[mid].cur;
        if (x == k) return mid;
        if (x < k) lo = mid + 1; else hi = mid;
    }
    return -lo - 1;
}
static int user_count(const op_user *u, uint16_t prev, uint16_t cur) {
    int i = user_find(u, prev, cur);
    return i >= 0 ? u->pair[i].count : 0;
}

static int32_t token_score(const op_model *m, const op_user *u, uint16_t prev, uint16_t id, uint16_t tri) {
    int32_t s = id > m->bos ? USER_WORD_BASE : lp3(m, tri, prev, id);
    if (u) {
        int c = user_count(u, USER_ANY, id);
        s += USER_UNI[c < 15 ? c : 15];
        if (prev != OP_NONE && prev != AFTER_NUMBER && prev != AFTER_DECIMAL) {
            c = user_count(u, prev, id);
            s += USER_PAIR[c < 15 ? c : 15];
        }
    }
    return s;
}

/* Does word a agree with the exact letters b[0..bn), apostrophes dropped from both? */
static int same_letters(const char *a, int an, const uint16_t *b, int bn) {
    int i = 0, j = 0;
    for (;;) {
        while (i < an && a[i] == '\'') i++;
        while (j < bn && b[j] == '\'') j++;
        if (i == an || j == bn) return i == an && j == bn;
        if (!lit_ok(b[j++], next_cp(a, an, &i))) return 0;
    }
}

/* ---- tokenizer ---- */

/* Exactly spelled letters are a-z or any table letter (a Russian or Greek multi-tap), stored
 * lowercased; see decoder.js isLetterKey / keyOf / litOf. */
static char table_key(int cp) {
    int t, k = cp >= 0xC0 ? ct_find(cp, &t) : -1;
    return k >= 0 ? CHARTABLE[t].keys[k] : 0;
}
static int is_letter_key(int ch) {      /* a press that can be part of a word's letters */
    return (ch >= '2' && ch <= '9') || (ch >= 'a' && ch <= 'z') || ch == '\'' || table_key(ch);
}
static int is_digit_lit(int ch) { return ch >= 'A' && ch <= 'J'; }
static char key_of(int ch) {
    if (ch >= 'a' && ch <= 'z') return KEY_OF[ch - 'a'];
    if (ch == '\'') return '#';
    if (table_key(ch)) return table_key(ch);
    return (char)ch;
}
static uint16_t lit_of(int ch) {
    if ((ch >= 'a' && ch <= 'z') || ch == '\'') return (uint16_t)ch;
    if (is_digit_lit(ch)) return (uint16_t)('0' + (ch - 'A'));
    if (table_key(ch)) return (uint16_t)lower_cp(ch);
    return 0;
}

/* Digits end a word and run on as a number through more digits and through '#', '.' or ','
 * between digits; a '#' just before a number is '$'. Digits next to exactly spelled letters
 * stay in the word (call signs). A number's span of keys[] holds '$', '0'-'9', '.', ',' and
 * 's' for a separator format_number chooses. See decoder.js tokenize. */
static int tokenize(op_decoder *d, const char *keys, const char *hint) {
    int n = (int)strlen(keys), nk = 0, hn = 0;
    if (n > OP_MAX_KEYS) return OP_ERR_TOO_LONG;
    d->n_toks = 0;
    memset(d->hints, 0, sizeof d->hints);
    /* the letter tapped for each press: a-z, or a table letter, lowercased (decoder.js hintAt) */
    uint16_t hcp[OP_MAX_KEYS];
    for (int i = 0, bn = hint ? (int)strlen(hint) : 0; i < bn && hn < OP_MAX_KEYS;) {
        int cp = next_cp(hint, bn, &i);
        hcp[hn++] = (uint16_t)((cp >= 'a' && cp <= 'z') ? cp : (cp >= 'A' && cp <= 'Z') ? cp + 32
                                : table_key(cp) ? lower_cp(cp) : 0);
    }
    for (int i = 0, press = 0; i < n; i++, press++) {
        /* one key press: a byte, or a letter in UTF-8 (exact Greek, Cyrillic, accents) */
        int i0 = i, at = i, ch = next_cp(keys, n, &at), nxt = 0;
        i = at - 1;
        if (at < n) { int a2 = at; nxt = next_cp(keys, n, &a2); }
        op_tok *last = d->n_toks ? &d->toks[d->n_toks - 1] : NULL;
        op_tok *t = &d->toks[d->n_toks];
        if (ch == '0') {
            t->type = TOK_SPACE; t->off = (uint16_t)i0; d->n_toks++;
        } else if ((ch == '#' || ch == '.' || ch == ',' || ch == ':') && last && last->type == TOK_NUM &&
                   d->keys[nk - 1] >= '0' && d->keys[nk - 1] <= '9' && is_digit_lit(nxt)) {
            d->keys[nk] = ch == '#' ? 's' : ch; d->lits[nk++] = 0; last->len++;
        } else if (ch == '#') {
            if (last && last->type == TOK_WORD && is_letter_key(nxt)) {
                d->keys[nk] = '#'; d->lits[nk++] = 0; last->len++;
            } else {
                t->type = TOK_PUNCT; t->lit = 0; d->n_toks++;
            }
        } else if (ch == '.' || ch == ',' || ch == '?' || ch == '!' || ch == ':') {
            t->type = TOK_PUNCT; t->lit = (char)ch; d->n_toks++;
        } else if (is_digit_lit(ch)) {
            char dg = (char)lit_of(ch);
            const op_tok *before = d->n_toks >= 2 ? &d->toks[d->n_toks - 2] : NULL;
            if (last && last->type == TOK_NUM) {
                d->keys[nk] = dg; d->lits[nk++] = 0; last->len++;
            } else if (last && last->type == TOK_WORD && d->lits[nk - 1]) {
                d->keys[nk] = (char)ch; d->lits[nk++] = (uint16_t)dg; last->len++;
            } else {
                if (last && last->type == TOK_PUNCT && !last->lit && keys[i0 - 1] == '#' &&
                    !(before && (before->type == TOK_WORD || before->type == TOK_NUM))) {
                    t = last;               /* the '#' becomes the number's '$' */
                    t->len = 1;
                    d->keys[nk] = '$'; d->lits[nk++] = 0;
                } else {
                    t->len = 0; d->n_toks++;
                }
                t->type = TOK_NUM; t->off = (uint16_t)(nk - t->len);
                d->keys[nk] = dg; d->lits[nk++] = 0; t->len++;
            }
        } else if (is_letter_key(ch)) {
            if (last && last->type == TOK_NUM && lit_of(ch)) {
                /* Digits followed by an exactly spelled letter are one word (2e0abc, 90's). */
                int pure = 1;
                for (int k = last->off; k < nk; k++) if (d->keys[k] < '0' || d->keys[k] > '9') pure = 0;
                if (pure) {
                    last->type = TOK_WORD; last->lit = 0;
                    for (int k = last->off; k < nk; k++) { d->lits[k] = d->keys[k]; d->keys[k] = (char)('A' + (d->keys[k] - '0')); }
                }
            }
            if (!(last && last->type == TOK_WORD)) {
                t->type = TOK_WORD; t->lit = 0; t->off = (uint16_t)nk; t->len = 0; d->n_toks++;
                last = t;
            }
            if (press < hn) d->hints[nk] = hcp[press];
            d->keys[nk] = key_of(ch); d->lits[nk++] = lit_of(ch); last->len++;
        }
    }
    return OP_OK;
}

/* Write a number's text (see decoder.js formatNumber): each 's' is ',' when the groups read
 * as thousands, else '.'; with `time`, H s MM is H:MM. Returns its length, or OP_ERR_SPACE. */
static int format_number(const char *k, int kn, int time, char *out, int cap) {
    int commas = 1, group = 0, glen = 0, n = 0;
    for (int i = 0; i < kn; i++) {
        char c = k[i];
        if (c >= '0' && c <= '9') glen++;
        else if (c != '$') {
            if (group == 0 ? glen > 3 : glen != 3) commas = 0;
            group++; glen = 0;
        }
    }
    if (group == 0 && glen > 3) commas = 0;
    for (int i = 0; i < kn; i++) {
        char c = k[i];
        if (time && (c == 's' || c == '.')) c = ':';
        else if (c == 's') {
            int next = 0;
            for (int j = i + 1; j < kn && k[j] >= '0' && k[j] <= '9'; j++) next++;
            c = commas && next == 3 ? ',' : '.';
        }
        if (n + 1 >= cap) return OP_ERR_SPACE;
        out[n++] = c;
    }
    return n;
}

/* Can number keys k[0..kn) be a time H:MM (H <= 24, MM <= 59)? The separator ('s', '.' or
 * ':'), else 0. See decoder.js timeKind. */
static char time_kind(const char *k, int kn) {
    if (kn != 4 && kn != 5) return 0;
    char sep = k[kn - 3];
    if (sep != 's' && sep != '.' && sep != ':') return 0;
    int h = 0;
    for (int i = 0; i < kn; i++) {
        if (i == kn - 3) continue;
        if (k[i] < '0' || k[i] > '9') return 0;
        if (i < kn - 3) h = h * 10 + (k[i] - '0');
    }
    return h <= 24 && (k[kn - 2] - '0') * 10 + (k[kn - 1] - '0') <= 59 ? sep : 0;
}

/* ---- candidates ---- */

typedef struct { uint16_t id; uint8_t kind; int32_t pen; } cand;

/* Learned words may hold digits (call signs). As decoder.js userKeys / userTyped: a learned
 * word's digit 2-9 is its own key and 0/1 the held digit; a typed held digit 2-9 compares as
 * that key, and the exact characters (lit) must still agree. */
static char user_key(int c) {           /* a learned word's character (code point) */
    c = lower_cp(c);
    if (c == 39) return '#';
    if (c >= '0' && c <= '9') return c >= '2' ? (char)c : (char)('A' + (c - '0'));
    if (c >= 'a' && c <= 'z') return KEY_OF[c - 'a'];
    return table_key(c) ? table_key(c) : '?';
}
static void user_typed(const char *k, int kn, char *out) {
    for (int i = 0; i < kn; i++) out[i] = k[i] >= 'C' && k[i] <= 'J' ? (char)('0' + (k[i] - 'A')) : k[i];
}

/* How learned word t fits typed keys k (through user_typed): 1 whole, 2 whole without its
 * apostrophes, 3 its beginning (only if `partial`), else 0. See decoder.js userMatch. */
static int user_match(const char *t, const char *k, const uint16_t *lit, int kn, int partial) {
    if (!t[0]) return 0;
    char full[OP_USER_WORD_LEN] = { 0 }, strip[OP_USER_WORD_LEN] = { 0 };
    int fn = 0, sn = 0, tn = (int)strlen(t);     /* keys per character (UTF-8) */
    for (int i = 0; i < tn;) {
        int cp = next_cp(t, tn, &i);
        full[fn] = user_key(cp);
        if (cp != 39) strip[sn++] = full[fn];
        fn++;
    }
    int full_eq = fn == kn && memcmp(full, k, (size_t)kn) == 0;
    int strip_eq = sn != fn && sn == kn && memcmp(strip, k, (size_t)kn) == 0;
    if (full_eq || strip_eq) return lit_match_text(t, tn, !full_eq, lit, kn) ? (full_eq ? 1 : 2) : 0;
    if (!partial) return 0;
    int fs = fn >= kn && memcmp(full, k, (size_t)kn) == 0;
    int ss = !fs && sn >= kn && memcmp(strip, k, (size_t)kn) == 0;
    return (fs || ss) && lit_match_text(t, tn, ss, lit, kn) ? 3 : 0;
}

/* Learned words matching a word token: exact ones into out (up to OP_MAX_USER_CANDS), and
 * with `partial` completions into part. Mirrors the user loop in decoder.js slotCandidates. */
static void user_cands(const op_model *m, const op_user *u, const char *k, const uint16_t *lit, int kn,
                       int partial, cand *out, int *n_out, cand *part, int *n_part) {
    char typed[OP_USER_WORD_LEN];
    if (kn > OP_USER_WORD_LEN) return;      /* longer than any learned word */
    user_typed(k, kn, typed);
    for (int w = 0; w < u->n_words; w++) {
        uint16_t id = (uint16_t)(m->bos + 1 + w);
        int f = user_match(u->word[w], typed, lit, kn, partial);
        if ((f == 1 || f == 2) && *n_out < OP_MAX_USER_CANDS)
            out[(*n_out)++] = (cand){ id, OP_WORD, (int16_t)(f == 1 ? 0 : m->param[OP_P_APOSTROPHE]) };
        else if (f == 3 && *n_part < OP_MAX_USER_CANDS)
            part[(*n_part)++] = (cand){ id, OP_PARTIAL, m->param[OP_P_COMPLETION] };
    }
}

/* A run of word and number tokens with nothing between them (53, held 7, 272) that spells a
 * learned word is that one word; the last run may be its beginning. See decoder.js joinLearned. */
static int pure_number(const op_decoder *d, const op_tok *t) {
    if (t->type != TOK_NUM) return 0;
    for (int k = t->off; k < t->off + t->len; k++) if (d->keys[k] < '0' || d->keys[k] > '9') return 0;
    return 1;
}
static void join_learned(op_decoder *d) {
    const op_user *u = d->user;
    int any = 0;
    for (int w = 0; w < u->n_words && !any; w++) any = strpbrk(u->word[w], "0123456789") != NULL;
    if (!any) return;
    int out = 0;
    for (int i = 0; i < d->n_toks;) {
        int j = i, len = 0;
        while (j < d->n_toks && (d->toks[j].type == TOK_WORD || pure_number(d, &d->toks[j]))) len += d->toks[j++].len;
        if (j - i > 1 && len <= OP_USER_WORD_LEN) {
            int off = d->toks[i].off, match = 0;
            char k[OP_USER_WORD_LEN];
            uint16_t lit[OP_USER_WORD_LEN];
            for (int r = i; r < j; r++) {           /* spans are contiguous: nothing between them */
                const op_tok *t = &d->toks[r];
                for (int q = t->off; q < t->off + t->len; q++) {
                    char c = d->keys[q];
                    k[q - off] = t->type == TOK_NUM ? (char)('A' + (c - '0')) : c;
                    lit[q - off] = t->type == TOK_NUM ? (uint16_t)c : d->lits[q];
                }
            }
            char typed[OP_USER_WORD_LEN];
            user_typed(k, len, typed);
            for (int w = 0; w < u->n_words && !match; w++) match = user_match(u->word[w], typed, lit, len, j == d->n_toks);
            if (match) {
                memcpy(d->keys + off, k, (size_t)len);
                memcpy(d->lits + off, lit, (size_t)len * sizeof *lit);
                d->toks[out].type = TOK_WORD; d->toks[out].lit = 0;
                d->toks[out].off = (uint16_t)off; d->toks[out].len = (uint16_t)len;
                out++; i = j;
                continue;
            }
        }
        if (j == i) j = i + 1;
        while (i < j) d->toks[out++] = d->toks[i++];
    }
    d->n_toks = (uint16_t)out;
}

/* Elision (see decoder.js splitElisions): a word typed with an apostrophe that no word matches
 * whole ("l#homme") is an elided word (ending in an apostrophe: l', qu') and the rest, or a word
 * and endings that start with an apostrophe or a hyphen (Turkish "tom#a": tom + 'a; French
 * "a#t#il": a + -t + -il). word_fits: does a dictionary word (or with `kind` 0 a learned one)
 * fit keys [off, off + len) of the token? kind 1: one ending in an apostrophe, 2: an ending
 * (starting with an apostrophe or a hyphen), 3: any dictionary word. */
static int word_fits(const op_decoder *d, const op_model *m, const op_tok *t, int len, int kind) {
    const char *k = d->keys + t->off;
    const uint16_t *lit = d->lits + t->off;
    int starts;
    for (uint32_t i = lower_bound(m, m->key_idx, m->n_words, 0, k, len);
         i < m->n_words && key_cmp(m, AU16(m->key_idx, i), 0, k, len, &starts) == 0; i++) {
        uint16_t id = AU16(m->key_idx, i);
        if (!lit_match(m, id, 0, lit, len)) continue;
        if (kind == 1 || kind == 2) {
            const char *w;
            char wb[OP_WORD_BUF];
            int wn = word_at(m, id, &w, wb);
            if (!wn || (kind == 1 ? w[wn - 1] != '\'' : w[0] != '\'' && w[0] != '-')) continue;
        }
        return 1;
    }
    if (kind == 0 && d->user && len <= OP_USER_WORD_LEN) {
        char typed[OP_USER_WORD_LEN];
        user_typed(k, len, typed);
        for (int w = 0; w < d->user->n_words; w++) {
            int f = user_match(d->user->word[w], typed, lit, len, 0);
            if (f == 1 || f == 2) return 1;
        }
    }
    return 0;
}
static int is_qwerty(const op_decoder *d, const op_tok *t);
static int qwerty_cands(const op_decoder *d, const op_model *m, const op_tok *t, int is_last,
                        cand *out, cand *comp, int *n_comp);
static int split_elisions(op_decoder *d, const op_model *m) {
    for (int i = 0; i < d->n_toks; i++) {
        op_tok *t = &d->toks[i];
        if (t->type != TOK_WORD) continue;
        const char *k = d->keys + t->off;
        int p = 0, q, cut, hyphen = 0;              /* cut: where the second word starts */
        while (p < t->len && k[p] != '#') p++;
        if (p >= t->len - 1) continue;
        q = p + 1;                                  /* the next # after p, or the end */
        while (q < t->len && k[q] != '#') q++;
        op_tok e = *t;                              /* keys [p, q): an ending? */
        e.off = (uint16_t)(t->off + p);
        e.len = (uint16_t)(q - p);
        if (p == 0) {                               /* an ending split off, and another # after it */
            if (!m->elided || q >= t->len || !word_fits(d, m, &e, e.len, 2)) continue;
            cut = q;
        } else {
            if (word_fits(d, m, t, t->len, 0)) continue;
            if (is_qwerty(d, t)) {                  /* a word the taps fit (a slip in "i'll" changes its digits) */
                cand qc[OP_MAX_EXACT], qcomp[OP_MAX_COMPLETIONS];
                int n_qc;
                if (qwerty_cands(d, m, t, 0, qc, qcomp, &n_qc) > 0) continue;
            }
            if (m->elided && word_fits(d, m, t, p + 1, 1)) cut = p + 1;
            else if (!word_fits(d, m, t, p, 3)) continue;
            else if (m->elided && word_fits(d, m, &e, e.len, 2)) cut = p;
            else {                                  /* two words joined by a hyphen: well-known */
                op_tok r = *t;
                r.off = (uint16_t)(t->off + p + 1);
                r.len = (uint16_t)(q - p - 1);
                if (d->lits[t->off + p] || !r.len || !word_fits(d, m, &r, r.len, 3)) continue;   /* an exact ' stays one */
                cut = p + 1;
                hyphen = 1;
            }
        }
        if (d->n_toks == OP_MAX_KEYS) return OP_ERR_TOO_LONG;
        memmove(t + 1, t, (size_t)(d->n_toks - i) * sizeof *t);
        d->n_toks++;
        t[0].len = (uint16_t)(cut - hyphen);        /* the # of a hyphen belongs to neither word */
        t[1].off = (uint16_t)(t[0].off + cut);
        t[1].len = (uint16_t)(t[1].len - cut);
        t[1].lit = hyphen ? '-' : 0;                /* a word token's lit '-': shown after a hyphen */
        /* t[1] is looked at next: it may hold another elision */
    }
    return OP_OK;
}

static int punct_index(const op_model *m, char ch) {
    for (int i = 0; i < m->n_punct; i++) if (ACH(m->punct, i) == ch) return i;
    return 0;
}
static int has_punct(const op_model *m, char ch) {
    for (int i = 0; i < m->n_punct; i++) if (ACH(m->punct, i) == ch) return 1;
    return 0;
}

/* Known words run together (lollypops: lolly + pops) for a word nothing matches: every split
 * into 2 or 3 parts of at least COMPOUND_PART keys, each part one of its COMPOUND_WORDS most
 * frequent words that agree with the exact letters, scored by how likely each part is to start
 * and end such a word (cmpHead / cmpTail) plus COMPOUND_PAIR x any lift a known word pair gives
 * (ice cream), each split costing COMPOUND_SPLIT nats; the
 * COMPOUND_MAX best distinct spellings. Same splits, order and ties as decoder.js compounds. */
#define COMPOUND_PART 2
#define COMPOUND_WORDS 5
#define COMPOUND_SPLIT 2
#define COMPOUND_PAIR 2
#define COMPOUND_KNOWN 10       /* nats for two parts known written as one (cmpBloom: "lollypops") */
static uint32_t fmix32(uint32_t h) {
    h ^= h >> 16; h *= 0x85ebca6bu; h ^= h >> 13; h *= 0xc2b2ae35u;
    return h ^ (h >> 16);
}
/* Are words a and b known written as one? The Bloom filter, else (older files) the sorted list. */
static int known_pair(const op_model *m, uint16_t a, uint16_t b) {
    uint32_t key = (uint32_t)a << 16 | b;
    if (!m->cmp_bloom) return find_u32(m->cmp_known, m->n_known, key) >= 0;
    uint32_t nbits = AU32(m->cmp_bloom, 0), h1 = fmix32(key), h2 = fmix32(key ^ 0x9e3779b9u) | 1;
    for (uint32_t i = 0; i < AU32(m->cmp_bloom, 1); i++) {
        uint32_t bit = (h1 + i * h2) % nbits;
        if (!(AU32(m->cmp_bloom, 2 + (bit >> 5)) & (1u << (bit & 31)))) return 0;
    }
    return 1;
}
#define COMPOUND_MAX 6
#define COMPOUND_KEYS 24
typedef struct { int32_t score; uint8_t len; char text[64]; } compound;

/* The words for keys k[0..kn), most frequent first, and their penalties; how many. */
static int compound_part(const op_model *m, const char *k, const uint16_t *lit, int kn, uint16_t *id, int16_t *pen) {
    int strip = !memchr(k, '#', (size_t)kn), starts, n = 0;
    uint32_t a = lower_bound(m, m->key_idx, m->n_words, 0, k, kn);
    uint32_t b = lower_bound(m, m->strip_idx, m->n_strip, 1, k, kn);
    int a_ok = a < m->n_words && key_cmp(m, AU16(m->key_idx, a), 0, k, kn, &starts) == 0;
    int b_ok = b < m->n_strip && key_cmp(m, AU16(m->strip_idx, b), 1, k, kn, &starts) == 0;
    while ((a_ok || b_ok) && n < COMPOUND_WORDS) {
        int from_strip = !a_ok || (b_ok && AU16(m->strip_idx, b) < AU16(m->key_idx, a));
        uint16_t w;
        if (from_strip) {
            w = AU16(m->strip_idx, b++);
            b_ok = b < m->n_strip && key_cmp(m, AU16(m->strip_idx, b), 1, k, kn, &starts) == 0;
        } else {
            w = AU16(m->key_idx, a++);
            a_ok = a < m->n_words && key_cmp(m, AU16(m->key_idx, a), 0, k, kn, &starts) == 0;
        }
        const char *wt;             /* endings ('a, -ce) and elided words (l') attach only through a typed # */
        char wb[OP_WORD_BUF];
        int wtn = word_at(m, w, &wt, wb);
        if (wtn && (wt[0] == '\'' || wt[0] == '-' || wt[wtn - 1] == '\'')) continue;
        if (!lit_match(m, w, strip, lit, kn)) continue;
        id[n] = w;
        pen[n++] = from_strip ? m->param[OP_P_APOSTROPHE] : 0;
    }
    return n;
}

static int32_t compound_score(const op_model *m, uint16_t id, int first, int last) {
    int32_t q = m->scale >> 2, s = 0;
    if (!last) s += AI16(m->uni, id) + (m->cmp_head ? AI8(m->cmp_head, id) : 0) * q;
    if (!first) s += AI16(m->uni, id) + (m->cmp_tail ? AI8(m->cmp_tail, id) : 0) * q;
    return s;
}

static void compound_add(const op_model *m, compound *best, int *nb, int32_t score, const uint16_t *ids, int np) {
    compound c;
    c.score = score;
    c.len = 0;
    for (int i = 0; i < np; i++) {
        const char *w;
        char wb[OP_WORD_BUF];
        int wn = word_at(m, ids[i], &w, wb);
        memcpy(c.text + c.len, w, (size_t)wn);        /* at most 2 * COMPOUND_KEYS: fits */
        c.len = (uint8_t)(c.len + wn);
    }
    for (int d = 0; d < *nb; d++) {
        if (best[d].len != c.len || memcmp(best[d].text, c.text, c.len)) continue;
        if (best[d].score >= score) return;
        memmove(best + d, best + d + 1, (size_t)(*nb - d - 1) * sizeof *best);
        (*nb)--;
        break;
    }
    int k = *nb;
    while (k > 0 && best[k - 1].score < score) k--;
    if (k >= COMPOUND_MAX) return;
    if (*nb == COMPOUND_MAX) (*nb)--;
    memmove(best + k + 1, best + k, (size_t)(*nb - k) * sizeof *best);
    best[k] = c;
    (*nb)++;
}

/* Split k[0..kn) at ends[0..np) (the last is kn) and add each choice of words. */
static void compound_split(const op_model *m, const char *k, const uint16_t *lit, const int *ends, int np,
                           compound *best, int *nb) {
    uint16_t id[3][COMPOUND_WORDS];
    int16_t pen[3][COMPOUND_WORDS];
    int32_t sc[3][COMPOUND_WORDS], top[4] = { 0, 0, 0, 0 };    /* top[i]: best possible from part i on */
    int32_t pl[2][COMPOUND_WORDS][COMPOUND_WORDS];               /* pair lift between parts i, i + 1 */
    int cnt[3] = { 1, 1, 1 };
    for (int i = 0, a = 0; i < np; a = ends[i++]) {
        cnt[i] = compound_part(m, k + a, lit + a, ends[i] - a, id[i], pen[i]);
        if (!cnt[i]) return;
        for (int x = 0; x < cnt[i]; x++) sc[i][x] = compound_score(m, id[i][x], i == 0, i == np - 1) + pen[i][x];
    }
    for (int i = 0; i + 1 < np; i++)
        for (int x = 0; x < cnt[i]; x++)
            for (int y = 0; y < cnt[i + 1]; y++) {
                int32_t g = lp(m, id[i][x], id[i + 1][y]) - AI16(m->uni, id[i + 1][y]);
                pl[i][x][y] = g > 0 ? COMPOUND_PAIR * g : 0;
                if (np == 2 && known_pair(m, id[0][x], id[1][y]))
                    pl[i][x][y] += COMPOUND_KNOWN * m->scale;
            }
    for (int i = np - 1; i >= 0; i--) {
        for (int y = 0; y < cnt[i]; y++) {
            int32_t s = sc[i][y];
            if (i) {
                int32_t g = 0;
                for (int x = 0; x < cnt[i - 1]; x++) if (pl[i - 1][x][y] > g) g = pl[i - 1][x][y];
                s += g;
            }
            if (y == 0 || s > top[i]) top[i] = s;
        }
        top[i] += top[i + 1];
    }
    /* Skipping what can't beat the last of a full list leaves the result unchanged. */
    #define HOPELESS(s) (*nb == COMPOUND_MAX && (s) <= best[COMPOUND_MAX - 1].score)
    int32_t base = -(np - 1) * COMPOUND_SPLIT * m->scale;
    for (int x = 0; x < cnt[0]; x++) {
        int32_t s0 = base + sc[0][x];
        if (HOPELESS(s0 + top[1])) continue;
        for (int y = 0; y < cnt[1]; y++) {
            int32_t s1 = s0 + sc[1][y] + pl[0][x][y];
            if (HOPELESS(s1 + top[2])) continue;
            for (int z = 0; z < (np == 3 ? cnt[2] : 1); z++) {
                uint16_t ids[3] = { id[0][x], id[1][y], np == 3 ? id[2][z] : 0 };
                compound_add(m, best, nb, np == 3 ? s1 + sc[2][z] + pl[1][y][z] : s1, ids, np);
            }
        }
    }
    #undef HOPELESS
}

static int compounds(const op_model *m, const char *k, const uint16_t *lit, int kn, compound *best) {
    int nb = 0;
    if (kn < 2 * COMPOUND_PART || kn > COMPOUND_KEYS) return 0;
    for (int i = COMPOUND_PART; i <= kn - COMPOUND_PART; i++) {
        int ends[2] = { i, kn };
        compound_split(m, k, lit, ends, 2, best, &nb);
    }
    for (int i = COMPOUND_PART; i <= kn - 2 * COMPOUND_PART; i++)
        for (int j = i + COMPOUND_PART; j <= kn - COMPOUND_PART; j++) {
            int ends[3] = { i, j, kn };
            compound_split(m, k, lit, ends, 3, best, &nb);
        }
    return nb;
}

/* The OP_MAX_COMPLETIONS most frequent distinct words whose keys start with k[0..kn) (and agree
 * with lit, if given), ascending by id. See decoder.js prefixCandidates. */
static int prefix_best(const op_model *m, const char *k, int kn, const uint16_t *lit, uint16_t *best) {
    int nb = 0, starts;
    for (int strip = 0; strip < 2; strip++) {
        OP_REF(uint16_t) ix = strip ? m->strip_idx : m->key_idx;
        uint32_t ixn = strip ? m->n_strip : m->n_words;
        for (uint32_t i = lower_bound(m, ix, ixn, strip, k, kn); i < ixn; i++) {
            key_cmp(m, AU16(ix, i), strip, k, kn, &starts);
            if (!starts) break;
            uint16_t id = AU16(ix, i);
            if (lit && !lit_match(m, id, strip, lit, kn)) continue;
            int dup = 0;
            for (int j = 0; j < nb; j++) if (best[j] == id) dup = 1;
            if (dup) continue;
            if (nb < OP_MAX_COMPLETIONS) best[nb++] = id;
            else if (id < best[nb - 1]) best[nb - 1] = id;
            else continue;
            for (int j = nb - 1; j > 0 && best[j] < best[j - 1]; j--) {
                uint16_t x = best[j]; best[j] = best[j - 1]; best[j - 1] = x;
            }
        }
    }
    return nb;
}

#ifndef OP_NO_QWERTY
/* ---- QWERTY: the letters tapped are evidence too (see decoder.js qwertyCandidates) ---- */
/* Where letter cp is on the layout: 2 * x (row r shifted by r/2 key) and the row; 0 if absent. */
static int layout_pos(const char *lay, int cp, int *x2, int *y) {
    int n = (int)strlen(lay), row = 0, col = 0;
    for (int i = 0; i < n;) {
        int c = next_cp(lay, n, &i);
        if (c == '|') { row++; col = 0; continue; }
        if (c == cp) { *x2 = 2 * col + row; *y = row; return 1; }
        col++;
    }
    return 0;
}
/* Are letters a and b neighbors on the layout (decoder.js layoutNear)? */
static int qw_near(const char *lay, int a, int b) {
    int xa, ya, xb, yb, d;
    if (!layout_pos(lay, a, &xa, &ya) || !layout_pos(lay, b, &xb, &yb)) return 0;
    d = (xa - xb) * (xa - xb) + 4 * (ya - yb) * (ya - yb);
    return d > 0 && d <= 5;
}
#define QW_NEAR (-32)           /* 1/4 nat: a neighboring key (-8 nats) */
#define QW_FAR (-96)            /* a letter only on the same keypad key (-24 nats) */
#define QW_MAX_KEYS 32

static int is_qwerty(const op_decoder *d, const op_tok *t) {
    int any = 0;
    if (t->len > QW_MAX_KEYS) return 0;
    for (int i = t->off; i < t->off + t->len; i++) {
        if (d->lits[i]) return 0;
        if (d->hints[i]) any = 1; else if (d->keys[i] != '#') return 0;
    }
    return any;
}
/* What the taps say about word w[0..wn) as the token's letters, in model units. */
static int32_t tap_cost(const op_model *m, const op_decoder *d, const op_tok *t, const char *w, int wn) {
    int strip = !memchr(d->keys + t->off, '#', (size_t)t->len), j = 0;
    int32_t c = 0, q = m->scale >> 2;
    for (int i = 0; i < wn && j < t->len;) {
        int cp = lower_cp(next_cp(w, wn, &i)), x, y;
        if (strip && cp == '\'') continue;
        uint16_t h = d->hints[t->off + j++];
        if (!h || lit_ok(h, cp)) continue;
        /* a neighbor; a word letter not on the keyboard is compared through its plain letter */
        c += qw_near(d->layout, h, layout_pos(d->layout, cp, &x, &y) ? cp : plain_cp(cp)) ? QW_NEAR * q : QW_FAR * q;
    }
    return c;
}
#else    /* -DOP_NO_QWERTY: words typed on QWERTY are matched by their keypad digits only */
static int is_qwerty(const op_decoder *d, const op_tok *t) { (void)d; (void)t; return 0; }
static int32_t tap_cost(const op_model *m, const op_decoder *d, const op_tok *t, const char *w, int wn) {
    (void)m; (void)d; (void)t; (void)w; (void)wn;
    return 0;
}
#endif
/* Best candidates by (unigram + pen) descending, then id; one entry per id. */
typedef struct { cand c[OP_MAX_EXACT + OP_MAX_COMPLETIONS]; int32_t s[OP_MAX_EXACT + OP_MAX_COMPLETIONS]; int n, max; } qw_list;
static void qw_add(qw_list *l, cand c, int32_t score) {
    for (int i = 0; i < l->n; i++) {
        if (l->c[i].id != c.id) continue;
        if (l->s[i] >= score) return;
        memmove(l->c + i, l->c + i + 1, (size_t)(l->n - i - 1) * sizeof *l->c);
        memmove(l->s + i, l->s + i + 1, (size_t)(l->n - i - 1) * sizeof *l->s);
        l->n--;
        break;
    }
    int k = l->n;
    while (k > 0 && (l->s[k - 1] < score || (l->s[k - 1] == score && l->c[k - 1].id > c.id))) k--;
    if (k >= l->max) return;
    if (l->n == l->max) l->n--;
    memmove(l->c + k + 1, l->c + k, (size_t)(l->n - k) * sizeof *l->c);
    memmove(l->s + k + 1, l->s + k, (size_t)(l->n - k) * sizeof *l->s);
    l->c[k] = c;
    l->s[k] = score;
    l->n++;
}
#ifndef OP_NO_QWERTY
typedef struct {
    const op_decoder *d; const op_model *m; const op_tok *t;
    char allowed[QW_MAX_KEYS][8], pre[QW_MAX_KEYS];
    int is_last;
    qw_list ex, comp;
} qw_walk_state;
static void qw_walk(qw_walk_state *w, int len) {
    const op_model *m = w->m;
    int starts, any = 0;
    for (int strip = 0; strip < 2 && !any; strip++) {
        OP_REF(uint16_t) ix = strip ? m->strip_idx : m->key_idx;
        uint32_t ixn = strip ? m->n_strip : m->n_words, i = lower_bound(m, ix, ixn, strip, w->pre, len);
        if (i < ixn) { key_cmp(m, AU16(ix, i), strip, w->pre, len, &starts); any = starts; }
    }
    if (!any) return;
    if (len < w->t->len) {
        for (const char *dg = w->allowed[len]; *dg; dg++) { w->pre[len] = *dg; qw_walk(w, len + 1); }
        return;
    }
    const char *text;
    char wb[OP_WORD_BUF];
    for (int strip = 0; strip < 2; strip++) {
        OP_REF(uint16_t) ix = strip ? m->strip_idx : m->key_idx;
        uint32_t ixn = strip ? m->n_strip : m->n_words;
        for (uint32_t i = lower_bound(m, ix, ixn, strip, w->pre, len); i < ixn && key_cmp(m, AU16(ix, i), strip, w->pre, len, &starts) == 0; i++) {
            int tn = word_at(m, AU16(ix, i), &text, wb);
            int32_t pen = (strip ? m->param[OP_P_APOSTROPHE] : 0) + tap_cost(m, w->d, w->t, text, tn);
            qw_add(&w->ex, (cand){ AU16(ix, i), OP_WORD, pen }, AI16(m->uni, AU16(ix, i)) + pen);
        }
    }
    if (w->is_last) {
        uint16_t best[OP_MAX_COMPLETIONS];
        int nb = prefix_best(m, w->pre, len, NULL, best);
        for (int j = 0; j < nb; j++) {
            int tn = word_at(m, best[j], &text, wb);
            if (u8_len(text, tn) <= len) continue;
            int32_t pen = m->param[OP_P_COMPLETION] + tap_cost(m, w->d, w->t, text, tn);
            qw_add(&w->comp, (cand){ best[j], OP_PARTIAL, pen }, AI16(m->uni, best[j]) + pen);
        }
    }
}
/* Exact candidates into out (returns how many), completions into comp (*n_comp). */
static int qwerty_cands(const op_decoder *d, const op_model *m, const op_tok *t, int is_last,
                        cand *out, cand *comp, int *n_comp) {
    qw_walk_state w;            /* ~1.6 KB of stack, only for words typed on QWERTY */
    w.d = d; w.m = m; w.t = t; w.is_last = is_last;
    w.ex.n = w.comp.n = 0;
    w.ex.max = OP_MAX_EXACT;
    w.comp.max = OP_MAX_EXACT + OP_MAX_COMPLETIONS;    /* the exact ones are dropped below */
    for (int i = 0; i < t->len; i++) {
        /* The keypad digits of the tapped letter and its neighbors, ascending. */
        uint16_t h = d->hints[t->off + i];
        char *a = w.allowed[i];
        int na = 0, ln = (int)strlen(d->layout);
        if (!h) { a[0] = '#'; a[1] = 0; continue; }
        /* the tapped letter, then its neighbors on the layout */
        for (int li = -1; li < ln;) {
            int c;
            if (li < 0) { c = h; li = 0; }
            else { c = next_cp(d->layout, ln, &li); if (c == '|' || !qw_near(d->layout, h, c)) continue; }
            char dg = c >= 'a' && c <= 'z' ? KEY_OF[c - 'a'] : table_key(c);
            if (!dg) continue;
            int k = 0;
            while (k < na && a[k] < dg) k++;
            if (k < na && a[k] == dg) continue;
            memmove(a + k + 1, a + k, (size_t)(na - k));
            a[k] = dg;
            na++;
        }
        a[na] = 0;
    }
    qw_walk(&w, 0);
    for (int i = 0; i < w.ex.n; i++) out[i] = w.ex.c[i];
    *n_comp = 0;
    for (int i = 0; i < w.comp.n && *n_comp < OP_MAX_COMPLETIONS; i++) {
        int dup = 0;
        for (int e = 0; e < w.ex.n; e++) if (w.ex.c[e].id == w.comp.c[i].id) dup = 1;
        if (!dup) comp[(*n_comp)++] = w.comp.c[i];
    }
    return w.ex.n;
}
#else
static int qwerty_cands(const op_decoder *d, const op_model *m, const op_tok *t, int is_last,
                        cand *out, cand *comp, int *n_comp) {
    (void)d; (void)m; (void)t; (void)is_last; (void)out; (void)comp;
    *n_comp = 0;
    return 0;
}
#endif

/* ---- Keypad slips (see decoder.js slipCandidates) ---- */
static const char *const KEYPAD_NEAR[8] = { "35", "26", "57", "2468", "359", "48", "579", "68" };   /* keys 2-9 */
#define KP_MIN_KEYS 3

static void slip_add(const op_model *m, const char *k, int kn, int32_t pen, qw_list *l) {
    int starts;
    for (int strip = 0; strip < 2; strip++) {
        OP_REF(uint16_t) ix = strip ? m->strip_idx : m->key_idx;
        uint32_t ixn = strip ? m->n_strip : m->n_words;
        for (uint32_t i = lower_bound(m, ix, ixn, strip, k, kn); i < ixn && key_cmp(m, AU16(ix, i), strip, k, kn, &starts) == 0; i++) {
            int32_t p = pen + (strip ? m->param[OP_P_APOSTROPHE] : 0);
            qw_add(l, (cand){ AU16(ix, i), OP_WORD, p }, AI16(m->uni, AU16(ix, i)) + p);
        }
    }
}
/* Words one slip away from keypad word t, not already among out[0..n_have), into out + n_have. */
static int slip_cands(const op_decoder *d, const op_model *m, const op_tok *t, cand *out, int n_have) {
    int32_t sub = m->param[OP_P_SLIP_SUB], dup = m->param[OP_P_SLIP_DUP];
    const char *k = d->keys + t->off;
    int kn = t->len;
    if ((!sub && !dup) || kn < KP_MIN_KEYS) return 0;
    for (int i = t->off; i < t->off + kn; i++) if (d->lits[i] || d->hints[i]) return 0;
    qw_list l;                  /* the best, kept long enough to drop the ones already offered */
    l.n = 0;
    l.max = OP_MAX_SLIPS + n_have < OP_MAX_EXACT + OP_MAX_COMPLETIONS ? OP_MAX_SLIPS + n_have : OP_MAX_EXACT + OP_MAX_COMPLETIONS;
    char v[OP_MAX_KEYS];
    for (int i = 0; i < kn; i++) {
        int near = k[i] >= '2' && k[i] <= '9';
        if (sub && near) {
            memcpy(v, k, (size_t)kn);
            for (const char *c = KEYPAD_NEAR[k[i] - '2']; *c; c++) { v[i] = *c; slip_add(m, v, kn, sub, &l); }
        }
        if (dup && near && i && k[i] == k[i - 1]) {
            memcpy(v, k, (size_t)i);
            memcpy(v + i, k + i + 1, (size_t)(kn - i - 1));
            slip_add(m, v, kn - 1, dup, &l);
        }
    }
    int n = 0;
    for (int i = 0; i < l.n && n < OP_MAX_SLIPS; i++) {
        int seen = 0;
        for (int e = 0; e < n_have; e++) if (out[e].id == l.c[i].id) seen = 1;
        if (!seen) out[n_have + n++] = l.c[i];
    }
    return n;
}

static int slot_cands(const op_decoder *d, const op_model *m, const op_tok *t, int is_last, cand *out) {
    const op_user *u = d->user;
    if (t->type == TOK_NUM) {
        char tk = time_kind(d->keys + t->off, t->len);
        if (tk == ':') { out[0] = (cand){ OP_TIME_ID, OP_TIME, 0 }; return 1; }
        out[0] = (cand){ OP_NONE, OP_NUMBER, 0 };
        if (!tk) return 1;
        out[1] = (cand){ OP_TIME_ID, OP_TIME, 0 };
        return 2;
    }
    if (t->type == TOK_PUNCT) {
        if (t->lit && !has_punct(m, t->lit)) { out[0] = (cand){ OP_NONE, OP_LITERAL, 0 }; return 1; }   /* ':' */
        if (t->lit) { out[0] = (cand){ (uint16_t)(m->n_words + punct_index(m, t->lit)), OP_PUNCT, 0 }; return 1; }
        for (int i = 0; i < m->n_punct; i++) out[i] = (cand){ (uint16_t)(m->n_words + i), OP_PUNCT, 0 };
        return m->n_punct;
    }
    const char *k = d->keys + t->off;
    const uint16_t *lit = d->lits + t->off;
    int kn = t->len, has_lit = 0, all_lit = 1, has_hash = 0, n = 0;
    for (int i = 0; i < kn; i++) {
        if (lit[i]) has_lit = 1; else all_lit = 0;
        if (k[i] == '#') has_hash = 1;
    }
    int qw = is_qwerty(d, t), n_qc = 0;
    cand qc[OP_MAX_COMPLETIONS];
    if (qw) {
        n = qwerty_cands(d, m, t, is_last, out, qc, &n_qc);
    } else {
        /* Exact matches from both indexes, merged into id (frequency) order. A contraction typed
         * without its apostrophe ("im" for I'm) matches through stripIdx with a small penalty. */
        int starts;
        uint32_t a = lower_bound(m, m->key_idx, m->n_words, 0, k, kn), ae = a;
        uint32_t b = lower_bound(m, m->strip_idx, m->n_strip, 1, k, kn), be = b;
        while (ae < m->n_words && key_cmp(m, AU16(m->key_idx, ae), 0, k, kn, &starts) == 0) ae++;
        while (be < m->n_strip && key_cmp(m, AU16(m->strip_idx, be), 1, k, kn, &starts) == 0) be++;
        while ((a < ae || b < be) && n < OP_MAX_EXACT) {
            int from_strip = a >= ae || (b < be && AU16(m->strip_idx, b) < AU16(m->key_idx, a));
            uint16_t id = from_strip ? AU16(m->strip_idx, b++) : AU16(m->key_idx, a++);
            if (has_lit && !lit_match(m, id, !has_hash, lit, kn)) continue;
            out[n++] = (cand){ id, OP_WORD, from_strip ? m->param[OP_P_APOSTROPHE] : 0 };
        }
    }
    cand upart[OP_MAX_USER_CANDS];
    int n_upart = 0;
    if (u) {
        int n_uexact = 0;
        user_cands(m, u, k, lit, kn, is_last && !all_lit, out + n, &n_uexact, upart, &n_upart);
        if (qw) {               /* what the taps say about each learned word */
            const char *w;
            char wb[OP_WORD_BUF];
            for (int j = 0; j < n_uexact; j++) {
                int wn = tok_text(m, u, out[n + j].id, &w, wb);
                out[n + j].pen += tap_cost(m, d, t, w, wn);
            }
            for (int j = 0; j < n_upart; j++) {
                int wn = tok_text(m, u, upart[j].id, &w, wb);
                upart[j].pen += tap_cost(m, d, t, w, wn);
            }
        }
        n += n_uexact;
    }
    if (!qw && !n) n = slip_cands(d, m, t, out, 0);     /* nothing matches: a slip? */
    int n_exact = n;
    if (qw) {
        for (int j = 0; j < n_qc; j++) {
            int seen = 0;
            for (int e = 0; e < n_exact; e++) if (out[e].id == qc[j].id) seen = 1;
            if (!seen) out[n++] = qc[j];
        }
        for (int j = 0; j < n_upart; j++) out[n++] = upart[j];
    } else if (is_last && !all_lit) {
        /* While still typing, also offer the most frequent completions. */
        uint16_t best[OP_MAX_COMPLETIONS];
        int nb = prefix_best(m, k, kn, has_lit ? lit : NULL, best);
        for (int j = 0; j < nb; j++) {
            int seen = 0;
            for (int e = 0; e < n_exact; e++) if (out[e].id == best[j]) seen = 1;
            if (!seen) out[n++] = (cand){ best[j], OP_PARTIAL, m->param[OP_P_COMPLETION] };
        }
        for (int j = 0; j < n_upart; j++) out[n++] = upart[j];
    }
    if (all_lit) {
        /* Spelled out exactly: keep only words spelled exactly that way, else the text as typed. */
        int kept = 0;
        for (int e = 0; e < n; e++) {
            const char *w;
            char wb[OP_WORD_BUF];
            int wn = tok_text(m, u, out[e].id, &w, wb);
            if (u8_len(w, wn) == kn && same_letters(w, wn, lit, kn)) out[kept++] = out[e];
        }
        if (kept) return kept;
        out[0] = (cand){ OP_NONE, OP_LITERAL, 0 };
        return 1;
    }
    if (!n) {
        /* No word fits: known words run together, unless every letter is known (QWERTY). */
        int typed = 1;
        for (int i = 0; i < kn; i++) if (!lit[i] && !d->hints[t->off + i] && k[i] != '#') typed = 0;
        if (!typed) {
            compound cs[COMPOUND_MAX];
            int nc = compounds(m, k, lit, kn, cs);
            for (int r = 0; r < nc; r++) out[n++] = (cand){ (uint16_t)(OP_COMPOUND_ID + r), OP_COMPOUND, 0 };
        }
        if (!n) out[n++] = (cand){ OP_NONE, OP_UNKNOWN, 0 };
    }
    return n;
}

/* ---- context ---- */

#define RECENT_BONUS 3          /* nats for a word used in the text before (see decoder.js) */
#define RECENT_MAX_UNI (-9)     /* ... unless it's this common (log P above -9 nats) */

/* Id of a word from the text before: the dictionary, then learned words, else OP_NONE. */
static uint16_t context_word(const op_model *m, const op_user *u, const char *w, int wn) {
    int letters = 1;            /* no digits: a-z, apostrophes and (UTF-8) table letters */
    for (int i = 0; i < wn; i++) if (!((w[i] >= 'a' && w[i] <= 'z') || w[i] == 39 || (unsigned char)w[i] >= 0x80)) letters = 0;
    if (letters && wn < 64) {
        char buf[64];
        memcpy(buf, w, (size_t)wn);
        buf[wn] = 0;
        int id = find_word(m, buf);
        if (id != OP_NONE) return (uint16_t)id;
    }
    for (int k = 0; u && k < u->n_words; k++) {
        const char *t = u->word[k];
        int i = 0;
        while (i < wn && t[i] && (t[i] >= 'A' && t[i] <= 'Z' ? t[i] + 32 : t[i]) == w[i]) i++;
        if (i == wn && !t[i]) return (uint16_t)(m->bos + 1 + k);
    }
    return OP_NONE;
}

/* The context a composition starts in, from start->before (decoder.js context): the last words,
 * whether to capitalize, the opener of an unfinished sentence and the recently used words. */
static void context(op_decoder *d, const op_model *m, const op_start *start,
                    uint16_t *prev2, uint8_t *first, uint8_t *cf) {
    *prev2 = OP_NONE;
    *first = *cf = 0;
    d->ctx_prev = start ? start->prev : m->bos;
    d->ctx_cap = start ? start->cap : 1;
    d->n_recent = 0;
    d->n_line = 0;
    if (!start || !start->before) return;
    const unsigned char *b = (const unsigned char *)start->before;
    uint16_t p2 = OP_NONE, p1 = m->bos, recent[OP_RECENT_WORDS], line[OP_NN_LINE];
    int n_recent = 0, cap = 1, n_line = 0;
    #define LINE_PUSH(id) do { if (n_line == OP_NN_LINE) { memmove(line, line + 1, sizeof line - sizeof *line); n_line--; } \
                               line[n_line++] = (id); } while (0)
    uint8_t f = 0, g = 0;
    char w[72];
    while (*b) {
        unsigned char c = *b;
        int wn = 0, long_word = 0;
        /* a run of letters (a-z and the table's), digits and apostrophes (curly ones included),
         * lowercased through the table (decoder.js beforeTokens); over 72 bytes: unknown */
        for (;;) {
            char x = 0;
            int cp = 0;
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == 39) x = (char)c;
            else if (c >= 'A' && c <= 'Z') x = (char)(c + 32);
            else if (c == 0xE2 && b[1] == 0x80 && (b[2] == 0x98 || b[2] == 0x99)) { x = 39; b += 2; }
            else if (c >= 0xC2 && c < 0xE0 && (b[1] & 0xC0) == 0x80) {
                int at = 0, t, k;
                cp = next_cp((const char *)b, 2, &at);
                k = ct_find(cp, &t);
                if (k < 0) cp = 0;
                else { cp += CHARTABLE[t].ldelta[k]; b++; }
            }
            if (!x && !cp) break;
            if (cp) {
                if (wn + 2 <= (int)sizeof w) { w[wn++] = (char)(0xC0 | cp >> 6); w[wn++] = (char)(0x80 | (cp & 0x3F)); }
                else long_word = 1;
            } else if (wn < (int)sizeof w) w[wn++] = x;
            else long_word = 1;
            c = *++b;
        }
        if (wn) {
            cap = 0;
            int digits = 1;
            for (int i = 0; i < wn; i++) if (w[i] < '0' || w[i] > '9') digits = 0;
            if (digits) { p2 = OP_NONE; p1 = AFTER_NUMBER; LINE_PUSH(OP_NONE); continue; }
            uint16_t id = long_word ? OP_NONE : context_word(m, d->user, w, wn);
            LINE_PUSH(id <= m->bos ? id : OP_NONE);         /* learned words are unknown to it */
            p2 = p1; p1 = id;
            int skip = id != OP_NONE && in_u16(m->q_skip, m->n_skip, id);
            if (!f && !skip) f = q_state(m, id);
            if (!g && !skip) g = q_state(m, id);
            if (id != OP_NONE) {
                if (n_recent == OP_RECENT_WORDS) { memmove(recent, recent + 1, sizeof recent - sizeof *recent); n_recent--; }
                recent[n_recent++] = id;
            }
            continue;
        }
        if (c == 0xD8 && (b[1] == 0x9F || b[1] == 0x8C)) { c = b[1] == 0x9F ? '?' : ','; b++; }   /* Arabic ؟ ، */
        if (c == '\n') n_line = 0;
        else if (c == '.' || c == '!' || c == '?' || c == ',') LINE_PUSH((uint16_t)(m->n_words + punct_index(m, (char)c)));
        if (c == '.' || c == '!' || c == '?' || c == '\n') {     /* a line break ends a sentence too */
            p2 = OP_NONE; p1 = m->bos; f = g = 0; cap = 1;
        } else if (c == ',') {
            p2 = p1; p1 = (uint16_t)(m->n_words + punct_index(m, ',')); g = 0; cap = 0;
        }
        b++;
    }
    *prev2 = p2;
    *first = f;
    *cf = g;
    d->ctx_prev = p1;
    d->ctx_cap = (uint8_t)cap;
    memcpy(d->recent, recent, (size_t)n_recent * sizeof *recent);
    d->n_recent = (uint8_t)n_recent;
    memcpy(d->line, line, (size_t)n_line * sizeof *line);
    d->n_line = (uint8_t)n_line;
    #undef LINE_PUSH
}

static int recent_bonus(const op_decoder *d, const op_model *m, uint16_t id) {
    for (int i = 0; i < d->n_recent; i++)
        if (d->recent[i] == id) return id > m->bos || AI16(m->uni, id) < RECENT_MAX_UNI * m->scale ? RECENT_BONUS * m->scale : 0;
    return 0;
}

/* ---- neural word model (see decoder.js buildNN / nnStep) ---- */

int op_nn_open(op_nn *nn, const void *data, size_t size, const op_model *m) {
#ifdef OP_NO_NN
    (void)nn; (void)data; (void)size; (void)m;
    return OP_ERR_ARG;
#endif
    const uint8_t *b = (const uint8_t *)data;
    if (!nn || !b || ((uintptr_t)b & 3) || size < 32 || memcmp(b, "KPN1", 4) != 0 || rd16(b + 4) != 1) return OP_ERR_MODEL;
    nn->E = rd16(b + 6); nn->H = rd16(b + 8); nn->V = rd16(b + 10);
    nn->bos = rd16(b + 12); nn->unk = rd16(b + 14); nn->weight = (int16_t)rd16(b + 16);
    nn->model_bos = rd16(b + 26);
    nn->punct_weight = (int16_t)rd16(b + 28);
    if ((m && nn->model_bos != m->bos) || nn->E > OP_NN_MAX || nn->H > OP_NN_MAX) return OP_ERR_MODEL;
    size_t off = 32, E = nn->E, H = nn->H, V = nn->V;
    #define TAKE(dst, T, n) do { off = (off + 3) & ~(size_t)3; if (off + (n) * sizeof(T) > size) return OP_ERR_MODEL; \
                                 dst = (const T *)(b + off); off += (n) * sizeof(T); } while (0)
    TAKE(nn->sig, int16_t, 4096);
    TAKE(nn->emb, int8_t, V * E);
    TAKE(nn->es, uint8_t, V);
    TAKE(nn->wih, int8_t, 3 * H * E);
    TAKE(nn->is, uint8_t, 3 * H);
    TAKE(nn->whh, int8_t, 3 * H * H);
    TAKE(nn->hs, uint8_t, 3 * H);
    TAKE(nn->bih, int32_t, 3 * H);
    TAKE(nn->bhh, int32_t, 3 * H);
    TAKE(nn->proj, int8_t, E * H);
    TAKE(nn->ps, uint8_t, E);
    TAKE(nn->bproj, int32_t, E);
    TAKE(nn->bias, int16_t, V);
    #undef TAKE
    return OP_OK;
}

#define NN_CLAMP 524287         /* gate inputs are clamped so r * gate stays inside int32 */
static int32_t nn_sig(const op_nn *nn, int32_t v) {
    return nn->sig[((v < -32768 ? -32768 : v > 32767 ? 32767 : v) + 32768) >> 4];
}
static int32_t clamp_gate(int32_t v) { return v < -NN_CLAMP ? -NN_CLAMP : v > NN_CLAMP ? NN_CLAMP : v; }

/* out = the state after token tok from state h (NULL: the zero state); p = its projection. */
static void nn_step(const op_nn *nn, int32_t *g, const int16_t *h, uint16_t tok, int16_t *out, int16_t *p) {
    int E = nn->E, H = nn->H, et = nn->es[tok];
    const int8_t *x = nn->emb + (size_t)tok * E;
    for (int k = 0; k < 3 * H; k++) {
        int32_t a = 0, c = 0;
        const int8_t *wi = nn->wih + (size_t)k * E, *wh = nn->whh + (size_t)k * H;
        for (int j = 0; j < E; j++) a += wi[j] * x[j];
        if (h) for (int j = 0; j < H; j++) c += wh[j] * h[j];
        int sh = nn->is[k] + et;    /* a is scaled by 2^sh; to Q12 */
        g[k] = clamp_gate((sh >= 12 ? a >> (sh - 12) : a * (1 << (12 - sh))) + nn->bih[k]);
        g[3 * H + k] = clamp_gate((c >> nn->hs[k]) + nn->bhh[k]);
    }
    for (int i = 0; i < H; i++) {
        int32_t r = nn_sig(nn, g[i] + g[3 * H + i]);
        int32_t z = nn_sig(nn, g[H + i] + g[4 * H + i]);
        int32_t n = 2 * nn_sig(nn, 2 * (g[2 * H + i] + ((r * g[5 * H + i]) >> 12))) - 4096;
        out[i] = (int16_t)(((4096 - z) * n + z * (h ? h[i] : 0)) >> 12);
    }
    for (int e = 0; e < E; e++) {
        int32_t a = 0;
        const int8_t *w = nn->proj + (size_t)e * H;
        for (int j = 0; j < H; j++) a += w[j] * out[j];
        int32_t v = (a >> nn->ps[e]) + nn->bproj[e];
        p[e] = (int16_t)(v < -32767 ? -32767 : v > 32767 ? 32767 : v);
    }
}
/* log P(tok | state) in model units, from the state's projection p. */
static int32_t nn_score(const op_nn *nn, const int16_t *p, uint16_t tok) {
    int32_t a = 0;
    const int8_t *e = nn->emb + (size_t)tok * nn->E;
    for (int i = 0; i < nn->E; i++) a += p[i] * e[i];
    int32_t v = (a >> (nn->es[tok] + 4)) + nn->bias[tok];
    return v < -32767 ? -32767 : v > 32767 ? 32767 : v;
}

/* ---- beam search ---- */

/* Does a come before b: a higher score, ties in generation order (decoder.js's stable sort)? */
static int entry_before(const op_entry *a, const op_entry *b) {
    return a->score != b->score ? a->score > b->score : a->seq < b->seq;
}
/* decoder.js sorts every extension of the beam, then keeps the best one per state (previous
 * token + opener states) until it has `beam`: the `beam` best states by their best entry. Kept
 * here as they are made, in k[0..*nk) best first, so only `beam` entries are stored. An entry
 * pushed out never comes back: the last kept only gets better. */
static void keep_entry(op_entry *k, int *nk, int beam, const op_entry *e) {
    int i = 0;
    while (i < *nk && !(k[i].prev == e->prev && k[i].tri == e->tri && k[i].first == e->first && k[i].cf == e->cf)) i++;
    if (i < *nk) {                  /* the same state: the better of the two */
        if (!entry_before(e, &k[i])) return;
        memmove(k + i, k + i + 1, (size_t)(*nk - i - 1) * sizeof *k);
        (*nk)--;
    } else if (*nk == beam) {
        if (!entry_before(e, &k[beam - 1])) return;
        (*nk)--;
    }
    int j = *nk;
    for (; j > 0 && entry_before(e, &k[j - 1]); j--) k[j] = k[j - 1];
    k[j] = *e;
    (*nk)++;
}


/* ---- resuming after a finished sentence ---- */

static uint32_t user_stamps;    /* op_user stamps: every change gets a new one */

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ULL;
    return h;
}
/* What the search up to slot `slot` depends on besides the keys and QWERTY letters. */
static uint64_t resume_fp(const op_model *m, const op_start *start, const op_lock *locks, int n_locks,
                          int slot, int beam, const op_user *u) {
    uint64_t h = 0xcbf29ce484222325ULL;
    const void *ptrs[4] = { m, u, start ? (const void *)start->nn : NULL, start ? (const void *)start->nn_work : NULL };
    h = fnv(h, ptrs, sizeof ptrs);
    uint32_t v[4] = { u ? u->stamp : 0, (uint32_t)beam, start ? start->prev : 0xFFFFFFFFu, start ? start->cap : 2u };
    h = fnv(h, v, sizeof v);
    if (start && start->before) h = fnv(h, start->before, strlen(start->before) + 1);
    if (start && start->layout) h = fnv(h, start->layout, strlen(start->layout) + 1);
    for (int l = 0; l < n_locks; l++) if (locks[l].slot <= slot) h = fnv(h, &locks[l], sizeof locks[l]);
    return h;
}

int op_decode(op_decoder *d, const op_model *m, const char *keys, int beam,
              const op_start *start, const op_lock *locks, int n_locks) {
    if (!d || !m || !keys || beam < 1 || beam > OP_MAX_BEAM) return OP_ERR_ARG;
    int r = tokenize(d, keys, start ? start->hint : NULL);
    if (r) return r;
    d->layout = start && start->layout ? start->layout : DEFAULT_LAYOUT;
    d->user = start && start->user && start->user->bos == m->bos ? start->user : NULL;
    if (d->user) join_learned(d);
    if ((r = split_elisions(d, m)) != OP_OK) return r;
    d->n_slots = 0;
    for (int i = 0; i < d->n_toks; i++) {
        if (d->toks[i].type == TOK_SPACE) continue;
        if (d->n_slots == OP_MAX_SLOTS) return OP_ERR_TOO_LONG;
        d->slot_tok[d->n_slots++] = (uint16_t)i;
    }
    int last_is_word = d->n_toks && d->toks[d->n_toks - 1].type == TOK_WORD;
    const int16_t *P = m->param;

    int32_t score[OP_MAX_BEAM];
    uint16_t prev[OP_MAX_BEAM], tri[OP_MAX_BEAM];
    uint8_t first[OP_MAX_BEAM], cf[OP_MAX_BEAM];
    int nb = 1;
    uint16_t prev2;
    context(d, m, start, &prev2, &first[0], &cf[0]);
    score[0] = 0;
    prev[0] = d->ctx_prev;
    tri[0] = tri_find(m, prev2, prev[0]);
#ifdef OP_NO_NN
    const op_nn *nn = NULL;     /* built without the network: its code drops out */
#else
    const op_nn *nn = start && start->nn && start->nn_work && start->nn->model_bos == m->bos ? start->nn : NULL;
#endif
    op_nn_work *w = nn ? start->nn_work : NULL;
    int cur = 0;                /* which half of w holds the beam's states */
    #define NN_TOK(id) ((id) <= m->bos - 1 ? (id) : nn->unk)
    if (nn) {                   /* BOS, then the current line of the text before */
        nn_step(nn, w->g, NULL, nn->bos, w->h[0][0], w->p[0][0]);
        for (int i = 0; i < d->n_line; i++) {
            nn_step(nn, w->g, w->h[cur][0], NN_TOK(d->line[i]), w->h[!cur][0], w->p[!cur][0]);
            cur = !cur;
        }
    }

    /* Resume after the last finished sentence of the previous call, if nothing up to it changed. */
    int j0 = 0;
    const char *hint = start ? start->hint : NULL;
    int kn = (int)strlen(keys), hn = hint ? (int)strlen(hint) : 0;
    if (d->rs.ok && kn > d->rs.n_keys && d->rs.slot + 1 < d->n_slots && memcmp(keys, d->rs.keys, d->rs.n_keys) == 0 &&
        (!nn || w->rs_owner == d) &&
        d->rs.fp == resume_fp(m, start, locks, n_locks, d->rs.slot, beam, d->user)) {
        int same = 1;
        for (int i = 0; i < d->rs.n_keys && same; i++) same = (i < hn ? hint[i] : 0) == d->rs.hint[i];
        if (same) {
            nb = d->rs.nb;
            for (int k = 0; k < nb; k++) {
                score[k] = d->rs.score[k]; prev[k] = d->rs.prev[k]; tri[k] = d->rs.tri[k];
                first[k] = d->rs.first[k]; cf[k] = d->rs.cf[k];
                if (nn) { memcpy(w->h[0][k], w->rh[k], sizeof w->rh[k]); memcpy(w->p[0][k], w->rp[k], sizeof w->rp[k]); }
            }
            cur = 0;
            j0 = d->rs.slot + 1;
        }
    }
    if (!j0) d->rs.ok = 0;      /* bp[] is about to be rewritten from the start */

    cand c[OP_MAX_CANDS];
    for (int j = j0; j < d->n_slots; j++) {
        const op_tok *t = &d->toks[d->slot_tok[j]];
        int is_final = j == d->n_slots - 1;
        int nc = slot_cands(d, m, t, is_final && last_is_word, c);
        char tk = t->type == TOK_NUM ? time_kind(d->keys + t->off, t->len) : 0;
        for (int l = 0; l < n_locks; l++) {
            if (locks[l].slot != j) continue;
            int hit = 0;
            for (int i = 0; i < nc; i++) if (c[i].id == locks[l].id) c[hit++] = c[i];
            if (hit) nc = hit;
        }
        /* Each hypothesis extended by each candidate, in order (seq), and the best kept. Finished
         * sentences are frozen (decoder.js): at a punctuation slot with more typed after it, a
         * first pass finds what decoding showed when the keys stopped here; if that ended the
         * sentence, only the entries from its parent are kept, so the words before it can't
         * change. */
        int nk = 0, from = -1;
        for (int pass = t->type == TOK_PUNCT && !is_final ? 0 : 1; pass < 2; pass++) {
            int seq = 0, top_parent = -1;
            uint16_t top_id = OP_NONE;
            int32_t best = 0;
            for (int h = 0; h < nb; h++) {
                for (int ci = 0; ci < nc; ci++) {
                    uint16_t id = c[ci].id, p;
                    uint8_t f = first[h], g = cf[h];
                    int32_t step;
                    if (id == OP_TIME_ID) {
                        int before = 0;
                        for (int i = 0; i < OP_TIME_BEFORE; i++) if (prev[h] != OP_NONE && prev[h] == m->time_before[i]) before = 1;
                        step = m->scale * ((before ? TIME_PREV : -TIME_ALONE) - (tk == '.' ? TIME_LITERAL : 0));
                        p = AFTER_NUMBER;
                    } else if (c[ci].kind == OP_COMPOUND) {
                        step = P[OP_P_UNKNOWN];
                        p = OP_NONE;
                    } else if (id == OP_NONE) {
                        step = c[ci].kind == OP_NUMBER ? 0 : c[ci].kind == OP_LITERAL ? P[OP_P_LITERAL] : P[OP_P_UNKNOWN];
                        p = c[ci].kind == OP_NUMBER ? (tk ? AFTER_DECIMAL : AFTER_NUMBER) : OP_NONE;
                    } else {
                        step = token_score(m, d->user, prev[h], id, tri[h]) + c[ci].pen + recent_bonus(d, m, id);
                        p = id;
                    }
                    if (nn)
                        step += ((id >= m->n_words && id < m->bos ? nn->punct_weight : nn->weight) * nn_score(nn, w->p[cur][h], NN_TOK(id))) >> 8;
                    if (id != OP_NONE && id >= m->n_words && id < m->bos) {
                        /* Punctuation: a word-pair model can't see how the sentence began,
                         * so use the sentence's first word to judge question vs statement. */
                        int q = f == 1 || g == 1;
                        char ch = ACH(m->punct, id - m->n_words);
                        if (ch == '?') step += q ? P[OP_P_QQ] : P[OP_P_QN];
                        else if (ch == '.') step += P[OP_P_DB] + (q ? P[OP_P_DQ] : 0);
                        else if (ch == ',') step += P[OP_P_CB] + (is_final ? P[OP_P_CF] : 0);
                        else if (ch == '!') step += P[OP_P_EB];
                        if (ch != ',') { p = m->bos; f = 0; }
                        g = 0;
                    } else {
                        int skip = id != OP_NONE && in_u16(m->q_skip, m->n_skip, id);
                        if (!f && !skip) f = q_state(m, id);
                        if (!g && !skip) g = q_state(m, id);
                    }
                    op_entry e;
                    e.score = score[h] + step;
                    e.seq = (uint16_t)seq++;
                    e.prev = p;
                    e.id = id;
                    e.parent = (uint8_t)h;
                    e.kind = c[ci].kind;
                    e.first = f;
                    e.cf = g;
                    e.tri = p == m->bos ? OP_NONE : tri_find(m, prev[h], p);
                    if (pass == 0) {
                        int comma = e.id != OP_NONE && ACH(m->punct, e.id - m->n_words) == ',';
                        int32_t sc = e.score + (comma ? P[OP_P_CF] : 0);
                        if (top_parent < 0 || sc > best) { top_parent = h; top_id = e.id; best = sc; }
                    } else if (from < 0 || h == from) {
                        keep_entry(d->ent, &nk, beam, &e);     /* with the network too: the best keeps its own state */
                    }
                }
            }
            char tc = pass == 0 && top_id != OP_NONE ? ACH(m->punct, top_id - m->n_words) : 0;
            if (tc == '.' || tc == '!' || tc == '?') from = top_parent;
        }
        for (int k = 0; k < nk; k++) {
            const op_entry *e = &d->ent[k];
            score[k] = e->score;
            prev[k] = e->prev;
            first[k] = e->first;
            cf[k] = e->cf;
            tri[k] = e->tri;
            if (nn) nn_step(nn, w->g, w->h[cur][e->parent], NN_TOK(e->id), w->h[!cur][k], w->p[!cur][k]);
            d->bp[j][k].id = e->id;
            d->bp[j][k].parent = e->parent;
            d->bp[j][k].kind = e->kind;
        }
        nb = nk;
        cur ^= nn != NULL;
        /* A finished sentence (a mark, a space, then more): remember the search here. */
        int sp = d->slot_tok[j] + 1;
        if (t->type == TOK_PUNCT && !is_final && sp < d->n_toks && d->toks[sp].type == TOK_SPACE) {
            d->rs.ok = 1; d->rs.nb = (uint8_t)nb; d->rs.slot = (uint16_t)j;
            d->rs.n_keys = (uint16_t)(d->toks[sp].off + 1);
            d->rs.fp = resume_fp(m, start, locks, n_locks, j, beam, d->user);
            memcpy(d->rs.keys, keys, d->rs.n_keys);
            for (int i = 0; i < d->rs.n_keys; i++) d->rs.hint[i] = i < hn ? hint[i] : 0;
            if (nn) w->rs_owner = d;
            for (int k = 0; k < nb; k++) {
                d->rs.score[k] = score[k]; d->rs.prev[k] = prev[k]; d->rs.tri[k] = tri[k];
                d->rs.first[k] = first[k]; d->rs.cf[k] = cf[k];
                if (nn) { memcpy(w->rh[k], w->h[cur][k], sizeof w->rh[k]); memcpy(w->rp[k], w->p[cur][k], sizeof w->rp[k]); }
            }
        }
    }
    #undef NN_TOK
    d->score = score[0];
    for (int j = d->n_slots - 1, h = 0; j >= 0; j--) {
        d->choice[j].id = d->bp[j][h].id;
        d->choice[j].kind = d->bp[j][h].kind;
        h = d->bp[j][h].parent;
    }
    return OP_OK;
}

/* The candidates op_decode weighed for slot `slot` of its last call: dictionary, learned and
 * punctuation ids (numbers, letters typed exactly and words run together left out), in the
 * order it took them (most frequent first). Lock one to show another guess. Returns how many. */
int op_candidates(const op_decoder *d, const op_model *m, int slot, uint16_t *ids, int max) {
    if (!d || !m || !ids || slot < 0 || slot >= d->n_slots) return OP_ERR_ARG;
    int last_word = slot == d->n_slots - 1 && d->toks[d->n_toks - 1].type == TOK_WORD;
    cand c[OP_MAX_CANDS];
    int nc = slot_cands(d, m, &d->toks[d->slot_tok[slot]], last_word, c), n = 0;
    for (int i = 0; i < nc && n < max; i++)
        if (c[i].id != OP_NONE && c[i].id != OP_TIME_ID && c[i].kind != OP_COMPOUND) ids[n++] = c[i].id;
    return n;
}

/* ---- rendering ---- */

/* Text of id in a casing table (display or alt), or -1. */
static int cased(OP_REF(uint16_t) ids, uint32_t n, OP_REF(uint32_t) off, OP_REF(char) text, uint16_t id,
                 const char **out, char *buf) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        uint16_t x = AU16(ids, mid);
        if (x == id) {
            uint32_t a = AU32(off, mid), len = AU32(off, mid + 1) - a;
#ifdef OP_EXT_MODEL
            if (len > OP_WORD_BUF) len = OP_WORD_BUF;
            op_ext_read(text + a, buf, len);
            *out = buf;
#else
            (void)buf;
            *out = text + a;
#endif
            return (int)len;
        }
        if (x < id) lo = mid + 1; else hi = mid;
    }
    return -1;
}
/* A word's casing between prev and next: its other casing if (prev, id) is a capKey pair
 * ("in May") or (id, next) a capNextKey pair ("Will Smith"), unless (id, next) is a capKeepKey
 * pair ("like apple pie"); else its usual one (iPhone, Friday), else -1 for plain lowercase.
 * See decoder.js capFlip. */
static int display(const op_model *m, uint16_t prev, uint16_t id, uint16_t next, const char **text, char *buf) {
    int keep = next < m->n_words && find_u32(m->cap_keep, m->n_cap_keep, (uint32_t)id << 16 | next) >= 0;
    if (id < m->n_words && !keep && ((prev <= m->bos && find_u32(m->cap_key, m->n_cap, (uint32_t)prev << 16 | id) >= 0) ||
        (next < m->n_words && find_u32(m->cap_next, m->n_cap_next, (uint32_t)id << 16 | next) >= 0)))
        return cased(m->alt_id, m->n_alt, m->alt_off, m->alt_text, id, text, buf);
    return cased(m->disp_id, m->n_disp, m->disp_off, m->disp_text, id, text, buf);
}

/* Call signs are shown in capitals: 1-2 letters, a digit, 1-4 letters (ke7aqa), or digit,
 * letter, digit, 1-3 letters (2e0abc). See decoder.js CALL_SIGN. */
static int call_sign(const char *w, int n) {
    int i = 0, a = 0, b = 0;
    #define LOW(c) (((c) | 32) >= 'a' && ((c) | 32) <= 'z')
    #define DIG(c) ((c) >= '0' && (c) <= '9')
    if (n >= 3 && DIG(w[0]) && LOW(w[1]) && DIG(w[2])) {
        for (i = 3; i < n && LOW(w[i]); i++) b++;
        return i == n && b >= 1 && b <= 3;
    }
    for (; i < n && LOW(w[i]); i++) a++;
    if (a < 1 || a > 2 || i >= n || !DIG(w[i])) return 0;
    for (i++; i < n && LOW(w[i]); i++) b++;
    return i == n && b >= 1 && b <= 4;
    #undef LOW
    #undef DIG
}

int op_render(const op_decoder *d, const op_model *m, const op_start *start, char *out, int cap) {
    int n = 0, cap_next = d->ctx_cap, mark_next = 0;     /* mark_next: the inverted mark waits past "Hola," */
    uint16_t prev = d->ctx_prev;                      /* as op_decode's context */
    (void)start;
    for (int i = 0, j = 0; i < d->n_toks; i++) {
        const op_tok *t = &d->toks[i];
        if (t->type == TOK_SPACE) {
            if (n + 1 >= cap) return OP_ERR_SPACE;
            out[n++] = ' ';
            continue;
        }
        op_choice ch = d->choice[j++];
        if (t->type == TOK_PUNCT) {
            if (n + 1 >= cap) return OP_ERR_SPACE;
            out[n++] = ch.kind == OP_LITERAL ? t->lit : ACH(m->punct, ch.id - m->n_words);
            cap_next = out[n - 1] == '.' || out[n - 1] == '!' || out[n - 1] == '?';
            if (cap_next) mark_next = 0;
            prev = ch.kind == OP_LITERAL ? OP_NONE : out[n - 1] == ',' ? ch.id : m->bos;
            if (out[n - 1] == '?' && (m->param[OP_P_FLAGS] & FLAG_GREEK_Q)) out[n - 1] = ';';
            if ((m->param[OP_P_FLAGS] & FLAG_FR_SPACE) && (out[n - 1] == '?' || out[n - 1] == '!' || out[n - 1] == ':')) {
                if (n + 3 >= cap) return OP_ERR_SPACE;
                out[n + 2] = out[n - 1];
                out[n - 1] = (char)0xE2; out[n] = (char)0x80; out[n + 1] = (char)0xAF;
                n += 3;
            }
            if ((m->param[OP_P_FLAGS] & FLAG_ARABIC) && (out[n - 1] == '?' || out[n - 1] == ',')) {
                if (n + 2 >= cap) return OP_ERR_SPACE;
                out[n] = (char)(out[n - 1] == '?' ? 0x9F : 0x8C);
                out[n - 1] = (char)0xD8;
                n++;
            }
            continue;
        }
        if (t->type == TOK_NUM) {
            int len = format_number(d->keys + t->off, t->len, ch.kind == OP_TIME, out + n, cap - n);
            if (len < 0) return len;
            n += len;
            cap_next = 0;
            prev = OP_NONE;
            continue;
        }
        if (t->lit == '-') {                        /* the second word of a hyphenated pair */
            if (n + 1 >= cap) return OP_ERR_SPACE;
            out[n++] = '-';
        }
        int s = n, len, call;
        if (ch.kind == OP_WORD || ch.kind == OP_PARTIAL) {
            const char *w, *shown, *src;
            /* the next word, if a whole word comes right after (spaces only between) */
            uint16_t next = OP_NONE;
            int k = i + 1;
            while (k < d->n_toks && d->toks[k].type == TOK_SPACE) k++;
            if (k < d->n_toks && d->toks[k].type == TOK_WORD && d->choice[j].kind == OP_WORD) next = d->choice[j].id;
            char wb[OP_WORD_BUF], sb[OP_WORD_BUF];
            int wn = tok_text(m, d->user, ch.id, &w, wb), dn = display(m, prev, ch.id, next, &shown, sb);
            call = dn >= 0 ? call_sign(shown, dn) : call_sign(w, wn);
            int chars = u8_len(w, wn);
            if (ch.kind == OP_PARTIAL && chars > t->len) chars = t->len;   /* only what's typed so far */
            if (dn >= 0) {
                len = ch.kind == OP_WORD ? dn : u8_prefix(shown, dn, chars);
                src = shown;
            } else {
                len = u8_prefix(w, wn, chars);
                src = w;
            }
            if (n + len >= cap) return OP_ERR_SPACE;
            memcpy(out + n, src, (size_t)len);
            n += len;
        } else if (ch.kind == OP_COMPOUND) {
            compound cs[COMPOUND_MAX];
            int nc = compounds(m, d->keys + t->off, d->lits + t->off, t->len, cs), r = ch.id - OP_COMPOUND_ID;
            if (r >= nc) return OP_ERR_ARG;
            if (n + cs[r].len >= cap) return OP_ERR_SPACE;
            memcpy(out + n, cs[r].text, cs[r].len);
            n += cs[r].len;
            call = call_sign(out + s, n - s);
        } else {
            /* Typed exactly, or no dictionary word fits: the exact letters, else the QWERTY
             * letters tapped, else each key's first letter. */
            if (n + 2 * t->len >= cap) return OP_ERR_SPACE;
            for (int k = 0; k < t->len; k++) {
                uint16_t l = d->lits[t->off + k];
                char key = d->keys[t->off + k];
                if (!l) l = d->hints[t->off + k];
                if (!l) l = (uint16_t)(key == '#' ? '\'' : (key >= '2' && key <= '9') ? FIRST_LETTER[key - '2'] : '?');
                if (l < 0x80) out[n++] = (char)l;
                else { out[n++] = (char)(0xC0 | l >> 6); out[n++] = (char)(0x80 | (l & 0x3F)); }   /* UTF-8 */
            }
            call = call_sign(out + s, n - s);
        }
        len = n - s;
        if (call) for (int k = s; k < n; k++) if (out[k] >= 'a' && out[k] <= 'z') out[k] = (char)(out[k] - 'a' + 'A');
        if ((m->param[OP_P_FLAGS] & FLAG_I) && len && out[s] == 'i' && (len == 1 || out[s + 1] == '\'')) out[s] = 'I';
        /* Capitalize a sentence start, but keep brand styling like iPhone / eBay. */
        int brand = len >= 2 && out[s] >= 'a' && out[s] <= 'z' && out[s + 1] >= 'A' && out[s + 1] <= 'Z';
        if (cap_next && !brand && len) {
            unsigned char c0 = (unsigned char)out[s];
            if (c0 >= 'a' && c0 <= 'z') out[s] = (char)(c0 - 32);
            else if (c0 >= 0xC2 && c0 < 0xE0 && len >= 2) {        /* é -> É, ж -> Ж, α -> Α */
                int at = 0, cp = next_cp(out + s, len, &at), t, k = ct_find(cp, &t);
                if (k >= 0 && CHARTABLE[t].delta[k]) {
                    cp += CHARTABLE[t].delta[k];
                    out[s] = (char)(0xC0 | cp >> 6);
                    out[s + 1] = (char)(0x80 | (cp & 0x3F));
                }
            }
        }
        if ((cap_next || mark_next) && (m->param[OP_P_FLAGS] & FLAG_INVERTED)) {
            /* Spanish opens a question / exclamation with ¿ / ¡: the mark ending this sentence.
             * "Hola, ¿qué tal?": a greeting or filler followed by a comma passes it on. */
            int k = i + 1, comma = 0;
            while (k < d->n_toks && d->toks[k].type == TOK_SPACE) k++;
            if (k < d->n_toks && d->toks[k].type == TOK_PUNCT)
                comma = d->choice[j].kind != OP_LITERAL && ACH(m->punct, d->choice[j].id - m->n_words) == ',';
            mark_next = comma && ch.id != OP_NONE && in_u16(m->q_skip, m->n_skip, ch.id);
            char end = 0;
            if (mark_next) end = 1;             /* not here */
            for (int k = i + 1, jj = j; k < d->n_toks && !end; k++) {
                const op_tok *tk = &d->toks[k];
                if (tk->type == TOK_SPACE) continue;
                op_choice c2 = d->choice[jj++];
                if (tk->type != TOK_PUNCT) continue;
                char p = c2.kind == OP_LITERAL ? tk->lit : ACH(m->punct, c2.id - m->n_words);
                if (p == '.' || p == '!' || p == '?') end = p;
            }
            if (end == '?' || end == '!') {
                if (n + 2 >= cap) return OP_ERR_SPACE;
                memmove(out + s + 2, out + s, (size_t)(n - s));
                out[s] = (char)0xC2;
                out[s + 1] = (char)(end == '?' ? 0xBF : 0xA1);
                n += 2;
            }
        }
        cap_next = 0;
        prev = ch.kind == OP_WORD && ch.id < m->n_words ? ch.id : OP_NONE;
    }
    if (n >= cap) return OP_ERR_SPACE;
    out[n] = 0;
    return n;
}

/* ---- learning ---- */

void op_user_init(op_user *u, const op_model *m) {
    memset(u, 0, sizeof *u);
    u->bos = m->bos;
    u->stamp = ++user_stamps;
}

static void user_bump(op_user *u, uint16_t prev, uint16_t cur) {
    int i = user_find(u, prev, cur);
    if (i >= 0) {
        if (u->pair[i].count < 255) u->pair[i].count++;
        return;
    }
    /* Full: halve every count and drop the ones that reach zero, until there is room. */
    while (u->n_pairs >= OP_USER_PAIRS) {
        int k = 0;
        for (int j = 0; j < u->n_pairs; j++) {
            u->pair[j].count >>= 1;
            if (u->pair[j].count) u->pair[k++] = u->pair[j];
        }
        u->n_pairs = (uint16_t)k;
    }
    i = -user_find(u, prev, cur) - 1;
    memmove(&u->pair[i + 1], &u->pair[i], (size_t)(u->n_pairs - i) * sizeof *u->pair);
    u->pair[i].prev = prev;
    u->pair[i].cur = cur;
    u->pair[i].count = 1;
    u->n_pairs++;
}

/* Id of learned word w[0..wn), adding it (replacing the least used one when full). */
static uint16_t user_word(op_user *u, const op_model *m, const char *w, int wn) {
    int k = -1;
    for (int j = 0; j < u->n_words && k < 0; j++) {     /* ignoring case: it keeps the casing it has */
        const char *t = u->word[j];
        int i = 0;
        while (i < wn && t[i] && (t[i] >= 'A' && t[i] <= 'Z' ? t[i] + 32 : t[i]) == w[i]) i++;
        if (i == wn && !t[i]) k = j;
    }
    if (k >= 0) return (uint16_t)(m->bos + 1 + k);
    if (k < 0 && u->n_words < OP_USER_WORDS) {
        k = u->n_words++;
    } else if (k < 0) {
        k = 0;
        for (int j = 1; j < u->n_words; j++)
            if (user_count(u, USER_ANY, (uint16_t)(m->bos + 1 + j)) < user_count(u, USER_ANY, (uint16_t)(m->bos + 1 + k))) k = j;
        uint16_t id = (uint16_t)(m->bos + 1 + k);
        int kept = 0;
        for (int j = 0; j < u->n_pairs; j++)
            if (u->pair[j].prev != id && u->pair[j].cur != id) u->pair[kept++] = u->pair[j];
        u->n_pairs = (uint16_t)kept;
    }
    memcpy(u->word[k], w, (size_t)wn);
    u->word[k][wn] = 0;
    return (uint16_t)(m->bos + 1 + k);
}

/* Saved user, all little-endian: "KPU1", u16 version (1), u16 bos, u16 nWords, u16 nPairs, the
 * words each NUL-terminated, then each pair as u16 prev, u16 cur, u8 count, sorted by
 * (prev, cur). Same bytes as decoder.js saveUser. */
int op_user_save(const op_user *u, uint8_t *out, int cap) {
    int n = 12;
    for (int k = 0; k < u->n_words; k++) n += (int)strlen(u->word[k]) + 1;
    n += 5 * u->n_pairs;
    if (!out) return n;
    if (n > cap) return OP_ERR_SPACE;
    uint16_t head[4] = { 1, u->bos, u->n_words, u->n_pairs };
    memcpy(out, "KPU1", 4);
    for (int i = 0; i < 4; i++) { out[4 + 2 * i] = (uint8_t)head[i]; out[5 + 2 * i] = (uint8_t)(head[i] >> 8); }
    int o = 12;
    for (int k = 0; k < u->n_words; k++) {
        size_t l = strlen(u->word[k]) + 1;
        memcpy(out + o, u->word[k], l);
        o += (int)l;
    }
    for (int i = 0; i < u->n_pairs; i++, o += 5) {
        const op_user_pair *p = &u->pair[i];
        out[o] = (uint8_t)p->prev; out[o + 1] = (uint8_t)(p->prev >> 8);
        out[o + 2] = (uint8_t)p->cur; out[o + 3] = (uint8_t)(p->cur >> 8);
        out[o + 4] = p->count;
    }
    return n;
}

int op_user_load(op_user *u, const op_model *m, const uint8_t *b, size_t size) {
    if (!u || !m) return OP_ERR_ARG;
    op_user_init(u, m);
    if (!b || size < 12 || memcmp(b, "KPU1", 4) != 0 || rd16(b + 4) != 1 || rd16(b + 6) != m->bos) return OP_ERR_MODEL;
    int nw = rd16(b + 8), np = rd16(b + 10);
    size_t o = 12;
    for (int k = 0; k < nw; k++) {
        const uint8_t *e = o < size ? memchr(b + o, 0, size - o) : NULL;
        if (!e || e - (b + o) > OP_USER_WORD_LEN) { op_user_init(u, m); return OP_ERR_MODEL; }
        if (k < OP_USER_WORDS) memcpy(u->word[k], b + o, (size_t)(e - (b + o)) + 1);
        o = (size_t)(e - b) + 1;
    }
    u->n_words = (uint16_t)(nw < OP_USER_WORDS ? nw : OP_USER_WORDS);
    if (size - o < 5u * (size_t)np) { op_user_init(u, m); return OP_ERR_MODEL; }
    uint16_t last = (uint16_t)(m->bos + u->n_words);      /* highest id kept */
    int kept = 0;
    for (int i = 0; i < np; i++, o += 5) {
        uint16_t prev = rd16(b + o), cur = rd16(b + o + 2);
        if ((prev != USER_ANY && prev != AFTER_NUMBER && prev > last) || cur > last || !b[o + 4]) continue;
        if (kept == OP_USER_PAIRS) {
            /* Full: halve every count and drop zeros, as op_learn does when it runs out of room. */
            int k = 0;
            for (int j = 0; j < kept; j++) {
                u->pair[j].count >>= 1;
                if (u->pair[j].count) u->pair[k++] = u->pair[j];
            }
            kept = k;
            if (kept == OP_USER_PAIRS) continue;
        }
        u->pair[kept].prev = prev;
        u->pair[kept].cur = cur;
        u->pair[kept].count = b[o + 4];
        kept++;
    }
    u->n_pairs = (uint16_t)kept;
    return OP_OK;
}

/* Mirrors decoder.js learn. */
int op_learn(op_user *u, const op_decoder *d, const op_model *m, const op_start *start) {
    if (!u || !d || !m || u->bos != m->bos) return OP_ERR_ARG;
    u->stamp = ++user_stamps;
    uint16_t prev = d->ctx_prev;
    (void)start;
    for (int j = 0; j < d->n_slots; j++) {
        op_choice ch = d->choice[j];
        const op_tok *t = &d->toks[d->slot_tok[j]];
        uint16_t id = ch.id;
        if (ch.kind == OP_NUMBER || ch.kind == OP_TIME) { prev = AFTER_NUMBER; continue; }
        if (ch.kind == OP_LITERAL && t->type != TOK_WORD) { prev = OP_NONE; continue; }   /* ':' */
        if (ch.kind == OP_LITERAL || (ch.kind == OP_UNKNOWN && t->type == TOK_WORD)) {
            /* Spelled exactly, or typed on QWERTY with no word matching: learn it. */
            char w[2 * OP_MAX_KEYS];
            int ok = 1, wn = 0;
            for (int k = 0; ok && k < t->len; k++) {
                uint16_t l = d->lits[t->off + k];
                if (!l) l = d->hints[t->off + k];
                if (!l && d->keys[t->off + k] == '#') l = 39;
                if ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9') || l == 39) w[wn++] = (char)l;
                else if (table_key(l)) { w[wn++] = (char)(0xC0 | l >> 6); w[wn++] = (char)(0x80 | (l & 0x3F)); }
                else ok = 0;
            }
            if (!ok || wn > OP_USER_WORD_LEN) { prev = OP_NONE; continue; }
            id = user_word(u, m, w, wn);
        } else if (ch.kind == OP_COMPOUND) {
            /* Known words run together: learn it as one word. */
            compound cs[COMPOUND_MAX];
            int nc = compounds(m, d->keys + t->off, d->lits + t->off, t->len, cs), r = ch.id - OP_COMPOUND_ID;
            int plain = 1;              /* learned words are plain letters (keys typed one per letter) */
            for (int k = 0; r < nc && k < cs[r].len; k++) if ((unsigned char)cs[r].text[k] >= 0x80) plain = 0;
            if (r >= nc || cs[r].len > OP_USER_WORD_LEN || !plain) { prev = OP_NONE; continue; }
            id = user_word(u, m, cs[r].text, cs[r].len);
        } else if (ch.kind == OP_PARTIAL || id == OP_NONE) {
            prev = OP_NONE;
            continue;
        }
        user_bump(u, USER_ANY, id);
        if (prev != OP_NONE && prev != AFTER_NUMBER) user_bump(u, prev, id);
        prev = id >= m->n_words && id < m->bos && ACH(m->punct, id - m->n_words) != ',' ? m->bos : id;
    }
    return OP_OK;
}
