/* OnePress text entry for overlay apps (API level 4, APP_CAP_ONEPRESS).
 *
 * The OnePress decoder (App/onepress, github.com/JamesDavid/OnePress-Keyboard,
 * PolyForm Noncommercial 1.0.0) stays resident; its word model is read from
 * external flash at APP_OP_MODEL_ADDR through a small read cache. Apps reach
 * it through app_api_t; see the level 4 block of apps/app_api.h. */
#ifndef APP_ONEPRESS_SVC_H
#define APP_ONEPRESS_SVC_H

#include <stdint.h>
#include "apps/app_api.h"

extern char gOpKeys[APP_OP_KEYS_MAX + 1];
extern char gOpText[APP_OP_TEXT_MAX + 1];

int8_t   OP_Open(void);
int16_t  OP_Decode(const char *keys, const app_op_lock_t *locks, uint8_t n_locks);
int16_t  OP_Render(char *out, uint8_t cap);
uint8_t  OP_Guesses(const char *keys, const app_op_lock_t *locks, uint8_t n_locks,
                    uint8_t slot, uint16_t *ids, uint8_t max);
uint16_t OP_Choice(uint8_t slot, uint8_t *kind);
uint8_t  OP_Word(uint16_t id, char *buf, uint8_t cap);

#endif
