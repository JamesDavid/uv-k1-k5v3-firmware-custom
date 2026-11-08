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

static char *coord(char *o, int32_t v, uint8_t degs, char plus, char minus)
{
    char h = plus;
    if (v < 0) { v = -v; h = minus; }
    uint32_t u = (uint32_t)v, deg = u / 6000u, r = u % 6000u;
    char d[7] = { (char)('0' + deg / 100u), (char)('0' + deg / 10u % 10u), (char)('0' + deg % 10u),
                  (char)('0' + r / 1000u), (char)('0' + r / 100u % 10u), (char)('0' + r / 10u % 10u), (char)('0' + r % 10u) };
    for (uint8_t i = (uint8_t)(3u - degs); i < 7u; i++) {
        *o++ = d[i];
        if (i == 4u) *o++ = '.';
    }
    *o++ = h;
    return o;
}

uint8_t aprs_tx_position(char *o, int32_t lat, int32_t lon, char sym)
{
    char *p = o;
    *p++ = '!';
    p = coord(p, lat, 2, 'N', 'S');
    *p++ = '/';
    p = coord(p, lon, 3, 'E', 'W');
    *p++ = sym;
    *p = 0;
    return (uint8_t)(p - o);
}

uint8_t aprs_tx_freq_kind(uint32_t f)
{
    static const uint32_t APRS[4] = { 14439000u, 14480000u, 14582500u, 44592500u };
    for (uint8_t i = 0; i < 4u; i++)
        if (f + 500u >= APRS[i] && f <= APRS[i] + 500u)     /* within 5 kHz */
            return (uint8_t)(APRS_TX_APRS | (i ? 0u : APRS_TX_APRS_MAIN));
    return 0;
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
