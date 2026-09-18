#include "nisaba.h"

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

int main(void)
{
    test_buf();
    test_varint();
    test_crc();

    printf("tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
