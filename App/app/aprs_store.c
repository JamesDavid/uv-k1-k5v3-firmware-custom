/* The APRS inbox kept in external flash (APP_RX_INBOX_ADDR), see aprs_store.h.
 *
 * One sector of 128-byte records appended in order: a message is written as
 * soon as the inbox is asked for after it came in, and a read one has its flag
 * byte programmed to 0, which needs no erase. A full sector is erased and the
 * inbox written again (~0.1 s, every 32 messages). The raw page program and
 * erase leave the sector cache alone, which holds the running app. A record a
 * power off cut short fails its checksum and is skipped. Records written
 * before messages carried their frequency (magic 0xA5, 84-byte body) still
 * load, with no frequency.
 */
#include <stddef.h>
#include <string.h>

#include "app/aprs_store.h"
#include "apps/app_api.h"
#include "driver/py25q16.h"
#include "onepress/aprs_rx.h"

#define REC_SIZE    128u
#define RECS        (0x1000u / REC_SIZE)
#define REC_MAGIC   0xA6u
#define REC_BODY    sizeof(aprs_msg_t)          /* its unread byte unused: the flag byte says */
#define OLD_MAGIC   0xA5u
#define OLD_BODY    84u                         /* from, text, id */
typedef struct { uint8_t magic, unread, sum, body[REC_BODY]; } rec_t;

static bool loaded;
static uint8_t nextRec;                         /* the first free record */
static uint8_t recOf[APRS_RX_INBOX];            /* the record of each inbox message */
static uint8_t recUnread[APRS_RX_INBOX];        /* its flag as stored */

static uint8_t sum(const uint8_t *b, uint8_t n)
{
    uint8_t s = 0x5Au;
    for (uint8_t i = 0; i < n; i++) s = (uint8_t)(s + b[i]);
    return s;
}
static uint8_t bodyOf(const rec_t *r) { return r->magic == REC_MAGIC ? REC_BODY : r->magic == OLD_MAGIC ? OLD_BODY : 0; }

static void readRec(uint8_t i, rec_t *r) { PY25Q16_ReadBuffer(APP_RX_INBOX_ADDR + i * REC_SIZE, r, sizeof *r); }

static void writeRec(const aprs_msg_t *m, uint8_t slot)
{
    rec_t r;
    r.magic = REC_MAGIC;
    r.unread = m->unread ? 0xFFu : 0u;
    memcpy(r.body, m, REC_BODY);
    r.sum = sum(r.body, REC_BODY);
    PY25Q16_WriteBufferPhysical(APP_RX_INBOX_ADDR + nextRec * REC_SIZE, &r, sizeof r);
    recOf[slot] = nextRec++;
    recUnread[slot] = m->unread;
}

/* The last messages written, oldest first, back in the inbox. */
static void load(void)
{
    uint8_t last[APRS_RX_INBOX], n = 0, i;
    rec_t r;
    loaded = true;
    for (i = 0; i < RECS; i++) {
        readRec(i, &r);
        if (r.magic == 0xFFu)
            break;                              /* erased: the end */
        if (!bodyOf(&r) || r.sum != sum(r.body, bodyOf(&r)))
            continue;                           /* cut short by a power off */
        memmove(last + 1, last, APRS_RX_INBOX - 1u);
        last[0] = i;
        if (n < APRS_RX_INBOX) n++;
    }
    nextRec = i;
    while (n--) {
        aprs_msg_t m;
        readRec(last[n], &r);
        memset(&m, 0, sizeof m);
        memcpy(&m, r.body, bodyOf(&r));
        m.unread = r.unread != 0;
        aprs_rx_put(&m);
        memmove(recOf + 1, recOf, APRS_RX_INBOX - 1u);
        memmove(recUnread + 1, recUnread, APRS_RX_INBOX - 1u);
        recOf[0] = last[n];
        recUnread[0] = m.unread;
    }
}

/* Writes a new message, and the read flags apps cleared. */
void APRS_STORE_Sync(void)
{
    if (!loaded)
        load();
    const aprs_msg_t *m = aprs_rx_poll();
    const uint8_t n = aprs_rx_count();
    if (m) {
        memmove(recOf + 1, recOf, APRS_RX_INBOX - 1u);
        memmove(recUnread + 1, recUnread, APRS_RX_INBOX - 1u);
        if (nextRec >= RECS) {                  /* full: erase, write the inbox again */
            PY25Q16_SectorErasePhysical(APP_RX_INBOX_ADDR);
            nextRec = 0;
            for (uint8_t i = n; i-- > 1u;)
                writeRec(aprs_rx_msg(i), i);
        }
        writeRec(m, 0);
    }
    for (uint8_t i = 0; i < n; i++) {
        if (recUnread[i] && !aprs_rx_msg(i)->unread) {
            static const uint8_t zero = 0;
            PY25Q16_WriteBufferPhysical(APP_RX_INBOX_ADDR + recOf[i] * REC_SIZE + offsetof(rec_t, unread), &zero, 1);
            recUnread[i] = 0;
        }
    }
}

