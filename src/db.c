#define _POSIX_C_SOURCE 200809L

#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

struct Nisaba {
    Log      log;
    Model    model;
    BTree    idx;
    int      have_idx;
    uint64_t generation;
    char    *path;
    char    *idx_path;
};

static int replay_cb(void *ud, const Record *r, uint64_t off)
{
    (void)off;
    return model_apply((Model *)ud, r);
}

static char *suffix_path(const char *path, const char *suffix)
{
    size_t n = strlen(path) + strlen(suffix) + 1;
    char *p = malloc(n);
    if (!p)
        return NULL;
    snprintf(p, n, "%s%s", path, suffix);
    return p;
}

int nis_open(const char *path, int create, Nisaba **out)
{
    Nisaba *db = calloc(1, sizeof *db);
    if (!db)
        return NIS_NOMEM;
    db->path = strdup(path);
    db->idx_path = suffix_path(path, ".idx");
    if (!db->path || !db->idx_path) {
        nis_close(db);
        return NIS_NOMEM;
    }

    int rc = log_open(&db->log, path, create, 0);
    if (rc != NIS_OK) {
        nis_close(db);
        return rc;
    }
    model_init(&db->model);
    rc = log_scan(&db->log, replay_cb, &db->model);
    if (rc != NIS_OK) {
        nis_close(db);
        return rc;
    }

    /* The persisted index is an optimization and a diagnostic, never truth. */
    if (btree_open(&db->idx, db->idx_path) == NIS_OK) {
        db->have_idx = 1;
        db->generation = db->idx.generation;
    }
    *out = db;
    return NIS_OK;
}

void nis_close(Nisaba *db)
{
    if (!db)
        return;
    if (db->have_idx)
        btree_close(&db->idx);
    model_free(&db->model);
    log_close(&db->log);
    free(db->path);
    free(db->idx_path);
    free(db);
}

int nis_inscribe(Nisaba *db, const Record *in, uint64_t *id_out)
{
    if (in->key.len == 0)
        return NIS_USAGE;
    Record r;
    if (record_copy(&r, in) != NIS_OK)
        return NIS_NOMEM;
    r.id = db->log.next_lsn;
    if (r.recorded_at == 0)
        r.recorded_at = (int64_t)time(NULL);
    int rc = log_append(&db->log, &r, &r.offset);
    if (rc == NIS_OK)
        rc = model_apply(&db->model, &r);
    uint64_t id = r.id;
    record_free(&r);
    if (rc != NIS_OK)
        return rc;
    if (id_out)
        *id_out = id;
    return NIS_OK;
}

int nis_retract(Nisaba *db, uint64_t id, uint64_t *new_id)
{
    const Record *t = model_record(&db->model, id);
    if (!t)
        return NIS_NOTFOUND;
    Record r;
    record_init(&r);
    r.op = RECORD_RETRACT;
    r.target = id;
    r.vtype = VAL_NULL;
    if (buf_set(&r.key, t->key.data, t->key.len) != NIS_OK) {
        record_free(&r);
        return NIS_NOMEM;
    }
    int rc = nis_inscribe(db, &r, new_id);
    record_free(&r);
    return rc;
}

int nis_canon(Nisaba *db, const void *key, size_t klen,
              KeyState *st, const Record **head_out)
{
    return model_canon(&db->model, key, klen, st, head_out);
}

int nis_canon_asof(Nisaba *db, const void *key, size_t klen, uint64_t asof,
                   KeyState *st, const Record **head_out)
{
    return model_canon_asof(&db->model, key, klen, asof, st, head_out);
}

int nis_history(Nisaba *db, const void *key, size_t klen,
                nis_history_cb cb, void *ud)
{
    return model_history(&db->model, key, klen, cb, ud);
}

int nis_schisms(Nisaba *db, const void *key, size_t klen,
                nis_history_cb cb, void *ud)
{
    return model_schisms(&db->model, key, klen, cb, ud);
}

int nis_scan(Nisaba *db, const void *prefix, size_t plen, size_t limit,
             nis_scan_cb cb, void *ud)
{
    /*
     * Prefer the on-disk B+tree when it is exactly caught up with the log:
     * this exercises the pager, page offsets, and cursor path at runtime.
     * Otherwise fall back to the in-memory index, which is always current.
     */
    if (db->have_idx && db->idx.log_offset == db->log.valid_len)
        return btree_scan(&db->idx, prefix, plen, limit, cb, ud);

    Index *ix = &db->model.ix;
    size_t i = index_lower_bound(ix, prefix, plen);
    size_t emitted = 0;
    for (; i < ix->n; i++) {
        if (plen && key_cmp(ix->keys[i].data, ix->keys[i].len, prefix, plen) < 0)
            continue;
        if (plen && (ix->keys[i].len < plen ||
                     memcmp(ix->keys[i].data, prefix, plen) != 0))
            break;
        int rc = cb(ud, &ix->keys[i], &ix->ents[i].st);
        if (rc != NIS_OK)
            return rc;
        if (limit && ++emitted >= limit)
            break;
    }
    return NIS_OK;
}

int nis_checkpoint(Nisaba *db)
{
    db->generation++;
    int rc = btree_build(db->idx_path, &db->model.ix, db->log.valid_len,
                         db->log.next_lsn - 1, db->generation);
    if (rc != NIS_OK)
        return rc;
    if (db->have_idx)
        btree_close(&db->idx);
    db->have_idx = 0;
    if (btree_open(&db->idx, db->idx_path) == NIS_OK)
        db->have_idx = 1;
    return NIS_OK;
}

int nis_verify(Nisaba *db)
{
    BTree bt;
    int rc = btree_open(&bt, db->idx_path);
    if (rc == NIS_IO || rc == NIS_CORRUPT)
        return rc;
    if (rc == NIS_OK)
        btree_close(&bt);
    return NIS_OK;
}

uint64_t nis_next_id(const Nisaba *db)
{
    return db->log.next_lsn;
}

size_t nis_record_count(const Nisaba *db)
{
    return db->model.n_recs;
}

const char *nis_index_path(const Nisaba *db)
{
    return db->idx_path;
}

const Record *nis_record(const Nisaba *db, uint64_t id)
{
    return model_record(&db->model, id);
}
