#include "internal.h"

#include <stdlib.h>
#include <string.h>

#define MAX_HEADS 64

typedef struct {
    uint64_t v[MAX_HEADS];
    size_t   n;
    int      overflow;
} HeadSet;

void model_init(Model *m)
{
    memset(m, 0, sizeof *m);
    index_init(&m->ix);
}

void model_free(Model *m)
{
    for (size_t i = 0; i < m->n_recs; i++)
        record_free(&m->recs[i]);
    free(m->recs);
    index_free(&m->ix);
    memset(m, 0, sizeof *m);
}

const Record *model_record(const Model *m, uint64_t id)
{
    if (id == 0 || id > m->n_recs)
        return NULL;
    return &m->recs[id - 1];
}

static void hs_remove(HeadSet *hs, uint64_t id)
{
    for (size_t i = 0; i < hs->n; i++) {
        if (hs->v[i] == id) {
            memmove(&hs->v[i], &hs->v[i + 1], (hs->n - i - 1) * sizeof hs->v[0]);
            hs->n--;
            return;
        }
    }
}

static void hs_add(HeadSet *hs, uint64_t id)
{
    if (hs->n >= MAX_HEADS) {
        hs->overflow = 1;
        return;
    }
    hs->v[hs->n++] = id;
}

/*
 * The Canon fold. Records for a key are visited in ascending lsn order:
 *   - a retraction removes its target from the live heads
 *   - an explicit supersedes edge removes that predecessor
 *   - otherwise a new claim replaces the sole head, unless it forks
 * The survivors are the heads; exactly one means Canon, more than one is a
 * schism, none means the key currently has no live claim.
 */
static void fold_heads(const Model *m, const uint64_t *ids, size_t n,
                       uint64_t asof, HeadSet *hs)
{
    hs->n = 0;
    hs->overflow = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t id = ids[i];
        if (asof && id > asof)
            break;
        const Record *r = model_record(m, id);
        if (!r)
            continue;
        if (r->op == RECORD_RETRACT) {
            hs_remove(hs, r->target);
            continue;
        }
        if (r->supersedes) {
            hs_remove(hs, r->supersedes);
        } else if (hs->n == 1 && !r->fork) {
            hs->n = 0;
        }
        /* 0 heads -> root; >=2 heads or a fork -> a competing head */
        hs_add(hs, id);
    }
}

static void recompute_key(const Model *m, KeyEntry *e)
{
    HeadSet hs;
    fold_heads(m, e->ids, e->n, 0, &hs);
    e->st.head_count = hs.overflow ? (uint32_t)MAX_HEADS + 1 : (uint32_t)hs.n;
    e->st.head_lsn = (hs.n == 1 && !hs.overflow) ? hs.v[0] : 0;
    e->st.flags = (hs.n > 1 || hs.overflow) ? KEY_FLAG_SCHISM : 0;
}

int model_apply(Model *m, const Record *r)
{
    if (r->id == 0)
        return NIS_CORRUPT;
    if (r->id != m->n_recs + 1)
        return NIS_CORRUPT; /* ids must be dense and strictly increasing */

    if (r->id > m->cap_recs) {
        size_t cap = m->cap_recs ? m->cap_recs * 2 : 64;
        while (cap < r->id)
            cap *= 2;
        Record *p = realloc(m->recs, cap * sizeof *p);
        if (!p)
            return NIS_NOMEM;
        m->recs = p;
        m->cap_recs = cap;
    }
    if (record_copy(&m->recs[r->id - 1], r) != NIS_OK)
        return NIS_NOMEM;
    m->n_recs = r->id;

    /*
     * A retraction is indexed under the key of the record it targets, so a
     * tombstone always lands in the same chain as its claim even if the
     * frame omits the key.
     */
    const void *kptr = r->key.data;
    size_t klen = r->key.len;
    if (r->op == RECORD_RETRACT && r->target) {
        const Record *t = model_record(m, r->target);
        if (t) {
            kptr = t->key.data;
            klen = t->key.len;
        }
    }

    KeyEntry *e = index_insert(&m->ix, kptr, klen);
    if (!e)
        return NIS_NOMEM;
    if (e->n == e->cap) {
        size_t cap = e->cap ? e->cap * 2 : 8;
        uint64_t *ids = realloc(e->ids, cap * sizeof *ids);
        if (!ids)
            return NIS_NOMEM;
        e->ids = ids;
        e->cap = cap;
    }
    e->ids[e->n++] = r->id;
    recompute_key(m, e);
    return NIS_OK;
}

static int query_canon(const Model *m, const void *key, size_t klen,
                       uint64_t asof, KeyState *st, const Record **head)
{
    KeyEntry *e = index_find((Index *)&m->ix, key, klen);
    if (!e)
        return NIS_NOTFOUND;
    HeadSet hs;
    fold_heads(m, e->ids, e->n, asof, &hs);
    uint32_t count = hs.overflow ? (uint32_t)MAX_HEADS + 1 : (uint32_t)hs.n;
    if (st) {
        st->head_count = count;
        st->head_lsn = (hs.n == 1 && !hs.overflow) ? hs.v[0] : 0;
        st->flags = (hs.n > 1 || hs.overflow) ? KEY_FLAG_SCHISM : 0;
    }
    if (count == 0)
        return NIS_NOTFOUND;
    if (count > 1 || hs.overflow)
        return NIS_SCHISM;
    if (head)
        *head = model_record(m, hs.v[0]);
    return NIS_OK;
}

int model_canon(Model *m, const void *key, size_t klen, KeyState *st, const Record **head)
{
    return query_canon(m, key, klen, 0, st, head);
}

int model_canon_asof(Model *m, const void *key, size_t klen, uint64_t asof,
                     KeyState *st, const Record **head)
{
    return query_canon(m, key, klen, asof, st, head);
}

int model_history(Model *m, const void *key, size_t klen, nis_history_cb cb, void *ud)
{
    KeyEntry *e = index_find(&m->ix, key, klen);
    if (!e)
        return NIS_NOTFOUND;
    for (size_t i = 0; i < e->n; i++) {
        const Record *r = model_record(m, e->ids[i]);
        if (!r)
            continue;
        int rc = cb(ud, r);
        if (rc != NIS_OK)
            return rc;
    }
    return NIS_OK;
}

int model_schisms(Model *m, const void *key, size_t klen, nis_history_cb cb, void *ud)
{
    KeyEntry *e = index_find(&m->ix, key, klen);
    if (!e)
        return NIS_NOTFOUND;
    HeadSet hs;
    fold_heads(m, e->ids, e->n, 0, &hs);
    if (hs.n < 2 && !hs.overflow)
        return NIS_OK;
    for (size_t i = 0; i < hs.n; i++) {
        const Record *r = model_record(m, hs.v[i]);
        if (r && cb(ud, r) != NIS_OK)
            return NIS_ERR;
    }
    return NIS_OK;
}
