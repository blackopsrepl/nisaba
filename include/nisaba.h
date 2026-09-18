#ifndef NISABA_H
#define NISABA_H

/*
 * Nisaba - a tiny append-only temporal/provenance key-value database.
 *
 * The append-only record log is the source of truth. The "Canon" (current
 * accepted value of a key) is a deterministic fold over the log, never a
 * stored mutable field. The on-disk index is a disposable cache.
 *
 * Internally this is an ordinary embedded storage engine. The Sumerian names
 * live only at the command-line surface (see cli.c).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NISABA_MAGIC       0x4E495341u /* "NISA" */
#define NISABA_VERSION     1u
#define NISABA_PAGE_SIZE   4096u
#define NISABA_HEADER_SIZE 64u

enum {
    NIS_OK       =  0,
    NIS_ERR      = -1,
    NIS_NOTFOUND = -2,
    NIS_SCHISM   = -3,
    NIS_CORRUPT  = -4,
    NIS_IO       = -5,
    NIS_USAGE    = -6,
    NIS_NOMEM    = -7,
    NIS_EXISTS   = -8,
    NIS_BUSY     = -9,
};

const char *nis_strerror(int rc);

/* ------------------------------------------------------------------ */
/* Growable byte buffer.                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} Buf;

void buf_init(Buf *b);
void buf_free(Buf *b);
int  buf_reserve(Buf *b, size_t extra);
int  buf_append(Buf *b, const void *p, size_t n);
int  buf_set(Buf *b, const void *p, size_t n);
int  buf_put_u8(Buf *b, uint8_t v);
int  buf_put_u16(Buf *b, uint16_t v);
int  buf_put_u32(Buf *b, uint32_t v);
int  buf_put_u64(Buf *b, uint64_t v);

/* ------------------------------------------------------------------ */
/* Unsigned LEB128 varints.                                           */
/* ------------------------------------------------------------------ */

size_t varint_len(uint64_t v);
size_t varint_put(uint8_t *p, uint64_t v);
int    varint_get(const uint8_t *p, size_t n, uint64_t *out, size_t *used);

/* ------------------------------------------------------------------ */
/* CRC-32C (Castagnoli).                                              */
/* ------------------------------------------------------------------ */

uint32_t crc32c(uint32_t crc, const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* NISABA_H */
