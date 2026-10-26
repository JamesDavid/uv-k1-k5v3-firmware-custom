/* OnePress text entry for overlay apps: see onepress_svc.h. */
#include <string.h>

#include "app/onepress_svc.h"
#include "driver/py25q16.h"
#include "onepress/onepress.h"

_Static_assert(sizeof(app_op_lock_t) == sizeof(op_lock), "app_op_lock_t mirrors op_lock");
_Static_assert(APP_OP_KEYS_MAX <= OP_MAX_KEYS, "the decoder takes a whole message");
_Static_assert((int)APP_OP_WORD == (int)OP_WORD && (int)APP_OP_PARTIAL == (int)OP_PARTIAL &&
               (int)APP_OP_PUNCT == (int)OP_PUNCT && (int)APP_OP_LITERAL == (int)OP_LITERAL &&
               (int)APP_OP_UNKNOWN == (int)OP_UNKNOWN, "slot kinds");

#define OP_BEAM 3               /* as good as 8 on the radio model (tests/eval.js --beam) */
#define CACHE_LINES 8           /* model reads: 8 lines of 32 bytes halve the flash reads */
#define CACHE_LINE 32u

char gOpKeys[APP_OP_KEYS_MAX + 1];
char gOpText[APP_OP_TEXT_MAX + 1];

static op_model model;
static op_decoder dec;
static int8_t state;            /* 0 not opened yet, 1 open, APP_OP_NO_MODEL */
static uint32_t tag[CACHE_LINES];               /* line number + 1, 0 = empty */
static uint8_t line[CACHE_LINES][CACHE_LINE];

/* The decoder's model reads (OP_EXT_MODEL). Read-only: safe while an app runs
 * from the overlay, which only external-flash writes use. */
void op_ext_read(uint32_t addr, void *dst, uint32_t n)
{
    uint8_t *o = dst;
    while (n) {
        uint32_t l = addr / CACHE_LINE, k = l % CACHE_LINES, at = addr % CACHE_LINE, take = CACHE_LINE - at;
        if (take > n)
            take = n;
        if (tag[k] != l + 1) {
            PY25Q16_ReadBuffer(l * CACHE_LINE, line[k], CACHE_LINE);
            tag[k] = l + 1;
        }
        memcpy(o, line[k] + at, take);
        o += take;
        addr += take;
        n -= take;
    }
}

int8_t OP_Open(void)
{
    if (state == 0) {
        memset(tag, 0, sizeof tag);     /* a model uploaded since boot */
        state = op_model_open(&model, (const void *)(uintptr_t)APP_OP_MODEL_ADDR, APP_OP_MODEL_MAX) == OP_OK
              ? 1 : APP_OP_NO_MODEL;
    }
    return state == 1 ? 0 : state;
}

int16_t OP_Decode(const char *keys, const app_op_lock_t *locks, uint8_t n_locks)
{
    if (OP_Open())
        return APP_OP_NO_MODEL;
    int r = op_decode(&dec, &model, keys, OP_BEAM, NULL, (const op_lock *)locks, n_locks);
    return r < 0 ? (int16_t)r : (int16_t)dec.n_slots;
}

int16_t OP_Render(char *out, uint8_t cap)
{
    if (state != 1)
        return APP_OP_NO_MODEL;
    return (int16_t)op_render(&dec, &model, NULL, out, cap);
}

uint8_t OP_Guesses(const char *keys, const app_op_lock_t *locks, uint8_t n_locks,
                   uint8_t slot, uint16_t *ids, uint8_t max)
{
    enum { MAX_LOCKS = 8, MAX_GUESSES = 8 };
    int32_t score[MAX_GUESSES];
    op_lock l[MAX_LOCKS + 1];
    if (max > MAX_GUESSES)
        max = MAX_GUESSES;
    if (n_locks > MAX_LOCKS || OP_Decode(keys, locks, n_locks) <= slot)
        return 0;
    int n = op_candidates(&dec, &model, slot, ids, max), nl = 0;
    for (uint8_t i = 0; i < n_locks; i++)
        if (locks[i].slot != slot) {
            l[nl].slot = locks[i].slot;
            l[nl++].id = locks[i].id;
        }
    l[nl].slot = slot;
    for (int i = 0; i < n; i++) {           /* each scored by the whole decode */
        l[nl].id = ids[i];
        score[i] = op_decode(&dec, &model, keys, OP_BEAM, NULL, l, nl + 1) == OP_OK &&    /* whole words only */
                   dec.choice[slot].id == ids[i] && dec.choice[slot].kind != OP_PARTIAL ? dec.score : INT32_MIN;
        for (int j = i; j > 0 && score[j] > score[j - 1]; j--) {   /* best first, ties in order */
            int32_t s = score[j]; score[j] = score[j - 1]; score[j - 1] = s;
            uint16_t x = ids[j]; ids[j] = ids[j - 1]; ids[j - 1] = x;
        }
    }
    while (n > 0 && score[n - 1] == INT32_MIN)
        n--;
    OP_Decode(keys, locks, n_locks);
    return (uint8_t)(n < 0 ? 0 : n);
}

uint16_t OP_Choice(uint8_t slot, uint8_t *kind)
{
    if (state != 1 || slot >= dec.n_slots) {
        if (kind)
            *kind = APP_OP_UNKNOWN;
        return OP_NONE;
    }
    if (kind)
        *kind = dec.choice[slot].kind;
    return dec.choice[slot].id;
}

uint8_t OP_Word(uint16_t id, char *buf, uint8_t cap)
{
    if (state != 1 || !cap)
        return 0;
    uint8_t n = 0;
    if (id < model.n_words) {
        const char *w;
        int wn = op_word(&model, id, &w);
        while (n + 1 < cap && n < wn) {
            buf[n] = w[n];
            n++;
        }
    } else if (id < model.bos && cap > 1) {     /* a mark, from the model's table */
        op_ext_read(model.punct + (uint32_t)(id - model.n_words), buf, 1);
        n = 1;
    }
    buf[n] = 0;
    return n;
}
