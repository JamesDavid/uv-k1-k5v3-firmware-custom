/* An APRS message frame, see aprs_tx.h. The frame and its FCS are those of the
 * APRS TX app (Copyright 2026 Armel F4HWN, Apache License 2.0), as OP APRS. */
#include <stdbool.h>
#include "aprs_tx.h"

/* An AX.25 address: 6 characters shifted left, then the SSID byte. */
static uint8_t *addr(uint8_t *f, const char *c, bool last)
{
    uint8_t i = 0, ssid = 0;
    for (; i < 6u && c[i] && c[i] != '-'; i++) *f++ = (uint8_t)(c[i] << 1);
    for (uint8_t k = i; k < 6u; k++) *f++ = (uint8_t)(' ' << 1);
    while (c[i] && c[i] != '-') i++;
    if (c[i] == '-') for (i++; c[i] >= '0' && c[i] <= '9'; i++) ssid = (uint8_t)(ssid * 10u + (uint8_t)(c[i] - '0'));
    *f++ = (uint8_t)(0x60u | ((ssid & 15u) << 1) | (last ? 1u : 0u));
    return f;
}

uint8_t aprs_tx_frame(uint8_t *f, const char *src, uint8_t path, const char *to, const char *text)
{
    uint8_t *p = f;
    p = addr(p, "APZOP", false);
    p = addr(p, src, path == 0);
    if (path) p = addr(p, "WIDE1-1", path == 1);
    if (path == 2) p = addr(p, "WIDE2-1", true);
    *p++ = 0x03; *p++ = 0xF0;                       /* UI frame, no layer 3 */
    *p++ = ':';
    for (uint8_t i = 0, end = 0; i < 9u; i++) { if (!to[i]) end = 1; *p++ = (uint8_t)(end ? ' ' : to[i]); }
    *p++ = ':';
    for (uint8_t i = 0; i < 73u && text[i]; i++) *p++ = (uint8_t)text[i];
    uint8_t k = (uint8_t)(p - f);
    uint16_t crc = 0xFFFFu;                         /* CRC-16/X.25 */
    for (uint8_t i = 0; i < k; i++) {
        crc ^= f[i];
        for (uint8_t b = 0; b < 8u; b++) crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408u) : (uint16_t)(crc >> 1);
    }
    crc ^= 0xFFFFu;
    f[k++] = (uint8_t)crc;
    f[k++] = (uint8_t)(crc >> 8);
    return k;
}
