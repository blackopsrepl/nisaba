#define _POSIX_C_SOURCE 200809L

#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Index file
 *   page 0        IdxHeader (64 meaningful bytes in a 4096-byte page)
 *   pages 1..N    B+tree pages
 *
 * Page header (16 bytes):
 *   type u8 | flags u8 | n_cells u16 | content u16 | reserved u16 | link u64
 * followed by n_cells u16 offsets to cells, counted from the page start.
 * Cells are packed downward from the end of the page.
 *
 * Leaf cell:     u16 klen | key | head_lsn u64 | head_count u32 | flags u8
 * Interior cell: u32 child | u16 klen | key
 *
 * For an interior page, `link` is the leftmost child and cell i carries the
 * smallest key of child i (children 1..n); a leaf's `link` is the next leaf.
 */

#define PAGE_HEADER 16u
#define MAX_KEY     4000u

enum { PAGE_INTERIOR = 2, PAGE_LEAF = 5 };

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wr64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static const uint8_t *page(const BTree *bt, uint32_t id)
{
    return bt->data.data + (size_t)id * NISABA_PAGE_SIZE;
}

/* --- page construction -------------------------------------------- */

typedef struct {
    uint8_t  buf[NISABA_PAGE_SIZE];
    uint8_t  type;
    uint16_t n;
    uint16_t content;
    uint16_t offs[(NISABA_PAGE_SIZE - PAGE_HEADER) / 2];
} Page;

static void page_reset(Page *p, uint8_t type)
{
    memset(p->buf, 0, sizeof p->buf);
    p->type = type;
    p->n = 0;
    p->content = (uint16_t)NISABA_PAGE_SIZE;
}

static int page_fits(const Page *p, size_t celllen)
{
    return PAGE_HEADER + (size_t)2 * (p->n + 1) + celllen <= p->content;
}

static void page_add(Page *p, const uint8_t *cell, size_t len)
{
    p->content = (uint16_t)(p->content - len);
    memcpy(p->buf + p->content, cell, len);
    p->offs[p->n++] = p->content;
}

static void page_finish(Page *p, uint64_t link)
{
    p->buf[0] = p->type;
    p->buf[1] = 0;
    wr16(p->buf + 2, p->n);
    wr16(p->buf + 4, p->content);
    wr16(p->buf + 6, 0);
    wr64(p->buf + 8, link);
    for (uint16_t i = 0; i < p->n; i++)
        wr16(p->buf + PAGE_HEADER + 2 * i, p->offs[i]);
}

typedef struct {
    uint32_t page;
    Buf      minkey;
} ChildRef;

static int append_page(Buf *file, const Page *p, uint32_t *page_count)
{
    int rc = buf_append(file, p->buf, NISABA_PAGE_SIZE);
    if (rc != NIS_OK)
        return rc;
    (*page_count)++;
    return NIS_OK;
}

static void set_link(Buf *file, uint32_t id, uint64_t link)
{
    wr64(file->data + (size_t)id * NISABA_PAGE_SIZE + 8, link);
}

static void childrefs_free(ChildRef *c, size_t n)
{
    for (size_t i = 0; i < n; i++)
        buf_free(&c[i].minkey);
    free(c);
}

/* --- build -------------------------------------------------------- */

static int leaf_cell(const Buf *key, const KeyState *st, uint8_t *cell, size_t *len)
{
    if (key->len > MAX_KEY)
        return NIS_ERR;
    size_t o = 0;
    wr16(cell + o, (uint16_t)key->len); o += 2;
    memcpy(cell + o, key->data, key->len); o += key->len;
    wr64(cell + o, st->head_lsn); o += 8;
    wr32(cell + o, st->head_count); o += 4;
    cell[o++] = st->flags;
    *len = o;
    return NIS_OK;
}

static int build_leaves(const Index *ix, Buf *file, uint32_t *page_count,
                        ChildRef **out, size_t *n_out)
{
    ChildRef *refs = NULL;
    size_t nrefs = 0, cap = 0;
    Page p;
    page_reset(&p, PAGE_LEAF);
    Buf min;
    buf_init(&min);
    uint32_t cur_id = 0;
    int have = 0;

    for (size_t i = 0; i <= ix->n; i++) {
        uint8_t cell[2 + MAX_KEY + 13];
        size_t clen = 0;
        int at_end = (i == ix->n);
        if (!at_end) {
            if (leaf_cell(&ix->keys[i], &ix->ents[i].st, cell, &clen) != NIS_OK)
                goto fail;
        }
        if (!at_end && (p.n == 0 || page_fits(&p, clen))) {
            if (p.n == 0)
                buf_set(&min, ix->keys[i].data, ix->keys[i].len);
            page_add(&p, cell, clen);
            continue;
        }
        /* flush current leaf (whether full or last) */
        page_finish(&p, 0);
        uint32_t id = *page_count;
        if (append_page(file, &p, page_count) != NIS_OK)
            goto fail;
        if (have)
            set_link(file, cur_id, id);
        cur_id = id;
        have = 1;
        if (nrefs == cap) {
            size_t ncap = cap ? cap * 2 : 16;
            ChildRef *nr = realloc(refs, ncap * sizeof *nr);
            if (!nr)
                goto fail;
            refs = nr;
            cap = ncap;
        }
        refs[nrefs].page = id;
        buf_init(&refs[nrefs].minkey);
        if (buf_set(&refs[nrefs].minkey, min.data, min.len) != NIS_OK)
            goto fail;
        nrefs++;
        page_reset(&p, PAGE_LEAF);
        buf_set(&min, "", 0);
        if (!at_end) {
            if (p.n == 0)
                buf_set(&min, ix->keys[i].data, ix->keys[i].len);
            page_add(&p, cell, clen);
        }
    }
    buf_free(&min);
    *out = refs;
    *n_out = nrefs;
    return NIS_OK;
fail:
    buf_free(&min);
    childrefs_free(refs, nrefs);
    return NIS_NOMEM;
}

static int build_interior(ChildRef *children, size_t nchildren, Buf *file,
                          uint32_t *page_count, ChildRef **out, size_t *n_out)
{
    ChildRef *next = NULL;
    size_t nnext = 0, cap = 0;
    Page p;
    uint32_t cur_id = 0;
    int have = 0;
    Buf min;
    buf_init(&min);

    size_t i = 0;
    while (i < nchildren) {
        /* leftmost child goes in `link`; the rest become (sep, child) cells */
        page_reset(&p, PAGE_INTERIOR);
        uint64_t leftmost = children[i].page;
        buf_set(&min, children[i].minkey.data, children[i].minkey.len);
        i++;
        for (; i < nchildren; i++) {
            uint8_t cell[4 + 2 + MAX_KEY];
            size_t o = 0;
            wr32(cell + o, children[i].page); o += 4;
            wr16(cell + o, (uint16_t)children[i].minkey.len); o += 2;
            memcpy(cell + o, children[i].minkey.data, children[i].minkey.len);
            o += children[i].minkey.len;
            if (p.n > 0 && !page_fits(&p, o))
                break;
            if (p.n == 0 && !page_fits(&p, o))
                goto fail;
            page_add(&p, cell, o);
        }
        page_finish(&p, leftmost);
        uint32_t id = *page_count;
        if (append_page(file, &p, page_count) != NIS_OK)
            goto fail;
        (void)have; (void)cur_id;
        have = 1; cur_id = id;
        if (nnext == cap) {
            size_t ncap = cap ? cap * 2 : 8;
            ChildRef *nr = realloc(next, ncap * sizeof *nr);
            if (!nr)
                goto fail;
            next = nr;
            cap = ncap;
        }
        next[nnext].page = id;
        buf_init(&next[nnext].minkey);
        if (buf_set(&next[nnext].minkey, min.data, min.len) != NIS_OK)
            goto fail;
        nnext++;
    }
    buf_free(&min);
    *out = next;
    *n_out = nnext;
    return NIS_OK;
fail:
    buf_free(&min);
    childrefs_free(next, nnext);
    return NIS_NOMEM;
}

static int write_atomic(const char *path, const void *data, size_t len)
{
    size_t plen = strlen(path);
    char *tmp = malloc(plen + 8);
    if (!tmp)
        return NIS_NOMEM;
    memcpy(tmp, path, plen);
    memcpy(tmp + plen, ".tmp", 5);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        free(tmp);
        return NIS_IO;
    }
    int ok = fwrite(data, 1, len, f) == len;
    if (ok)
        ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    if (ok)
        ok = rename(tmp, path) == 0;
    if (!ok)
        remove(tmp);
    free(tmp);
    return ok ? NIS_OK : NIS_IO;
}

int btree_build(const char *path, const Index *ix, uint64_t log_offset,
                uint64_t log_lsn, uint64_t generation)
{
    Buf file;
    buf_init(&file);
    uint8_t zero[NISABA_PAGE_SIZE];
    memset(zero, 0, sizeof zero);
    if (buf_append(&file, zero, NISABA_PAGE_SIZE) != NIS_OK) {
        buf_free(&file);
        return NIS_NOMEM;
    }
    uint32_t page_count = 1;

    ChildRef *children = NULL;
    size_t nchildren = 0;
    int rc = build_leaves(ix, &file, &page_count, &children, &nchildren);
    if (rc != NIS_OK) {
        buf_free(&file);
        return rc;
    }

    while (nchildren > 1) {
        ChildRef *next = NULL;
        size_t nnext = 0;
        rc = build_interior(children, nchildren, &file, &page_count, &next, &nnext);
        childrefs_free(children, nchildren);
        if (rc != NIS_OK) {
            buf_free(&file);
            return rc;
        }
        children = next;
        nchildren = nnext;
    }

    uint64_t root = nchildren ? children[0].page : 0;
    childrefs_free(children, nchildren);

    uint8_t h[NISABA_PAGE_SIZE];
    memset(h, 0, sizeof h);
    wr32(h, NISABA_MAGIC);
    wr16(h + 4, NISABA_VERSION);
    wr16(h + 6, NISABA_PAGE_SIZE);
    wr32(h + 8, page_count);
    wr64(h + 16, root);
    wr64(h + 32, log_offset);
    wr64(h + 40, log_lsn);
    wr64(h + 48, generation);
    wr32(h + 60, crc32c(0, h, 60));
    memcpy(file.data, h, NISABA_PAGE_SIZE);

    rc = write_atomic(path, file.data, file.len);
    buf_free(&file);
    return rc;
}

/* --- read --------------------------------------------------------- */

int btree_open(BTree *bt, const char *path)
{
    memset(bt, 0, sizeof *bt);
    buf_init(&bt->data);
    FILE *f = fopen(path, "rb");
    if (!f)
        return NIS_IO;
    uint8_t pg[NISABA_PAGE_SIZE];
    if (fread(pg, 1, sizeof pg, f) != sizeof pg) {
        fclose(f);
        return NIS_CORRUPT;
    }
    if (rd32(pg) != NISABA_MAGIC ||
        rd32(pg + 60) != crc32c(0, pg, 60)) {
        fclose(f);
        return NIS_CORRUPT;
    }
    uint32_t page_count = rd32(pg + 8);
    bt->page_count = page_count;
    bt->root_page = rd64(pg + 16);
    bt->log_offset = rd64(pg + 32);
    bt->log_lsn = rd64(pg + 40);
    bt->generation = rd64(pg + 48);
    if (buf_append(&bt->data, pg, sizeof pg) != NIS_OK) {
        fclose(f);
        btree_close(bt);
        return NIS_NOMEM;
    }
    for (uint32_t i = 1; i < page_count; i++) {
        if (fread(pg, 1, sizeof pg, f) != sizeof pg) {
            fclose(f);
            btree_close(bt);
            return NIS_CORRUPT;
        }
        if (buf_append(&bt->data, pg, sizeof pg) != NIS_OK) {
            fclose(f);
            btree_close(bt);
            return NIS_NOMEM;
        }
    }
    fclose(f);
    return NIS_OK;
}

void btree_close(BTree *bt)
{
    buf_free(&bt->data);
    memset(bt, 0, sizeof *bt);
}

static int leaf_find(const uint8_t *pg, const void *key, size_t klen,
                     KeyState *out)
{
    uint16_t n = rd16(pg + 2);
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const uint8_t *c = pg + rd16(pg + PAGE_HEADER + 2 * mid);
        uint16_t kl = rd16(c);
        int cmp = key_cmp(c + 2, kl, key, klen);
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= n)
        return NIS_NOTFOUND;
    const uint8_t *c = pg + rd16(pg + PAGE_HEADER + 2 * lo);
    uint16_t kl = rd16(c);
    if (key_cmp(c + 2, kl, key, klen) != 0)
        return NIS_NOTFOUND;
    const uint8_t *k = c + 2 + kl;
    out->head_lsn = rd64(k);
    out->head_count = rd32(k + 8);
    out->flags = k[12];
    return NIS_OK;
}

int btree_get(BTree *bt, const void *key, size_t klen, KeyState *out)
{
    if (bt->root_page == 0)
        return NIS_NOTFOUND;
    uint32_t id = (uint32_t)bt->root_page;
    for (;;) {
        const uint8_t *pg = page(bt, id);
        if (pg[0] == PAGE_LEAF)
            return leaf_find(pg, key, klen, out);
        uint16_t n = rd16(pg + 2);
        uint32_t child = (uint32_t)rd64(pg + 8);
        for (uint16_t i = 0; i < n; i++) {
            const uint8_t *c = pg + rd16(pg + PAGE_HEADER + 2 * i);
            uint16_t kl = rd16(c + 4);
            if (key_cmp(key, klen, c + 6, kl) >= 0)
                child = rd32(c);
            else
                break;
        }
        id = child;
    }
}

static int leaf_first_ge(const uint8_t *pg, const void *key, size_t klen,
                         uint16_t *out)
{
    uint16_t n = rd16(pg + 2);
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const uint8_t *c = pg + rd16(pg + PAGE_HEADER + 2 * mid);
        uint16_t kl = rd16(c);
        int cmp = key_cmp(c + 2, kl, key, klen);
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    *out = (uint16_t)lo;
    return NIS_OK;
}

int btree_scan(BTree *bt, const void *prefix, size_t plen, size_t limit,
               nis_scan_cb cb, void *ud)
{
    if (bt->root_page == 0)
        return NIS_OK;
    uint32_t id = (uint32_t)bt->root_page;
    while (page(bt, id)[0] != PAGE_LEAF) {
        const uint8_t *pg = page(bt, id);
        uint16_t n = rd16(pg + 2);
        uint32_t child = (uint32_t)rd64(pg + 8);
        for (uint16_t i = 0; i < n; i++) {
            const uint8_t *c = pg + rd16(pg + PAGE_HEADER + 2 * i);
            uint16_t kl = rd16(c + 4);
            if (key_cmp(prefix, plen, c + 6, kl) >= 0)
                child = rd32(c);
            else
                break;
        }
        id = child;
    }

    size_t emitted = 0;
    while (id != 0) {
        const uint8_t *pg = page(bt, id);
        uint16_t n = rd16(pg + 2);
        uint16_t start = 0;
        leaf_first_ge(pg, prefix, plen, &start);
        for (uint16_t i = start; i < n; i++) {
            const uint8_t *c = pg + rd16(pg + PAGE_HEADER + 2 * i);
            uint16_t kl = rd16(c);
            const uint8_t *k = c + 2;
            if (plen && key_cmp(k, kl, prefix, plen) < 0)
                continue;
            if (kl < plen || (plen && memcmp(k, prefix, plen) != 0))
                return NIS_OK; /* past the prefix range */
            const uint8_t *ks = k + kl;
            KeyState st;
            st.head_lsn = rd64(ks);
            st.head_count = rd32(ks + 8);
            st.flags = ks[12];
            Buf kb;
            kb.data = (uint8_t *)k;
            kb.len = kl;
            kb.cap = 0;
            int rc = cb(ud, &kb, &st);
            if (rc != NIS_OK)
                return rc;
            if (limit && ++emitted >= limit)
                return NIS_OK;
        }
        id = (uint32_t)rd64(pg + 8);
    }
    return NIS_OK;
}
