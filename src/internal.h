#ifndef NISABA_INTERNAL_H
#define NISABA_INTERNAL_H

#include "nisaba.h"

/* ------------------------------------------------------------------ */
/* Record model                                                       */
/* ------------------------------------------------------------------ */

#define NIS_VALID_TIME_UNSET INT64_MIN

typedef enum {
    VAL_NULL   = 0,
    VAL_BOOL   = 1,
    VAL_INT    = 2,
    VAL_DOUBLE = 3,
    VAL_STR    = 4,
    VAL_BYTES  = 5
} ValueType;

typedef enum {
    RECORD_PUT     = 0,
    RECORD_RETRACT = 1
} RecordOp;

typedef struct {
    uint64_t id;          /* log sequence number; == inscription id */
    uint64_t supersedes;  /* lsn of predecessor, 0 = none */
    uint64_t target;      /* retraction target, 0 = none */
    uint64_t offset;      /* byte offset in the log (filled on scan) */
    uint8_t  op;          /* RecordOp */
    uint8_t  fork;        /* do not auto-link to the current head */
    uint8_t  vtype;       /* ValueType */
    int64_t  valid_time;  /* NIS_VALID_TIME_UNSET when unset */
    int64_t  recorded_at; /* wall clock, informational only */
    Buf key;
    Buf value;
    Buf witness;
    Buf source;
} Record;

void record_init(Record *r);
void record_free(Record *r);
int  record_copy(Record *dst, const Record *src);

/* ------------------------------------------------------------------ */
/* TLV body codec                                                     */
/* ------------------------------------------------------------------ */

int codec_encode_body(const Record *r, Buf *out);
int codec_decode_body(const uint8_t *p, size_t n, Record *r);

#endif /* NISABA_INTERNAL_H */
