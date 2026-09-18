#include "internal.h"

#include <string.h>

void record_init(Record *r)
{
    memset(r, 0, sizeof *r);
    r->valid_time = NIS_VALID_TIME_UNSET;
    buf_init(&r->key);
    buf_init(&r->value);
    buf_init(&r->witness);
    buf_init(&r->source);
}

void record_free(Record *r)
{
    buf_free(&r->key);
    buf_free(&r->value);
    buf_free(&r->witness);
    buf_free(&r->source);
}

int record_copy(Record *dst, const Record *src)
{
    record_init(dst);
    dst->id = src->id;
    dst->supersedes = src->supersedes;
    dst->target = src->target;
    dst->offset = src->offset;
    dst->op = src->op;
    dst->fork = src->fork;
    dst->vtype = src->vtype;
    dst->valid_time = src->valid_time;
    dst->recorded_at = src->recorded_at;
    int rc = buf_set(&dst->key, src->key.data, src->key.len);
    if (rc == NIS_OK)
        rc = buf_set(&dst->value, src->value.data, src->value.len);
    if (rc == NIS_OK)
        rc = buf_set(&dst->witness, src->witness.data, src->witness.len);
    if (rc == NIS_OK)
        rc = buf_set(&dst->source, src->source.data, src->source.len);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Tag-length-value encoding                                          */
/* ------------------------------------------------------------------ */

enum {
    TAG_KEY         = 0x01,
    TAG_VALUE       = 0x02,
    TAG_VTYPE       = 0x03,
    TAG_WITNESS     = 0x04,
    TAG_SOURCE      = 0x05,
    TAG_SUPERSEDES  = 0x06,
    TAG_VALID_TIME  = 0x07,
    TAG_RECORDED_AT = 0x08,
    TAG_OP          = 0x09,
    TAG_TARGET      = 0x0a,
    TAG_FORK        = 0x0b,
};

static uint64_t zigzag(int64_t v)
{
    return ((uint64_t)v << 1) ^ (uint64_t)(v >> 63);
}

static int64_t unzigzag(uint64_t v)
{
    return (int64_t)(v >> 1) ^ -(int64_t)(v & 1);
}

static int tlv_bytes(Buf *out, uint8_t tag, const void *p, size_t n)
{
    uint8_t lenbuf[10];
    size_t ln = varint_put(lenbuf, n);
    int rc = buf_put_u8(out, tag);
    if (rc == NIS_OK)
        rc = buf_append(out, lenbuf, ln);
    if (rc == NIS_OK)
        rc = buf_append(out, p, n);
    return rc;
}

static int tlv_uint(Buf *out, uint8_t tag, uint64_t v)
{
    uint8_t tmp[10];
    size_t n = varint_put(tmp, v);
    return tlv_bytes(out, tag, tmp, n);
}

static int tlv_int(Buf *out, uint8_t tag, int64_t v)
{
    return tlv_uint(out, tag, zigzag(v));
}

int codec_encode_body(const Record *r, Buf *out)
{
    int rc;
    if (r->key.len && (rc = tlv_bytes(out, TAG_KEY, r->key.data, r->key.len)) != NIS_OK)
        return rc;
    if ((rc = tlv_uint(out, TAG_VTYPE, r->vtype)) != NIS_OK)
        return rc;
    if (r->vtype != VAL_NULL &&
        (rc = tlv_bytes(out, TAG_VALUE, r->value.data, r->value.len)) != NIS_OK)
        return rc;
    if (r->witness.len &&
        (rc = tlv_bytes(out, TAG_WITNESS, r->witness.data, r->witness.len)) != NIS_OK)
        return rc;
    if (r->source.len &&
        (rc = tlv_bytes(out, TAG_SOURCE, r->source.data, r->source.len)) != NIS_OK)
        return rc;
    if ((rc = tlv_uint(out, TAG_OP, r->op)) != NIS_OK)
        return rc;
    if ((rc = tlv_int(out, TAG_RECORDED_AT, r->recorded_at)) != NIS_OK)
        return rc;
    if (r->supersedes &&
        (rc = tlv_uint(out, TAG_SUPERSEDES, r->supersedes)) != NIS_OK)
        return rc;
    if (r->target &&
        (rc = tlv_uint(out, TAG_TARGET, r->target)) != NIS_OK)
        return rc;
    if (r->valid_time != NIS_VALID_TIME_UNSET &&
        (rc = tlv_int(out, TAG_VALID_TIME, r->valid_time)) != NIS_OK)
        return rc;
    if (r->fork &&
        (rc = tlv_uint(out, TAG_FORK, r->fork)) != NIS_OK)
        return rc;
    return NIS_OK;
}

int codec_decode_body(const uint8_t *p, size_t n, Record *r)
{
    record_init(r);
    size_t i = 0;
    while (i < n) {
        uint8_t tag = p[i++];
        uint64_t len = 0;
        size_t used = 0;
        if (varint_get(p + i, n - i, &len, &used) != NIS_OK)
            goto corrupt;
        i += used;
        if (len > n - i)
            goto corrupt;
        const uint8_t *val = p + i;
        switch (tag) {
        case TAG_KEY:
            if (buf_set(&r->key, val, (size_t)len) != NIS_OK)
                goto nomem;
            break;
        case TAG_VALUE:
            if (buf_set(&r->value, val, (size_t)len) != NIS_OK)
                goto nomem;
            break;
        case TAG_VTYPE: {
            uint64_t v;
            size_t u;
            if (varint_get(val, (size_t)len, &v, &u) != NIS_OK)
                goto corrupt;
            r->vtype = (uint8_t)v;
            break;
        }
        case TAG_WITNESS:
            if (buf_set(&r->witness, val, (size_t)len) != NIS_OK)
                goto nomem;
            break;
        case TAG_SOURCE:
            if (buf_set(&r->source, val, (size_t)len) != NIS_OK)
                goto nomem;
            break;
        case TAG_SUPERSEDES: {
            uint64_t v;
            size_t u;
            if (varint_get(val, (size_t)len, &v, &u) != NIS_OK)
                goto corrupt;
            r->supersedes = v;
            break;
        }
        case TAG_VALID_TIME: {
            uint64_t v;
            size_t u;
            if (varint_get(val, (size_t)len, &v, &u) != NIS_OK)
                goto corrupt;
            r->valid_time = unzigzag(v);
            break;
        }
        case TAG_RECORDED_AT: {
            uint64_t v;
            size_t u;
            if (varint_get(val, (size_t)len, &v, &u) != NIS_OK)
                goto corrupt;
            r->recorded_at = unzigzag(v);
            break;
        }
        case TAG_OP: {
            uint64_t v;
            size_t u;
            if (varint_get(val, (size_t)len, &v, &u) != NIS_OK)
                goto corrupt;
            r->op = (uint8_t)v;
            break;
        }
        case TAG_TARGET: {
            uint64_t v;
            size_t u;
            if (varint_get(val, (size_t)len, &v, &u) != NIS_OK)
                goto corrupt;
            r->target = v;
            break;
        }
        case TAG_FORK: {
            uint64_t v;
            size_t u;
            if (varint_get(val, (size_t)len, &v, &u) != NIS_OK)
                goto corrupt;
            r->fork = (uint8_t)v;
            break;
        }
        default:
            break; /* unknown tag: skip for forward compatibility */
        }
        i += (size_t)len;
    }
    return NIS_OK;

corrupt:
    record_free(r);
    return NIS_CORRUPT;
nomem:
    record_free(r);
    return NIS_NOMEM;
}
