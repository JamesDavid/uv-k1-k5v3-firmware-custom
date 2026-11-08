/*
 * Feeds recorded RX audio (gen.py) to the background APRS receiver and prints
 * its inbox. --gap MS@S drops MS milliseconds of samples at S seconds: the
 * receiver stopped (interrupts off, an app loading) and missing that audio.
 *
 *   rxtest file.u16 [--gap MS@S ...] [--call KE7AQA --ssid 7]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../App/onepress/aprs_rx.h"

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: rxtest file.u16 [--gap MS@S ...]\n"); return 2; }
    const char *call = "KE7AQA";
    int ssid = 7, ngap = 0;
    long gapAt[16], gapLen[16];
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--call") && i + 1 < argc) call = argv[++i];
        else if (!strcmp(argv[i], "--ssid") && i + 1 < argc) ssid = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gap") && i + 1 < argc && ngap < 16) {
            double ms, s;
            if (sscanf(argv[++i], "%lf@%lf", &ms, &s) == 2) { gapLen[ngap] = (long)(ms * 9.6); gapAt[ngap++] = (long)(s * 9600); }
        }
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    aprs_rx_init(call, (uint8_t)ssid);
    unsigned char b[2];
    long n = 0, skip = 0;
    while (fread(b, 1, 2, f) == 2) {
        for (int g = 0; g < ngap; g++) if (n == gapAt[g]) skip = gapLen[g];
        n++;
        if (skip) { skip--; continue; }
        aprs_rx_sample((uint16_t)(b[0] | b[1] << 8));
        if (!(n % 480)) aprs_rx_poll();          /* the main loop, every 50 ms */
    }
    fclose(f);
    aprs_rx_poll();
    printf("frames %u, inbox %u (unread %u)\n", aprs_rx_frames, aprs_rx_count(), aprs_rx_unread());
    for (uint8_t i = aprs_rx_count(); i-- > 0;) {
        const aprs_msg_t *m = aprs_rx_msg(i);
        printf("  %s: %s%s%s\n", m->from, m->text, m->id[0] ? "  {" : "", m->id);
    }
    return 0;
}
