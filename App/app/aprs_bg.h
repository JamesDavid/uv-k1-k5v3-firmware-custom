/* The background APRS message receiver on the radio (API level 5,
 * APP_CAP_APRS_INBOX): App/onepress/aprs_rx.c run from the ADC interrupt. */
#ifndef APP_APRS_BG_H
#define APP_APRS_BG_H

#include <stdbool.h>
#include <stdint.h>
#include "apps/app_api.h"

/* The app services (app_api_t rx_*). */
void          APRS_BG_Start(const char *call, uint8_t ssid);
bool          APRS_BG_Station(char *call, uint8_t *ssid);
uint8_t       APRS_BG_Count(void);
app_rx_msg_t *APRS_BG_Msg(uint8_t n);
uint8_t       APRS_BG_Unread(void);
void          APRS_BG_Stats(app_rx_stats_t *s);
uint8_t       APRS_BG_MsgId(const char *to, const char *text);
uint8_t       APRS_BG_Delivered(void);
/* While an app waits (delay_ms): the inbox, and an ack due sent. */
void          APRS_BG_Service(void);

/* For the loader: off at the end of a chain of apps; paused while an app
 * transmits or captures the audio itself; the BK4829 audio output back on
 * after the loader or a tone set it up. */
void APRS_BG_Stop(void);
void APRS_BG_Pause(void);
void APRS_BG_Resume(void);
void APRS_BG_Tune(void);
void APRS_BG_AfterTone(void);
/* While it runs it owns the ADC: the battery, read between packets. */
bool APRS_BG_Battery(uint16_t *voltage);

#endif
