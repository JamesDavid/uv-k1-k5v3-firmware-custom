/*
 * Background CW receiver: Morse sent as a tone over FM (MCW, as OP CW sends
 * it), decoded from the same RX audio samples as the APRS receiver (9.6 kHz).
 * A tone of 600-1000 Hz that stands out of the others is the key down; the
 * speed follows the sender. Platform-free: App/app/aprs_bg.c calls it from the
 * ADC interrupt, tools/aprs_rx_test/cwtest.c with recorded audio.
 */
#ifndef CW_RX_H
#define CW_RX_H

#include <stdint.h>
#include <stdbool.h>

#define CW_RX_TEXT  32u         /* the last characters heard */

void cw_rx_init(void);
/* One RX audio sample (12-bit ADC code around 2048); quiet: a frame of
 * another kind is on the air (APRS), not CW. */
void cw_rx_sample(uint16_t adc, bool quiet);
/* The characters heard, oldest first, NUL-terminated ("" for none). */
extern char cw_rx_text[CW_RX_TEXT + 1];
/* The speed heard, in WPM (0 before the first characters). */
uint8_t cw_rx_wpm(void);

#endif
