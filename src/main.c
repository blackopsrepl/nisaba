#define _POSIX_C_SOURCE 200809L

#include "nisaba.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const char *dbpath;
    int json;
    int raw;
    int quiet;
    int readonly;
    int no_index;
} Config;

static int rc_to_exit(int rc)
{
    switch (rc) {
    case NIS_OK:       return 0;
    case NIS_NOTFOUND: return 1;
    case NIS_USAGE:    return 2;
    case NIS_SCHISM:   return 4;
    case NIS_CORRUPT:  return 5;
    case NIS_IO:       return 3;
    default:           return 3;
    }
}

static void json_string(FILE *f, const uint8_t *s, size_t n)
{
    fputc('"', f);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        switch (c) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (c < 0x20)
                fprintf(f, "\\u%04x", c);
            else
                fputc(c, f);
        }
    }
    fputc('"', f);
}

static void human_string(FILE *f, const uint8_t *s, size_t n)
{
    fputc('"', f);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == '"' || c == '\\')
            fputc('\\', f);
        if (c < 0x20)
            fprintf(f, "\\x%02x", c);
        else
            fputc(c, f);
    }
    fputc('"', f);
}

static void print_value(FILE *f, const Record *r, int json, int raw)
{
    if (r->vtype == VAL_NULL) {
        fputs("null", f);
        return;
    }
    if (raw) {
        fwrite(r->value.data, 1, r->value.len, f);
        return;
    }
    if (json && r->vtype != VAL_INT && r->vtype != VAL_DOUBLE)
        json_string(f, r->value.data, r->value.len);
    else if (!json && r->vtype == VAL_STR)
        human_string(f, r->value.data, r->value.len);
    else
        fwrite(r->value.data, 1, r->value.len, f);
}

/* --- tokenizer: whitespace separated, double quotes with backslash --- */

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/*
 * Split a line into whitespace-separated tokens. Double quotes group a token
 * and honor backslash escapes. Tokenizing copies each token down into the
 * space already consumed, so the write cursor never overtakes the read
 * cursor; separators are consumed before the terminator is written.
 */
static int tokenize(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (is_space(*p))
            p++;
        if (!*p)
            break;
        char *out = p;
        char *w = p;
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) {
                    p++;
                    *w++ = *p++;
                } else {
                    *w++ = *p++;
                }
            }
            if (*p == '"')
                p++;
        } else {
            while (*p && !is_space(*p))
                *w++ = *p++;
        }
        /* Consume the separator(s) before writing the terminator, so the
         * terminator does not erase the start of the next token. */
        while (is_space(*p))
            p++;
        *w = '\0';
        argv[argc++] = out;
    }
    return argc;
}

static int infer_type(const char *v)
{
    if (!*v)
        return VAL_STR;
    const char *p = v;
    if (*p == '-' || *p == '+')
        p++;
    if (!*p)
        return VAL_STR;
    for (; *p; p++)
        if (!isdigit((unsigned char)*p))
            return VAL_STR;
    return VAL_INT;
}

/* --- commands ------------------------------------------------------ */

static int cmd_inscribe(Nisaba *db, int argc, char **argv, const Config *cfg)
{
    if (argc < 3)
        return NIS_USAGE;
    Record r;
    record_init(&r);
    r.op = RECORD_PUT;
    const char *value = argv[2];
    r.vtype = (uint8_t)infer_type(value);
    buf_set(&r.key, argv[1], strlen(argv[1]));
    buf_set(&r.value, value, strlen(value));

    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--fork")) {
            r.fork = 1;
        } else if (!strcmp(argv[i], "--witness")) {
            if (++i >= argc) goto bad;
            buf_set(&r.witness, argv[i], strlen(argv[i]));
        } else if (!strncmp(argv[i], "--witness=", 10)) {
            buf_set(&r.witness, argv[i] + 10, strlen(argv[i] + 10));
        } else if (!strcmp(argv[i], "--source")) {
            if (++i >= argc) goto bad;
            buf_set(&r.source, argv[i], strlen(argv[i]));
        } else if (!strncmp(argv[i], "--source=", 9)) {
            buf_set(&r.source, argv[i] + 9, strlen(argv[i] + 9));
        } else if (!strcmp(argv[i], "--supersedes")) {
            if (++i >= argc) goto bad;
            r.supersedes = strtoull(argv[i], NULL, 10);
        } else if (!strncmp(argv[i], "--supersedes=", 13)) {
            r.supersedes = strtoull(argv[i] + 13, NULL, 10);
        } else if (!strcmp(argv[i], "--valid-time")) {
            if (++i >= argc) goto bad;
            r.valid_time = (int64_t)strtoll(argv[i], NULL, 10);
        } else if (!strncmp(argv[i], "--valid-time=", 13)) {
            r.valid_time = (int64_t)strtoll(argv[i] + 13, NULL, 10);
        } else {
            goto bad;
        }
    }
    goto ok;
bad:
    record_free(&r);
    return NIS_USAGE;
ok:

    uint64_t id = 0;
    int rc = nis_inscribe(db, &r, &id);
    record_free(&r);
    if (rc != NIS_OK)
        return rc;
    if (cfg->json)
        printf("{\"id\":%llu}\n", (unsigned long long)id);
    else
        printf("%llu\n", (unsigned long long)id);
    return NIS_OK;
}

static int cmd_canon(Nisaba *db, int argc, char **argv, const Config *cfg)
{
    if (argc < 2)
        return NIS_USAGE;
    uint64_t asof = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--as-of")) {
            if (++i >= argc)
                return NIS_USAGE;
            asof = strtoull(argv[i], NULL, 10);
        } else if (!strncmp(argv[i], "--as-of=", 8)) {
            asof = strtoull(argv[i] + 8, NULL, 10);
        } else {
            return NIS_USAGE;
        }
    }
    const void *key = argv[1];
    size_t klen = strlen(argv[1]);
    KeyState st;
    const Record *head = NULL;
    int rc = asof ? nis_canon_asof(db, key, klen, asof, &st, &head)
                  : nis_canon(db, key, klen, &st, &head);
    if (rc == NIS_NOTFOUND) {
        if (cfg->json) {
            printf("{\"key\":");
            json_string(stdout, (const uint8_t *)key, klen);
            printf(",\"value\":null}\n");
        } else {
            printf("null\n");
        }
        return NIS_NOTFOUND;
    }
    if (rc == NIS_SCHISM) {
        if (cfg->json) {
            printf("{\"key\":");
            json_string(stdout, (const uint8_t *)key, klen);
            printf(",\"value\":null,\"schism\":true,\"heads\":%u}\n", st.head_count);
        } else {
            fprintf(stderr, "schism: %u competing claims\n", st.head_count);
        }
        return NIS_SCHISM;
    }
    if (rc != NIS_OK)
        return rc;
    if (cfg->json) {
        printf("{\"key\":");
        json_string(stdout, (const uint8_t *)key, klen);
        printf(",\"id\":%llu,\"value\":",
               (unsigned long long)head->id);
        print_value(stdout, head, 1, 0);
        printf(",\"witness\":");
        json_string(stdout, head->witness.data, head->witness.len);
        printf("}\n");
    } else {
        print_value(stdout, head, 0, cfg->raw);
        putchar('\n');
    }
    return NIS_OK;
}

static int history_cb_print(void *ud, const Record *r)
{
    const Config *cfg = ud;
    if (cfg->json) {
        printf("{\"id\":%llu,\"op\":%s,\"key\":",
               (unsigned long long)r->id,
               r->op == RECORD_RETRACT ? "\"retract\"" : "\"put\"");
        json_string(stdout, r->key.data, r->key.len);
        printf(",\"value\":");
        print_value(stdout, r, 1, 0);
        printf(",\"supersedes\":%llu,\"retracts\":%llu,\"witness\":",
               (unsigned long long)r->supersedes,
               (unsigned long long)r->target);
        json_string(stdout, r->witness.data, r->witness.len);
        printf("}\n");
    } else if (r->op == RECORD_RETRACT) {
        printf("%llu  retract(%llu)\n", (unsigned long long)r->id,
               (unsigned long long)r->target);
    } else {
        printf("%llu  ", (unsigned long long)r->id);
        print_value(stdout, r, 0, cfg->raw);
        if (r->supersedes)
            printf("  supersedes=%llu", (unsigned long long)r->supersedes);
        putchar('\n');
    }
    return NIS_OK;
}

static int cmd_history(Nisaba *db, int argc, char **argv, const Config *cfg)
{
    if (argc < 2)
        return NIS_USAGE;
    return nis_history(db, argv[1], strlen(argv[1]), history_cb_print, (void *)cfg);
}

static int scan_cb_print(void *ud, const Buf *key, const KeyState *st)
{
    Nisaba *db = ((void **)ud)[0];
    const Config *cfg = ((void **)ud)[1];
    const Record *head = nis_record(db, st->head_lsn);
    if (cfg->json) {
        printf("{\"key\":");
        json_string(stdout, key->data, key->len);
        printf(",\"id\":%llu,\"value\":",
               (unsigned long long)(head ? head->id : 0));
        if (head)
            print_value(stdout, head, 1, 0);
        else
            fputs("null", stdout);
        printf("}\n");
    } else {
        fwrite(key->data, 1, key->len, stdout);
        printf("  ");
        if (head)
            print_value(stdout, head, 0, cfg->raw);
        else
            fputs("null", stdout);
        putchar('\n');
    }
    return NIS_OK;
}

static int cmd_scan(Nisaba *db, int argc, char **argv, const Config *cfg)
{
    const char *prefix = argc >= 2 ? argv[1] : "";
    size_t plen = strlen(prefix);
    size_t limit = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--limit")) {
            if (++i >= argc)
                return NIS_USAGE;
            limit = (size_t)strtoull(argv[i], NULL, 10);
        } else if (!strncmp(argv[i], "--limit=", 8)) {
            limit = (size_t)strtoull(argv[i] + 8, NULL, 10);
        } else {
            return NIS_USAGE;
        }
    }
    void *ud[2] = { db, (void *)cfg };
    return nis_scan(db, prefix, plen, limit, scan_cb_print, ud);
}

static int cmd_retract(Nisaba *db, int argc, char **argv, const Config *cfg)
{
    if (argc < 2)
        return NIS_USAGE;
    uint64_t id = strtoull(argv[1], NULL, 10);
    uint64_t new_id = 0;
    int rc = nis_retract(db, id, &new_id);
    if (rc == NIS_OK) {
        if (cfg->json)
            printf("{\"id\":%llu,\"retracts\":%llu}\n",
                   (unsigned long long)new_id, (unsigned long long)id);
        else
            printf("%llu\n", (unsigned long long)new_id);
    }
    return rc;
}

static int cmd_schisms(Nisaba *db, int argc, char **argv, const Config *cfg)
{
    if (argc < 2)
        return NIS_USAGE;
    return nis_schisms(db, argv[1], strlen(argv[1]), history_cb_print, (void *)cfg);
}

static int cmd_witness(Nisaba *db, int argc, char **argv, const Config *cfg)
{
    if (argc < 2)
        return NIS_USAGE;
    uint64_t id = strtoull(argv[1], NULL, 10);
    const Record *r = nis_record(db, id);
    if (!r)
        return NIS_NOTFOUND;
    if (cfg->json) {
        printf("{\"id\":%llu,\"witness\":", (unsigned long long)r->id);
        json_string(stdout, r->witness.data, r->witness.len);
        printf(",\"source\":");
        json_string(stdout, r->source.data, r->source.len);
        printf(",\"recorded_at\":%lld}\n", (long long)r->recorded_at);
    } else {
        printf("id       %llu\n", (unsigned long long)r->id);
        printf("witness  ");
        fwrite(r->witness.data, 1, r->witness.len, stdout);
        putchar('\n');
        printf("source   ");
        fwrite(r->source.data, 1, r->source.len, stdout);
        putchar('\n');
        printf("recorded %lld\n", (long long)r->recorded_at);
    }
    return NIS_OK;
}

static int cmd_stats(Nisaba *db, const Config *cfg)
{
    if (cfg->json) {
        printf("{\"records\":%zu,\"next_id\":%llu,\"index\":",
               nis_record_count(db), (unsigned long long)nis_next_id(db));
        json_string(stdout, (const uint8_t *)nis_index_path(db),
                    strlen(nis_index_path(db)));
        printf("}\n");
    } else {
        printf("records   %zu\n", nis_record_count(db));
        printf("next id   %llu\n", (unsigned long long)nis_next_id(db));
        printf("index     %s\n", nis_index_path(db));
    }
    return NIS_OK;
}

static int dispatch(Nisaba *db, int argc, char **argv, const Config *cfg)
{
    if (!strcmp(argv[0], "inscribe") || !strcmp(argv[0], "put"))
        return cmd_inscribe(db, argc, argv, cfg);
    if (!strcmp(argv[0], "canon"))
        return cmd_canon(db, argc, argv, cfg);
    if (!strcmp(argv[0], "history"))
        return cmd_history(db, argc, argv, cfg);
    if (!strcmp(argv[0], "scan"))
        return cmd_scan(db, argc, argv, cfg);
    if (!strcmp(argv[0], "retract"))
        return cmd_retract(db, argc, argv, cfg);
    if (!strcmp(argv[0], "schisms"))
        return cmd_schisms(db, argc, argv, cfg);
    if (!strcmp(argv[0], "witness"))
        return cmd_witness(db, argc, argv, cfg);
    if (!strcmp(argv[0], "checkpoint"))
        return nis_checkpoint(db);
    if (!strcmp(argv[0], "verify"))
        return nis_verify(db);
    if (!strcmp(argv[0], "stats"))
        return cmd_stats(db, cfg);
    return NIS_USAGE;
}

static int is_write_cmd(const char *cmd)
{
    return !strcmp(cmd, "inscribe") || !strcmp(cmd, "put") ||
           !strcmp(cmd, "retract") || !strcmp(cmd, "init") ||
           !strcmp(cmd, "checkpoint") || !strcmp(cmd, "compact");
}

static void usage(FILE *f)
{
    fputs(
        "usage: nisaba [options] DB [command [args...]]\n"
        "\n"
        "options:\n"
        "  --json              emit one JSON object per result line\n"
        "  --format=raw        print values without quotes\n"
        "  --readonly          never write to the archive\n"
        "  --quiet             suppress the interactive banner\n"
        "  --help              show this message\n"
        "\n"
        "commands:\n"
        "  inscribe KEY VALUE [--witness W] [--source S]\n"
        "                     [--supersedes ID] [--valid-time T] [--fork]\n"
        "  canon KEY [--as-of ID]\n"
        "  history KEY\n"
        "  scan PREFIX [--limit N]\n"
        "  retract ID\n"
        "  schisms KEY\n"
        "  witness ID\n"
        "  checkpoint | verify | init | stats\n",
        f);
}

int main(int argc, char **argv)
{
    Config cfg = {0};
    char *cmd[64];
    int ncmd = 0;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!cfg.dbpath && !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        } else if (!cfg.dbpath && !strcmp(a, "--json")) {
            cfg.json = 1;
        } else if (!cfg.dbpath && !strcmp(a, "--format=raw")) {
            cfg.raw = 1;
        } else if (!cfg.dbpath && !strcmp(a, "--readonly")) {
            cfg.readonly = 1;
        } else if (!cfg.dbpath && !strcmp(a, "--no-index")) {
            cfg.no_index = 1;
        } else if (!cfg.dbpath && !strcmp(a, "--quiet")) {
            cfg.quiet = 1;
        } else if (!cfg.dbpath && a[0] == '-' && a[1] == '-') {
            fprintf(stderr, "nisaba: unknown option %s\n", a);
            usage(stderr);
            return 2;
        } else if (!cfg.dbpath) {
            cfg.dbpath = a;
        } else if (ncmd < (int)(sizeof cmd / sizeof cmd[0])) {
            cmd[ncmd++] = a;
        }
    }

    if (!cfg.dbpath) {
        usage(stderr);
        return 2;
    }

    int create = cfg.readonly ? 0 : 1;
    if (ncmd > 0 && !is_write_cmd(cmd[0]))
        create = 0;

    Nisaba *db = NULL;
    int rc = nis_open(cfg.dbpath, create, &db);
    if (rc != NIS_OK) {
        fprintf(stderr, "nisaba: cannot open %s: %s\n", cfg.dbpath, nis_strerror(rc));
        return rc_to_exit(rc);
    }

    if (ncmd > 0) {
        if (!strcmp(cmd[0], "init"))
            rc = NIS_OK;
        else
            rc = dispatch(db, ncmd, cmd, &cfg);
        if (rc != NIS_OK && !(cfg.json)) {
            const char *msg = nis_strerror(rc);
            if (rc != NIS_NOTFOUND)
                fprintf(stderr, "nisaba: %s\n", msg);
        }
        if (rc == NIS_USAGE) {
            usage(stderr);
        }
        nis_close(db);
        return rc_to_exit(rc);
    }

    /* Interactive */
    if (!cfg.quiet && isatty(STDIN_FILENO)) {
        printf("NISABA\n");
        printf("The tablet remembers.\n");
    }
    char line[8192];
    while (1) {
        if (!cfg.quiet && isatty(STDIN_FILENO)) {
            fputs("nisaba> ", stdout);
            fflush(stdout);
        }
        if (!fgets(line, sizeof line, stdin))
            break;
        char *toks[64];
        int n = tokenize(line, toks, 64);
        if (n == 0)
            continue;
        if (!strcmp(toks[0], "quit") || !strcmp(toks[0], "exit"))
            break;
        int r = dispatch(db, n, toks, &cfg);
        if (r != NIS_OK && r != NIS_NOTFOUND && !cfg.json)
            fprintf(stderr, "nisaba: %s\n", nis_strerror(r));
    }
    nis_close(db);
    return 0;
}
