#include "internal.h"

#include <stdlib.h>
#include <string.h>

int key_cmp(const void *a, size_t alen, const void *b, size_t blen)
{
    size_t n = alen < blen ? alen : blen;
    int c = n ? memcmp(a, b, n) : 0;
    if (c)
        return c;
    if (alen < blen)
        return -1;
    if (alen > blen)
        return 1;
    return 0;
}

void index_init(Index *ix)
{
    memset(ix, 0, sizeof *ix);
}

void index_free(Index *ix)
{
    for (size_t i = 0; i < ix->n; i++) {
        buf_free(&ix->keys[i]);
        free(ix->ents[i].ids);
    }
    free(ix->keys);
    free(ix->ents);
    memset(ix, 0, sizeof *ix);
}

size_t index_lower_bound(const Index *ix, const void *key, size_t klen)
{
    size_t lo = 0, hi = ix->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = key_cmp(ix->keys[mid].data, ix->keys[mid].len, key, klen);
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

KeyEntry *index_find(Index *ix, const void *key, size_t klen)
{
    size_t i = index_lower_bound(ix, key, klen);
    if (i >= ix->n)
        return NULL;
    if (key_cmp(ix->keys[i].data, ix->keys[i].len, key, klen) != 0)
        return NULL;
    return &ix->ents[i];
}

static int index_grow(Index *ix)
{
    if (ix->n < ix->cap)
        return NIS_OK;
    size_t cap = ix->cap ? ix->cap * 2 : 16;
    Buf *keys = realloc(ix->keys, cap * sizeof *keys);
    if (!keys)
        return NIS_NOMEM;
    ix->keys = keys;
    KeyEntry *ents = realloc(ix->ents, cap * sizeof *ents);
    if (!ents)
        return NIS_NOMEM;
    ix->ents = ents;
    ix->cap = cap;
    return NIS_OK;
}

KeyEntry *index_insert(Index *ix, const void *key, size_t klen)
{
    size_t i = index_lower_bound(ix, key, klen);
    if (i < ix->n && key_cmp(ix->keys[i].data, ix->keys[i].len, key, klen) == 0)
        return &ix->ents[i];
    if (index_grow(ix) != NIS_OK)
        return NULL;
    memmove(&ix->keys[i + 1], &ix->keys[i], (ix->n - i) * sizeof *ix->keys);
    memmove(&ix->ents[i + 1], &ix->ents[i], (ix->n - i) * sizeof *ix->ents);
    buf_init(&ix->keys[i]);
    if (buf_set(&ix->keys[i], key, klen) != NIS_OK)
        return NULL;
    memset(&ix->ents[i], 0, sizeof ix->ents[i]);
    ix->n++;
    return &ix->ents[i];
}
