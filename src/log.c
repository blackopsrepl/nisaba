#define _POSIX_C_SOURCE 200809L

#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static int read_at(FILE *fp, uint64_t off, void *dst, size_t n)
{
    if (fseeko(fp, (off_t)off, SEEK_SET) != 0)
        return NIS_IO;
    return fread(dst, 1, n, fp) == n ? NIS_OK : NIS_IO;
}

static int write_header(Log *l)
{
    uint8_t h[LOG_HEADER_SIZE];
    memset(h, 0, sizeof h);
    h[0] = 0x41; h[1] = 0x53; h[2] = 0x49; h[3] = 0x4e; /* "NISA" LE */
    h[4] = (uint8_t)(NISABA_VERSION & 0xff);
    h[5] = (uint8_t)(NISABA_VERSION >> 8);
    uint32_t crc = crc32c(0, h, LOG_HEADER_SIZE - 4);
    h[LOG_HEADER_SIZE - 4] = (uint8_t)(crc & 0xff);
    h[LOG_HEADER_SIZE - 3] = (uint8_t)((crc >> 8) & 0xff);
    h[LOG_HEADER_SIZE - 2] = (uint8_t)((crc >> 16) & 0xff);
    h[LOG_HEADER_SIZE - 1] = (uint8_t)((crc >> 24) & 0xff);
    if (fseeko(l->fp, 0, SEEK_SET) != 0)
        return NIS_IO;
    if (fwrite(h, 1, sizeof h, l->fp) != sizeof h)
        return NIS_IO;
    return NIS_OK;
}

static int sync_file(FILE *fp)
{
    if (fflush(fp) != 0)
        return NIS_IO;
    if (fsync(fileno(fp)) != 0)
        return errno == EINVAL ? NIS_OK : NIS_IO;
    return NIS_OK;
}

int log_open(Log *l, const char *path, int create, int readonly)
{
    memset(l, 0, sizeof *l);
    l->readonly = readonly;
    l->path = strdup(path);
    if (!l->path)
        return NIS_NOMEM;

    const char *mode = readonly ? "rb" : "r+b";
    l->fp = fopen(path, mode);
    if (!l->fp && create && !readonly)
        l->fp = fopen(path, "w+b");
    if (!l->fp) {
        free(l->path);
        l->path = NULL;
        return NIS_IO;
    }

    struct stat st;
    if (fstat(fileno(l->fp), &st) != 0) {
        log_close(l);
        return NIS_IO;
    }

    if (st.st_size == 0) {
        if (!create || readonly) {
            log_close(l);
            return NIS_CORRUPT;
        }
        if (write_header(l) != NIS_OK || sync_file(l->fp) != NIS_OK) {
            log_close(l);
            return NIS_IO;
        }
        l->valid_len = LOG_HEADER_SIZE;
        l->next_lsn = 1;
        return NIS_OK;
    }

    if ((uint64_t)st.st_size < LOG_HEADER_SIZE) {
        log_close(l);
        return NIS_CORRUPT;
    }
    uint8_t h[LOG_HEADER_SIZE];
    if (read_at(l->fp, 0, h, sizeof h) != NIS_OK) {
        log_close(l);
        return NIS_IO;
    }
    uint32_t want = crc32c(0, h, LOG_HEADER_SIZE - 4);
    if (rd_u32(h) != NISABA_MAGIC || rd_u32(h + LOG_HEADER_SIZE - 4) != want) {
        log_close(l);
        return NIS_CORRUPT;
    }
    l->valid_len = (uint64_t)st.st_size;
    l->next_lsn = 1;
    return NIS_OK;
}

void log_close(Log *l)
{
    if (l->fp)
        fclose(l->fp);
    free(l->path);
    l->fp = NULL;
    l->path = NULL;
}

int log_append(Log *l, const Record *r, uint64_t *offset_out)
{
    if (l->readonly)
        return NIS_IO;

    Buf body;
    buf_init(&body);
    int rc = codec_encode_body(r, &body);
    if (rc != NIS_OK) {
        buf_free(&body);
        return rc;
    }

    uint32_t frame_len = (uint32_t)(FRAME_HEADER_SIZE + body.len + FRAME_TRAILER_SIZE);
    uint8_t *frame = malloc(frame_len);
    if (!frame) {
        buf_free(&body);
        return NIS_NOMEM;
    }
    frame[0] = 0x41;
    frame[1] = 0x53;
    frame[2] = 0x49;
    frame[3] = 0x4e;
    uint32_t fl = frame_len;
    frame[4] = (uint8_t)(fl & 0xff);
    frame[5] = (uint8_t)((fl >> 8) & 0xff);
    frame[6] = (uint8_t)((fl >> 16) & 0xff);
    frame[7] = (uint8_t)((fl >> 24) & 0xff);
    uint64_t lsn = r->id;
    for (int i = 0; i < 8; i++)
        frame[8 + i] = (uint8_t)((lsn >> (8 * i)) & 0xff);
    uint16_t flags = (r->op == RECORD_RETRACT) ? 1 : 0;
    frame[16] = (uint8_t)(flags & 0xff);
    frame[17] = (uint8_t)(flags >> 8);
    uint32_t bl = (uint32_t)body.len;
    frame[18] = (uint8_t)(bl & 0xff);
    frame[19] = (uint8_t)((bl >> 8) & 0xff);
    frame[20] = (uint8_t)((bl >> 16) & 0xff);
    frame[21] = (uint8_t)((bl >> 24) & 0xff);
    if (body.len)
        memcpy(frame + FRAME_HEADER_SIZE, body.data, body.len);
    uint32_t crc = crc32c(0, frame, FRAME_HEADER_SIZE + body.len);
    size_t coff = FRAME_HEADER_SIZE + body.len;
    frame[coff + 0] = (uint8_t)(crc & 0xff);
    frame[coff + 1] = (uint8_t)((crc >> 8) & 0xff);
    frame[coff + 2] = (uint8_t)((crc >> 16) & 0xff);
    frame[coff + 3] = (uint8_t)((crc >> 24) & 0xff);

    if (fseeko(l->fp, (off_t)l->valid_len, SEEK_SET) != 0) {
        free(frame);
        buf_free(&body);
        return NIS_IO;
    }
    int ok = fwrite(frame, 1, frame_len, l->fp) == frame_len;
    free(frame);
    buf_free(&body);
    if (!ok)
        return NIS_IO;
    if (sync_file(l->fp) != NIS_OK)
        return NIS_IO;

    if (offset_out)
        *offset_out = l->valid_len;
    l->valid_len += frame_len;
    l->next_lsn = lsn + 1;
    return NIS_OK;
}

int log_scan(Log *l, int (*cb)(void *ud, const Record *r, uint64_t off), void *ud)
{
    if (fseeko(l->fp, 0, SEEK_END) != 0)
        return NIS_IO;
    off_t end = ftello(l->fp);
    if (end < 0)
        return NIS_IO;
    uint64_t size = (uint64_t)end;

    uint64_t off = LOG_HEADER_SIZE;
    uint64_t last_lsn = 0;
    while (off < size) {
        if (size - off < FRAME_HEADER_SIZE)
            break;
        uint8_t hdr[FRAME_HEADER_SIZE];
        if (read_at(l->fp, off, hdr, sizeof hdr) != NIS_OK)
            break;
        if (rd_u32(hdr) != NISABA_MAGIC)
            break;
        uint32_t frame_len = rd_u32(hdr + 4);
        uint32_t body_len = rd_u32(hdr + 18);
        if (frame_len != FRAME_HEADER_SIZE + body_len + FRAME_TRAILER_SIZE)
            break;
        if (body_len > frame_len || (uint64_t)frame_len > size - off)
            break;

        Buf frame;
        buf_init(&frame);
        if (buf_append(&frame, hdr, sizeof hdr) != NIS_OK) {
            buf_free(&frame);
            return NIS_NOMEM;
        }
        /* read body + crc in one go */
        uint8_t *tail = malloc(body_len + FRAME_TRAILER_SIZE);
        if (!tail) {
            buf_free(&frame);
            return NIS_NOMEM;
        }
        if (read_at(l->fp, off + FRAME_HEADER_SIZE, tail,
                    body_len + FRAME_TRAILER_SIZE) != NIS_OK) {
            free(tail);
            buf_free(&frame);
            break;
        }
        if (buf_append(&frame, tail, body_len + FRAME_TRAILER_SIZE) != NIS_OK) {
            free(tail);
            buf_free(&frame);
            return NIS_NOMEM;
        }
        free(tail);

        uint32_t stored = rd_u32(frame.data + FRAME_HEADER_SIZE + body_len);
        uint32_t calc = crc32c(0, frame.data, FRAME_HEADER_SIZE + body_len);
        if (stored != calc) {
            buf_free(&frame);
            break;
        }

        Record r;
        if (codec_decode_body(frame.data + FRAME_HEADER_SIZE, body_len, &r) != NIS_OK) {
            buf_free(&frame);
            break;
        }
        r.id = rd_u64(hdr + 8);
        r.offset = off;
        if (r.id > last_lsn)
            last_lsn = r.id;
        if (cb && cb(ud, &r, off) != NIS_OK) {
            record_free(&r);
            buf_free(&frame);
            return NIS_ERR;
        }
        record_free(&r);
        buf_free(&frame);

        off += frame_len;
    }

    if (last_lsn + 1 > l->next_lsn)
        l->next_lsn = last_lsn + 1;
    l->valid_len = off;
    if (off < size && !l->readonly) {
        if (ftruncate(fileno(l->fp), (off_t)off) != 0)
            return NIS_IO;
        if (sync_file(l->fp) != NIS_OK)
            return NIS_IO;
    }
    return NIS_OK;
}

int log_read_record(Log *l, uint64_t offset, Record *out)
{
    uint8_t hdr[FRAME_HEADER_SIZE];
    if (read_at(l->fp, offset, hdr, sizeof hdr) != NIS_OK)
        return NIS_IO;
    if (rd_u32(hdr) != NISABA_MAGIC)
        return NIS_CORRUPT;
    uint32_t frame_len = rd_u32(hdr + 4);
    uint32_t body_len = rd_u32(hdr + 18);
    if (frame_len != FRAME_HEADER_SIZE + body_len + FRAME_TRAILER_SIZE)
        return NIS_CORRUPT;
    uint8_t *buf = malloc(body_len);
    if (!buf && body_len)
        return NIS_NOMEM;
    if (body_len && read_at(l->fp, offset + FRAME_HEADER_SIZE, buf, body_len) != NIS_OK) {
        free(buf);
        return NIS_IO;
    }
    int rc = codec_decode_body(buf, body_len, out);
    free(buf);
    if (rc != NIS_OK)
        return rc;
    out->id = rd_u64(hdr + 8);
    out->offset = offset;
    return NIS_OK;
}
