/*
 * The background CW receiver (App/onepress/cw_rx.c) on recorded RX audio
 * (cwgen.py, or gen.py's APRS traffic: nothing should come out of it), with
 * the APRS receiver beside it as on the radio: its busy flag quiets the CW one.
 *
 *   cwtest file.u16
 */
#include <stdio.h>
#include "onepress/aprs_rx.h"
#include "onepress/cw_rx.h"

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: cwtest file.u16\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    aprs_rx_init("KE7AQA", 7);
    cw_rx_init();
    unsigned char b[2];
    while (fread(b, 1, 2, f) == 2) {
        const uint16_t v = (uint16_t)(b[0] | b[1] << 8);
        aprs_rx_sample(v);
        cw_rx_sample(v, aprs_rx_busy());
    }
    fclose(f);
    printf("heard \"%s\" at %u WPM\n", cw_rx_text, cw_rx_wpm());
    return 0;
}
