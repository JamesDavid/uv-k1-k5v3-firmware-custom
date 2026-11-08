/*
 * The APRS inbox in external flash (App/app/aprs_store.c) against a simulated
 * NOR flash kept in a file between runs, one run per power on: it loads the
 * inbox, optionally hears recorded audio (gen.py) and marks a message read,
 * then prints the inbox and saves the flash. Programming only clears bits, as
 * on the chip; an erase sets the sector back to 0xFF.
 *
 *   storetest flash.bin [audio.u16] [--read N] [--cut]
 *
 * --cut stops at the first record write, half done (a power off mid-write).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apps/app_api.h"
#include "app/aprs_store.h"
#include "onepress/aprs_rx.h"

static uint8_t sector[0x1000];
static int writes, erases, cut;
static const char *flashFile;

static void save(void)
{
    FILE *f = fopen(flashFile, "wb");
    fwrite(sector, 1, sizeof sector, f);
    fclose(f);
}

void PY25Q16_ReadBuffer(uint32_t a, void *buf, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        ((uint8_t *)buf)[i] = a + i >= APP_RX_INBOX_ADDR && a + i < APP_RX_INBOX_ADDR + 0x1000u ? sector[a + i - APP_RX_INBOX_ADDR] : 0xFF;
}
void PY25Q16_WriteBufferPhysical(uint32_t a, const void *buf, uint32_t n)
{
    if (a < APP_RX_INBOX_ADDR || a + n > APP_RX_INBOX_ADDR + 0x1000u) { printf("WRITE OUTSIDE THE SECTOR %06x\n", a); exit(1); }
    if (cut && n > 1) n /= 2;
    for (uint32_t i = 0; i < n; i++) sector[a - APP_RX_INBOX_ADDR + i] &= ((const uint8_t *)buf)[i];
    writes++;
    if (cut && n > 1) { save(); printf("(power off during a write)\n"); exit(0); }
}
void PY25Q16_SectorErasePhysical(uint32_t a)
{
    if (a != APP_RX_INBOX_ADDR) { printf("ERASE OUTSIDE THE SECTOR %06x\n", a); exit(1); }
    memset(sector, 0xFF, sizeof sector);
    erases++;
}

static void show(const char *when)
{
    printf("%s: %u message(s)\n", when, aprs_rx_count());
    for (uint8_t i = 0; i < aprs_rx_count(); i++) {
        const aprs_msg_t *m = aprs_rx_msg(i);
        printf("  %u %s %s: %s\n", i, m->unread ? "NEW " : "read", m->from, m->text);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: storetest flash.bin [audio.u16] [--read N] [--cut]\n"); return 2; }
    flashFile = argv[1];
    const char *audio = NULL;
    int readN = -1;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--read") && i + 1 < argc) readN = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cut")) cut = 1;
        else audio = argv[i];
    }
    memset(sector, 0xFF, sizeof sector);
    FILE *f = fopen(flashFile, "rb");
    if (f) { if (fread(sector, 1, sizeof sector, f) != sizeof sector) memset(sector, 0xFF, sizeof sector); fclose(f); }
    APRS_STORE_Sync();                          /* power on: the inbox back */
    aprs_rx_init("KE7AQA", 7);
    show("at power on");
    if (audio) {
        FILE *af = fopen(audio, "rb");
        if (!af) { perror(audio); return 1; }
        unsigned char b[2];
        long n = 0;
        while (fread(b, 1, 2, af) == 2) {
            aprs_rx_sample((uint16_t)(b[0] | b[1] << 8));
            if (!(++n % 480)) APRS_STORE_Sync();
        }
        fclose(af);
        APRS_STORE_Sync();
    }
    if (readN >= 0 && aprs_rx_msg((uint8_t)readN)) { aprs_rx_msg((uint8_t)readN)->unread = 0; APRS_STORE_Sync(); }
    show("now");
    uint8_t used = 0;
    while (used < 32 && sector[used * 128] != 0xFF) used++;
    printf("records %u/32, writes %d, erases %d\n", used, writes, erases);
    save();
    return 0;
}
