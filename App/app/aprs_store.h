/* The APRS inbox (App/onepress/aprs_rx.c) kept in external flash over power off. */
#ifndef APP_APRS_STORE_H
#define APP_APRS_STORE_H

/* Loads the inbox the first time, then writes a message received since the
 * last call and the read flags cleared. The main loop, before reading the inbox. */
void APRS_STORE_Sync(void);

#endif
