/* The APRS inbox (App/onepress/aprs_rx.c) kept in external flash over power off. */
#ifndef APP_APRS_STORE_H
#define APP_APRS_STORE_H

/* Loads the inbox the first time, then writes a message received since the
 * last call and the read flags cleared. The main loop, before reading the inbox. */
void APRS_STORE_Sync(void);

#include "apps/app_api.h"
/* The saved locations (app_loc_t), loaded with the inbox; LocSave writes
 * them, as one more record of the inbox's sector. */
app_loc_t *APRS_STORE_Loc(void);
void       APRS_STORE_LocSave(void);
/* The attached location's position report (aprs_tx_position, 21 bytes): its
 * length, 0 when none is attached or the slot is not set. */
uint8_t    APRS_STORE_LocReport(char *out);

#endif
