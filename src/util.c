#include "nisaba.h"

#include <stdlib.h>
#include <string.h>

const char *nis_strerror(int rc)
{
    switch (rc) {
    case NIS_OK:       return "ok";
    case NIS_ERR:      return "error";
    case NIS_NOTFOUND: return "not found";
    case NIS_SCHISM:   return "schism: conflicting claims";
    case NIS_CORRUPT:  return "corrupt archive";
    case NIS_IO:       return "i/o error";
    case NIS_USAGE:    return "usage error";
    case NIS_NOMEM:    return "out of memory";
    case NIS_EXISTS:   return "already exists";
    case NIS_BUSY:     return "archive is locked by another writer";
    default:           return "unknown error";
    }
}

/* ------------------------------------------------------------------ */
/* Buffer                                                             */
/* ------------------------------------------------------------------ */

void buf_init(Buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(Buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

int buf_reserve(Buf *b, size_t extra)
{
    size_t need = b->len + extra;
    if (need <= b->cap)
        return NIS_OK;
    size_t cap = b->cap ? b->cap : 32;
    while (cap < need) {
        if (cap > (SIZE_MAX / 2))
            return NIS_NOMEM;
        cap *= 2;
    }
    uint8_t *p = realloc(b->data, cap);
    if (!p)
        return NIS_NOMEM;
    b->data = p;
    b->cap = cap;
    return NIS_OK;
}

int buf_append(Buf *b, const void *p, size_t n)
{
    if (n == 0)
        return NIS_OK;
    int rc = buf_reserve(b, n);
    if (rc != NIS_OK)
        return rc;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return NIS_OK;
}

int buf_set(Buf *b, const void *p, size_t n)
{
    b->len = 0;
    return buf_append(b, p, n);
}

int buf_put_u8(Buf *b, uint8_t v)
{
    return buf_append(b, &v, 1);
}

int buf_put_u16(Buf *b, uint16_t v)
{
    uint8_t p[2] = { (uint8_t)(v & 0xff), (uint8_t)(v >> 8) };
    return buf_append(b, p, sizeof p);
}

int buf_put_u32(Buf *b, uint32_t v)
{
    uint8_t p[4] = {
        (uint8_t)(v & 0xff), (uint8_t)((v >> 8) & 0xff),
        (uint8_t)((v >> 16) & 0xff), (uint8_t)((v >> 24) & 0xff)
    };
    return buf_append(b, p, sizeof p);
}

int buf_put_u64(Buf *b, uint64_t v)
{
    uint8_t p[8];
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)((v >> (8 * i)) & 0xff);
    return buf_append(b, p, sizeof p);
}

/* ------------------------------------------------------------------ */
/* Varints                                                            */
/* ------------------------------------------------------------------ */

size_t varint_len(uint64_t v)
{
    size_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        n++;
    }
    return n;
}

size_t varint_put(uint8_t *p, uint64_t v)
{
    size_t n = 0;
    while (v >= 0x80) {
        p[n++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    p[n++] = (uint8_t)v;
    return n;
}

int varint_get(const uint8_t *p, size_t n, uint64_t *out, size_t *used)
{
    uint64_t v = 0;
    int shift = 0;
    for (size_t i = 0; i < n && i < 10; i++) {
        uint8_t b = p[i];
        v |= (uint64_t)(b & 0x7f) << shift;
        if ((b & 0x80) == 0) {
            *out = v;
            *used = i + 1;
            return NIS_OK;
        }
        shift += 7;
    }
    return NIS_CORRUPT;
}

/* ------------------------------------------------------------------ */
/* CRC-32C                                                            */
/* ------------------------------------------------------------------ */

static uint32_t crc_table[256];
static int crc_table_ready;

static void crc_table_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_table_ready = 1;
}

uint32_t crc32c(uint32_t crc, const void *data, size_t len)
{
    if (!crc_table_ready)
        crc_table_init();
    const uint8_t *p = data;
    crc = ~crc;
    while (len--)
        crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}
