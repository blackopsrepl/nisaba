#include "nisaba.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void)
{
    test_buf();
    test_varint();
    test_crc();
    test_codec();

    printf("tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
