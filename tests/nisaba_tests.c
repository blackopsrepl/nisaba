#define _POSIX_C_SOURCE 200809L

#include "nisaba.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int g_pass;
static int g_fail;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (cond) {                                                        \
            g_pass++;                                                      \
        } else {                                                           \
            g_fail++;                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                  \
    } while (0)

static void test_buf(void)
{
    Buf b;
    buf_init(&b);
    CHECK(b.data == NULL && b.len == 0);

    CHECK(buf_put_u8(&b, 0x12) == NIS_OK);
    CHECK(buf_put_u16(&b, 0x3456) == NIS_OK);
    CHECK(buf_put_u32(&b, 0x789ABCDEu) == NIS_OK);
    CHECK(buf_put_u64(&b, 0x0102030405060708ull) == NIS_OK);
    CHECK(b.len == 1 + 2 + 4 + 8);
    CHECK(b.data[0] == 0x12);
    CHECK(b.data[1] == 0x56 && b.data[2] == 0x34);
    CHECK(b.data[3] == 0xDE && b.data[4] == 0xBC);
    CHECK(b.data[5] == 0x9A && b.data[6] == 0x78);
    CHECK(b.data[7] == 0x08 && b.data[14] == 0x01);

    CHECK(buf_set(&b, "abc", 3) == NIS_OK);
    CHECK(b.len == 3 && memcmp(b.data, "abc", 3) == 0);

    buf_free(&b);
    CHECK(b.data == NULL && b.cap == 0);
}

static void test_varint(void)
{
    static const uint64_t vals[] = {
        0, 1, 2, 127, 128, 129, 255, 256, 16383, 16384,
        0x7fffffffULL, 0x80000000ULL, 0xffffffffULL, UINT64_MAX
    };
    uint8_t buf[10];
    for (size_t i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        size_t n = varint_put(buf, vals[i]);
        CHECK(n == varint_len(vals[i]));
        uint64_t out = 0;
        size_t used = 0;
        CHECK(varint_get(buf, n, &out, &used) == NIS_OK);
        CHECK(used == n);
        CHECK(out == vals[i]);
    }
    uint64_t out;
    size_t used;
    CHECK(varint_get((const uint8_t *)"\x80", 1, &out, &used) == NIS_CORRUPT);
}

static void test_crc(void)
{
    CHECK(crc32c(0, "123456789", 9) == 0xE3069283u);
    CHECK(crc32c(0, "", 0) == 0u);

    Buf b;
    buf_init(&b);
    buf_append(&b, "hello", 5);
    uint32_t a = crc32c(0, b.data, b.len);
    b.data[0] ^= 0xff;
    uint32_t c = crc32c(0, b.data, b.len);
    CHECK(a != c);
    buf_free(&b);
}

static int buf_eq(const Buf *b, const char *s)
{
    return b->len == strlen(s) && memcmp(b->data, s, b->len) == 0;
}

static void test_codec(void)
{
    Record r;
    record_init(&r);
    r.id = 7;
    r.op = RECORD_PUT;
    r.vtype = VAL_STR;
    r.recorded_at = 1699999999;
    buf_set(&r.key, "project.status", 14);
    buf_set(&r.value, "active", 6);
    buf_set(&r.witness, "user", 4);
    buf_set(&r.source, "cli", 3);
    r.supersedes = 3;
    r.fork = 1;

    Buf body;
    buf_init(&body);
    CHECK(codec_encode_body(&r, &body) == NIS_OK);
    CHECK(body.len > 0);

    Record d;
    CHECK(codec_decode_body(body.data, body.len, &d) == NIS_OK);
    CHECK(d.id == 0); /* id/offset live in the frame, not the body */
    CHECK(d.op == RECORD_PUT);
    CHECK(d.vtype == VAL_STR);
    CHECK(d.recorded_at == 1699999999);
    CHECK(d.supersedes == 3);
    CHECK(d.fork == 1);
    CHECK(d.valid_time == NIS_VALID_TIME_UNSET);
    CHECK(buf_eq(&d.key, "project.status"));
    CHECK(buf_eq(&d.value, "active"));
    CHECK(buf_eq(&d.witness, "user"));
    CHECK(buf_eq(&d.source, "cli"));
    record_free(&d);

    /* A retraction body. */
    Record ret;
    record_init(&ret);
    ret.op = RECORD_RETRACT;
    ret.target = 7;
    buf_set(&ret.key, "project.status", 14);
    Buf rb;
    buf_init(&rb);
    CHECK(codec_encode_body(&ret, &rb) == NIS_OK);
    Record rd;
    CHECK(codec_decode_body(rb.data, rb.len, &rd) == NIS_OK);
    CHECK(rd.op == RECORD_RETRACT && rd.target == 7);
    CHECK(buf_eq(&rd.key, "project.status"));
    record_free(&rd);
    buf_free(&rb);
    record_free(&ret);

    /* Unknown tags are skipped, not rejected. */
    Buf ext;
    buf_init(&ext);
    CHECK(buf_append(&ext, body.data, body.len) == NIS_OK);
    CHECK(buf_put_u8(&ext, 0x7f) == NIS_OK);
    CHECK(buf_put_u8(&ext, 0x00) == NIS_OK);
    Record x;
    CHECK(codec_decode_body(ext.data, ext.len, &x) == NIS_OK);
    CHECK(buf_eq(&x.key, "project.status"));
    record_free(&x);
    buf_free(&ext);

    /* A truncated body is corrupt, not a crash. */
    Record t;
    CHECK(codec_decode_body(body.data, body.len - 1, &t) == NIS_CORRUPT);

    buf_free(&body);
    record_free(&r);
}

static void mkrec(Record *r, uint64_t id, const char *k, const char *v)
{
    record_init(r);
    r->id = id;
    r->op = RECORD_PUT;
    r->vtype = VAL_STR;
    buf_set(&r->key, k, strlen(k));
    buf_set(&r->value, v, strlen(v));
}

static int count_cb(void *ud, const Record *r, uint64_t off)
{
    (void)r;
    (void)off;
    (*(int *)ud)++;
    return NIS_OK;
}

static void test_log(void)
{
    const char *p = "tests/tmp_log.db";
    remove(p);

    Log l;
    CHECK(log_open(&l, p, 1, 0) == NIS_OK);
    uint64_t offs[3];
    for (uint64_t i = 1; i <= 3; i++) {
        Record r;
        mkrec(&r, i, "k", "v");
        CHECK(log_append(&l, &r, &offs[i - 1]) == NIS_OK);
        record_free(&r);
    }
    CHECK(l.next_lsn == 4);
    log_close(&l);

    Log l2;
    CHECK(log_open(&l2, p, 0, 0) == NIS_OK);
    int n = 0;
    CHECK(log_scan(&l2, count_cb, &n) == NIS_OK);
    CHECK(n == 3);
    CHECK(l2.next_lsn == 4);

    Record got;
    CHECK(log_read_record(&l2, offs[2], &got) == NIS_OK);
    CHECK(buf_eq(&got.value, "v"));
    CHECK(got.id == 3);
    record_free(&got);
    log_close(&l2);

    /* Append a torn suffix: it must be discarded, not accepted. */
    FILE *f = fopen(p, "ab");
    CHECK(f != NULL);
    if (f) {
        fwrite("\x41\x53\x49\x4e garbage-not-a-frame", 1, 25, f);
        fclose(f);
    }
    Log l3;
    CHECK(log_open(&l3, p, 0, 0) == NIS_OK);
    n = 0;
    CHECK(log_scan(&l3, count_cb, &n) == NIS_OK);
    CHECK(n == 3);
    CHECK(l3.next_lsn == 4);
    struct stat st;
    CHECK(stat(p, &st) == 0);
    CHECK((uint64_t)st.st_size == l3.valid_len);
    log_close(&l3);

    /* Corrupt the second frame's body: the first frame survives, the rest
     * of the log is treated as a torn tail. */
    f = fopen(p, "r+b");
    CHECK(f != NULL);
    if (f) {
        CHECK(fseeko(f, (off_t)(offs[1] + FRAME_HEADER_SIZE + 3), SEEK_SET) == 0);
        fputc(0xff, f);
        fclose(f);
    }
    Log l4;
    CHECK(log_open(&l4, p, 0, 0) == NIS_OK);
    n = 0;
    CHECK(log_scan(&l4, count_cb, &n) == NIS_OK);
    CHECK(n == 1);
    log_close(&l4);

    remove(p);
}

static void apply_rec(Model *m, uint64_t id, const char *k, const char *v,
                      uint64_t sup, int fork, uint64_t retract)
{
    Record r;
    record_init(&r);
    r.id = id;
    r.op = retract ? RECORD_RETRACT : RECORD_PUT;
    r.vtype = VAL_STR;
    r.supersedes = sup;
    r.fork = (uint8_t)(fork ? 1 : 0);
    r.target = retract;
    buf_set(&r.key, k, strlen(k));
    buf_set(&r.value, v, strlen(v));
    CHECK(model_apply(m, &r) == NIS_OK);
    record_free(&r);
}

static int hist_cb(void *ud, const Record *r)
{
    (void)r;
    (*(int *)ud)++;
    return NIS_OK;
}

static int scan_cb(void *ud, const Buf *key, const KeyState *st)
{
    (void)key;
    (void)st;
    (*(int *)ud)++;
    return NIS_OK;
}

static uint64_t canon_id(Model *m, const char *k)
{
    KeyState st;
    const Record *head = NULL;
    int rc = model_canon(m, k, strlen(k), &st, &head);
    if (rc == NIS_OK)
        return head->id;
    return 0;
}

static void test_model(void)
{
    Model m;
    model_init(&m);

    apply_rec(&m, 1, "x", "a", 0, 0, 0);
    CHECK(canon_id(&m, "x") == 1);

    /* Implicit supersession: the sole head is replaced. */
    apply_rec(&m, 2, "x", "b", 0, 0, 0);
    CHECK(canon_id(&m, "x") == 2);

    /* Explicit supersession of the current head. */
    apply_rec(&m, 3, "x", "c", 2, 0, 0);
    CHECK(canon_id(&m, "x") == 3);

    /* A fork adds a competing head: schism. */
    apply_rec(&m, 4, "x", "d", 0, 1, 0);
    KeyState st;
    const Record *head = NULL;
    CHECK(model_canon(&m, "x", 1, &st, &head) == NIS_SCHISM);
    CHECK(st.head_count == 2);
    CHECK((st.flags & KEY_FLAG_SCHISM) != 0);

    /* Retracting one head resolves the schism. */
    apply_rec(&m, 5, "", "", 0, 0, 4);
    CHECK(canon_id(&m, "x") == 3);

    /* Retracting the last head leaves no live claim. */
    apply_rec(&m, 6, "", "", 0, 0, 3);
    CHECK(model_canon(&m, "x", 1, &st, &head) == NIS_NOTFOUND);
    CHECK(st.head_count == 0);

    /* Time travel sees the value as of an earlier log position. */
    CHECK(model_canon_asof(&m, "x", 1, 2, &st, &head) == NIS_OK);
    CHECK(head->id == 2);

    /* Independent keys do not interfere. */
    apply_rec(&m, 7, "y", "one", 0, 0, 0);
    CHECK(canon_id(&m, "y") == 7);
    CHECK(canon_id(&m, "x") == 0);

    /* History is the full chain, in log order. */
    int n = 0;
    CHECK(model_history(&m, "x", 1, hist_cb, &n) == NIS_OK);
    CHECK(n == 6);

    model_free(&m);
}

static void test_btree(void)
{
    const char *p = "tests/tmp_idx.idx";
    remove(p);

    Index ix;
    index_init(&ix);
    enum { N = 500 };
    char kb[32];
    for (int i = N - 1; i >= 0; i--) { /* insert out of order */
        snprintf(kb, sizeof kb, "k%04d", i);
        KeyEntry *e = index_insert(&ix, kb, strlen(kb));
        CHECK(e != NULL);
        e->st.head_lsn = (uint64_t)(i + 1);
        e->st.head_count = 1;
        e->st.flags = 0;
    }
    CHECK(ix.n == N);

    CHECK(btree_build(p, &ix, 1234, 500, 7) == NIS_OK);

    BTree bt;
    CHECK(btree_open(&bt, p) == NIS_OK);
    CHECK(bt.log_offset == 1234);
    CHECK(bt.log_lsn == 500);
    CHECK(bt.generation == 7);
    CHECK(bt.root_page != 0);

    for (int i = 0; i < N; i++) {
        snprintf(kb, sizeof kb, "k%04d", i);
        KeyState st;
        CHECK(btree_get(&bt, kb, strlen(kb), &st) == NIS_OK);
        CHECK(st.head_lsn == (uint64_t)(i + 1));
    }
    KeyState st;
    CHECK(btree_get(&bt, "nope", 4, &st) == NIS_NOTFOUND);

    int n = 0;
    CHECK(btree_scan(&bt, "k01", 3, 0, scan_cb, &n) == NIS_OK);
    CHECK(n == 100);
    n = 0;
    CHECK(btree_scan(&bt, "k", 1, 10, scan_cb, &n) == NIS_OK);
    CHECK(n == 10);
    n = 0;
    CHECK(btree_scan(&bt, "zzz", 3, 0, scan_cb, &n) == NIS_OK);
    CHECK(n == 0);
    btree_close(&bt);
    index_free(&ix);

    /* An empty index is a valid, empty tree. */
    index_init(&ix);
    CHECK(btree_build(p, &ix, 0, 0, 1) == NIS_OK);
    CHECK(btree_open(&bt, p) == NIS_OK);
    CHECK(btree_get(&bt, "x", 1, &st) == NIS_NOTFOUND);
    btree_close(&bt);
    index_free(&ix);
    remove(p);
}

int main(void)
{
    test_buf();
    test_varint();
    test_crc();
    test_codec();
    test_log();
    test_model();
    test_btree();

    printf("tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
