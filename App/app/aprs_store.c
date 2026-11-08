/* The APRS inbox kept in external flash (APP_RX_INBOX_ADDR), see aprs_store.h.
 *
 * One sector of 128-byte records appended in order: a message is written as
 * soon as the inbox is asked for after it came in, and a read one has its flag
 * byte programmed to 0, which needs no erase. A full sector is erased and the
 * inbox written again (~0.1 s, every 32 messages). The raw page program and
 * erase leave the sector cache alone, which holds the running app. A record a
 * power off cut short fails its checksum and is skipped. Records written
 * before messages carried their frequency (magic 0xA5, 84-byte body) still
 * load, with no frequency. The saved locations are a record of their own
 * (magic 0xA7), the last one written the one in use.
 */
#include <stddef.h>
#include <string.h>

#include "app/aprs_store.h"
#include "apps/app_api.h"
#include "driver/py25q16.h"
#include "onepress/aprs_rx.h"
#include "onepress/aprs_tx.h"

#define REC_SIZE    128u
#define RECS        (0x1000u / REC_SIZE)
#define REC_MAGIC   0xA6u
#define REC_BODY    sizeof(aprs_msg_t)          /* its unread byte unused: the flag byte says */
#define OLD_MAGIC   0xA5u
#define OLD_BODY    84u                         /* from, text, id */
#define LOC_MAGIC   0xA7u
typedef struct { uint8_t magic, unread, sum, body[REC_BODY]; } rec_t;
/* The locations record: the locations, and the station listened for. */
typedef struct { app_loc_t loc; char call[7]; uint8_t ssid; } svc_t;
_Static_assert(sizeof(svc_t) <= REC_BODY, "the locations fit a record");

static svc_t svc;
#define loc (svc.loc)
static bool locSaved;                           /* a location record is in the sector */

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
static uint8_t bodyOf(const rec_t *r)
{
    return r->magic == REC_MAGIC ? REC_BODY : r->magic == OLD_MAGIC ? OLD_BODY : r->magic == LOC_MAGIC ? sizeof(svc_t) : 0;
}

static void readRec(uint8_t i, rec_t *r) { PY25Q16_ReadBuffer(APP_RX_INBOX_ADDR + i * REC_SIZE, r, sizeof *r); }

static void writeLoc(void)
{
    rec_t r;
    memset(&r, 0, sizeof r);
    r.magic = LOC_MAGIC;
    memcpy(r.body, &svc, sizeof svc);
    r.sum = sum(r.body, sizeof svc);
    PY25Q16_WriteBufferPhysical(APP_RX_INBOX_ADDR + nextRec++ * REC_SIZE, &r, sizeof r);
    locSaved = true;
}

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
    for (i = 0; i < 4u; i++) loc.lat[i] = loc.lon[i] = APP_LOC_NONE;
    for (i = 0; i < RECS; i++) {
        readRec(i, &r);
        if (r.magic == 0xFFu)
            break;                              /* erased: the end */
        if (!bodyOf(&r) || r.sum != sum(r.body, bodyOf(&r)))
            continue;                           /* cut short by a power off */
        if (r.magic == LOC_MAGIC) {
            memcpy(&svc, r.body, sizeof svc);
            locSaved = true;
            continue;
        }
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

/* Room for one more record: a full sector erased, the inbox (but its newest
 * `skip` messages) and the locations written again. */
static void room(uint8_t skip)
{
    if (nextRec < RECS)
        return;
    PY25Q16_SectorErasePhysical(APP_RX_INBOX_ADDR);
    nextRec = 0;
    for (uint8_t i = aprs_rx_count(); i-- > skip;)
        writeRec(aprs_rx_msg(i), i);
    if (locSaved)
        writeLoc();
}

app_loc_t *APRS_STORE_Loc(void)
{
    if (!loaded)
        load();
    return &loc;
}

bool APRS_STORE_Station(char *call, uint8_t *ssid)
{
    APRS_STORE_Loc();
    memcpy(call, svc.call, sizeof svc.call);
    *ssid = svc.ssid;
    return svc.call[0] != 0;
}

void APRS_STORE_SetStation(const char *call, uint8_t ssid)
{
    APRS_STORE_Loc();
    if (!memcmp(call, svc.call, sizeof svc.call) && ssid == svc.ssid)
        return;
    memcpy(svc.call, call, sizeof svc.call);
    svc.ssid = ssid;
    APRS_STORE_LocSave();
}

uint8_t APRS_STORE_LocReport(char *out)
{
    const app_loc_t *l = APRS_STORE_Loc();
    const uint8_t a = l->attach;
    if (a < 1u || a > 4u || l->lat[a - 1u] == APP_LOC_NONE)
        return 0;
    return aprs_tx_position(out, l->lat[a - 1u], l->lon[a - 1u], '[');   /* a person */
}

void APRS_STORE_LocSave(void)
{
    if (!loaded)
        load();
    if (nextRec >= RECS) {                      /* the sector written again, the locations with it */
        locSaved = true;
        room(0);
    } else
        writeLoc();
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
        room(1);                                /* the new one is written below */
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

