/*
 * Background APRS message receiver: the Bell 202 demodulator of the APRS RX
 * app run on every RX audio sample (9.6 kHz), keeping only APRS messages
 * addressed to this station, in a small inbox.
 *
 * aprs_rx_sample() is the whole receiver, platform-free: the radio calls it
 * from an interrupt (App/app/aprs_bg.c), tools/aprs_rx_test feeds it
 * recorded samples.
 */
#ifndef APRS_RX_H
#define APRS_RX_H

#include <stdint.h>
#include <stdbool.h>

#ifndef APRS_RX_SLICERS
#define APRS_RX_SLICERS 2u      /* decision thresholds, each with its own frame: a
                                   third decoded nothing more (tools/aprs_rx_test) */
#endif
#define APRS_RX_INBOX   3u      /* messages kept, newest first                  */
#define APRS_RX_TEXT    67u     /* an APRS message's text                       */

typedef struct {
    char    from[10];           /* "KE7AQA-7"                                   */
    char    text[APRS_RX_TEXT + 1];
    char    id[6];              /* message number ("{12"), "" for none: no ack  */
    uint8_t unread;
    uint8_t pad[3];
    uint32_t freq;              /* heard on (10 Hz units, aprs_rx_freq), 0: unknown */
} aprs_msg_t;

/* This station's callsign and SSID: messages to "CALL-SSID" or "CALL" are
 * kept. Resets the demodulator; the inbox stays (empty at start-up). */
void aprs_rx_init(const char *call, uint8_t ssid);
/* One RX audio sample, the 12-bit ADC code around 2048 (the interrupt). */
void aprs_rx_sample(uint16_t adc);
/* A frame is coming in: an interruption now would lose it. */
bool aprs_rx_busy(void);
/* Moves a message received to the inbox (the main loop): the message when it
 * is a new one, else NULL. */
aprs_msg_t *aprs_rx_poll(void);
/* Puts m in the inbox as the newest (messages kept from before power off). */
void aprs_rx_put(const aprs_msg_t *m);
/* The inbox, newest first (0 = newest), NULL past the last. */
uint8_t aprs_rx_count(void);
aprs_msg_t *aprs_rx_msg(uint8_t n);
uint8_t aprs_rx_unread(void);
/* Frames with a good FCS seen, for any station (a test and debug count). */
extern uint16_t aprs_rx_frames;
/* The frequency listened on, set by the platform: messages and acks carry it. */
extern uint32_t aprs_rx_freq;

/* ---- acknowledgements ----
 * A message to this station with a number ("{12") asks for an ack, every time
 * it is heard (the sender repeats it until one gets through). aprs_rx_poll
 * notes the last one; aprs_rx_ack_due hands it over once (sender, number, and
 * the addressee it was sent to, CALL-SSID or CALL: the ack comes from that).
 * "ack12" / "rej12" to this station answer a message sent with
 * aprs_rx_msgid's number: aprs_rx_delivered tells. */
bool aprs_rx_ack_due(char *to, char *id, char *as, uint32_t *freq);   /* 10, 6, 10 bytes */
/* The number to send text to `to` with, "1".."99" as 1..99: the last one again
 * for the same addressee and text (a repeat), else the next. Starts after seed. */
uint8_t aprs_rx_msgid(const char *to, const char *text);
void aprs_rx_seed(uint8_t n);
/* For the last number given: 0 waiting (or none), 1 acked, 2 rejected. */
uint8_t aprs_rx_delivered(void);

#endif
