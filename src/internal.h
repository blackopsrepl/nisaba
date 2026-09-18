#ifndef NISABA_INTERNAL_H
#define NISABA_INTERNAL_H

#include "nisaba.h"

#include <stdio.h>

/* ------------------------------------------------------------------ */
/* TLV body codec                                                     */
/* ------------------------------------------------------------------ */

int codec_encode_body(const Record *r, Buf *out);
int codec_decode_body(const uint8_t *p, size_t n, Record *r);

/* ------------------------------------------------------------------ */
/* Append-only record log                                             */
/* ------------------------------------------------------------------ */

/*
 * Log file layout
 *   [0, 64)      LogHeader: magic, version, flags, reserved, crc32c
 *   [64, EOF)    frames, back to back
 *
 * Frame:
 *   magic u32 | frame_len u32 | lsn u64 | flags u16 | body_len u32
 *   body[body_len] | crc32c u32
 * frame_len = 22 + body_len + 4; crc32c covers the 22-byte header and body.
 */

#define LOG_HEADER_SIZE 64u
#define FRAME_HEADER_SIZE 22u
#define FRAME_TRAILER_SIZE 4u

typedef struct {
    FILE    *fp;
    char    *path;
    uint64_t next_lsn;  /* lsn to assign to the next appended record */
    uint64_t valid_len; /* end of the last fully valid frame */
    int      readonly;
} Log;

int  log_open(Log *l, const char *path, int create, int readonly);
void log_close(Log *l);
int  log_append(Log *l, const Record *r, uint64_t *offset_out);
int  log_scan(Log *l, int (*cb)(void *ud, const Record *r, uint64_t off), void *ud);
int  log_read_record(Log *l, uint64_t offset, Record *out);

/* ------------------------------------------------------------------ */
/* In-memory ordered index                                            */
/* ------------------------------------------------------------------ */

#define KEY_FLAG_SCHISM 0x01u

typedef struct {
    uint64_t *ids;  /* lsns of every record for this key, ascending */
    size_t    n;
    size_t    cap;
    KeyState  st;   /* derived canon state */
} KeyEntry;

typedef struct {
    Buf      *keys; /* sorted by memcmp, then length */
    KeyEntry *ents;
    size_t    n;
    size_t    cap;
} Index;

void       index_init(Index *ix);
void       index_free(Index *ix);
KeyEntry  *index_find(Index *ix, const void *key, size_t klen);
KeyEntry  *index_insert(Index *ix, const void *key, size_t klen);
size_t     index_lower_bound(const Index *ix, const void *key, size_t klen);
int        key_cmp(const void *a, size_t alen, const void *b, size_t blen);

/* ------------------------------------------------------------------ */
/* Record model: supersession fold over the log                       */
/* ------------------------------------------------------------------ */

typedef struct {
    Index   ix;
    Record *recs;   /* recs[lsn - 1] */
    size_t  n_recs;
    size_t  cap_recs;
} Model;

void model_init(Model *m);
void model_free(Model *m);
int  model_apply(Model *m, const Record *r);
int  model_canon(Model *m, const void *key, size_t klen, KeyState *st, const Record **head);
int  model_canon_asof(Model *m, const void *key, size_t klen, uint64_t asof,
                      KeyState *st, const Record **head);
int  model_history(Model *m, const void *key, size_t klen, nis_history_cb cb, void *ud);
int  model_schisms(Model *m, const void *key, size_t klen, nis_history_cb cb, void *ud);
const Record *model_record(const Model *m, uint64_t id);

/* ------------------------------------------------------------------ */
/* Paged B+tree index snapshot                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    Buf      data;       /* whole index file; page 0 is the header */
    uint32_t page_count;
    uint64_t root_page;
    uint64_t log_offset;
    uint64_t log_lsn;
    uint64_t generation;
} BTree;

int  btree_build(const char *path, const Index *ix, uint64_t log_offset,
                 uint64_t log_lsn, uint64_t generation);
int  btree_open(BTree *bt, const char *path);
void btree_close(BTree *bt);
int  btree_get(BTree *bt, const void *key, size_t klen, KeyState *out);
int  btree_scan(BTree *bt, const void *prefix, size_t plen, size_t limit,
                nis_scan_cb cb, void *ud);

#endif /* NISABA_INTERNAL_H */
