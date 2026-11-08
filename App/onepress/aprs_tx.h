/*
 * An APRS message as an AX.25 UI frame, FCS included, ready for the AFSK
 * keying: "SRC>APZOP,path::TO       :text". The background receiver sends its
 * acks with it (App/app/aprs_bg.c); tools/aprs_rx_test checks it. Platform-free.
 */
#ifndef APRS_TX_H
#define APRS_TX_H

#include <stdint.h>

#define APRS_TX_FRAME_MAX   (7u * 4u + 2u + 11u + 67u + 6u + 2u)

/* src "KE7AQA-7" (or "KE7AQA"), path 0 direct, 1 WIDE1-1, 2 WIDE1-1,WIDE2-1;
 * to up to 9 characters; text up to 73 (67 and "{12345"). Returns the length. */
uint8_t aprs_tx_frame(uint8_t *f, const char *src, uint8_t path, const char *to, const char *text);
/* A position report's information field, "!4903.50N/07201.75W[" (20
 * characters, NUL-terminated), from lat / lon in 1/6000 degree, north and
 * east positive, with the symbol sym of the primary table. Returns 20. */
uint8_t aprs_tx_position(char *o, int32_t lat, int32_t lon, char sym);
/* What a frequency (10 Hz units) is for, by the band plans: APRS_TX_APRS an
 * APRS frequency (144.390 in North America, the main one: APRS_TX_APRS_MAIN;
 * 144.800 Europe, 145.825 the ISS, 445.925 UHF), APRS_TX_CW_ONLY 144.0-144.1
 * MHz, CW only (US: no MCW, voice or data). */
#define APRS_TX_APRS      0x01u
#define APRS_TX_APRS_MAIN 0x02u
#define APRS_TX_CW_ONLY   0x04u
uint8_t aprs_tx_freq_kind(uint32_t f);

#endif
