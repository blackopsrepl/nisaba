# Nisaba

A tiny append-only temporal/provenance key-value database in C.

The append-only record log is the source of truth. The current accepted value
of a key — the **Canon** — is a deterministic fold over that log, never a
stored mutable field. The on-disk index is a disposable cache: delete it and
the database rebuilds it from the log without losing a fact.

```text
nisaba> inscribe project.status "active"
1

nisaba> inscribe project.status "paused"
2

nisaba> canon project.status
"paused"

nisaba> history project.status
1  "active"
2  "paused"  supersedes=1
```

## Build

```sh
make            # builds libnisaba.a and the nisaba CLI
make test       # runs the unit and integration tests
```

## Use

Interactive:

```sh
$ nisaba memory.db
NISABA
The tablet remembers.
nisaba> inscribe user.name "Vittorio"
1
nisaba> canon user.name
"Vittorio"
```

One-shot, from a shell script or an agent:

```sh
nisaba memory.db inscribe project.status active
nisaba memory.db canon project.status
nisaba memory.db history project.status
nisaba memory.db scan project
nisaba --json memory.db canon project.status
nisaba --format=raw memory.db canon project.status
```

## Commands

```text
inscribe KEY VALUE [--witness W] [--source S]
                   [--supersedes ID] [--valid-time T] [--fork]
canon KEY [--as-of ID]
history KEY
scan PREFIX [--limit N]
retract ID
schisms KEY
witness ID
checkpoint | verify | init | stats
```

Global options: `--json` (one JSON object per result line), `--format=raw`
(values without quotes), `--readonly`, `--quiet`.

Exit codes: `0` ok, `1` not found / no live claim, `2` usage, `3` I/O,
`4` schism, `5` corrupt.

## Semantics

- Every write appends an immutable record carrying a log-sequence id (its
  inscription id and transaction time), a witness, and an optional
  supersession edge.
- A new claim supersedes the current single head automatically. `--fork`
  adds a competing head instead, which produces a **schism** (≥2 heads).
  `--supersedes ID` links to a specific predecessor.
- `canon` returns the unique live head's value, `null` when nothing is
  asserted, or reports the schism. Conflict is never silently resolved.
- `retract ID` withdraws a prior inscription; it does not resurrect what
  that inscription superseded.
- `--as-of N` evaluates Canon over the log prefix up to record N.
- Wall-clock time never decides Canon: only append order and explicit edges
  do, so replay is deterministic.

## Storage

```text
memory.db      append-only framed record log (source of truth)
memory.db.idx  paged B+tree index snapshot (disposable cache)
```

The log is a 64-byte header followed by records framed with magic, length,
lsn, a TLV body, and a CRC-32C trailer. Because records are only ever
appended, recovery has nothing to undo: a scan stops at the first invalid
frame and truncates the torn suffix, so a crash can only lose an incomplete
tail, never corrupt the prefix. `checkpoint` rewrites the index snapshot
atomically; a missing or stale index is simply rebuilt from the log.

## Layout

```text
include/nisaba.h   public API and record model
src/util.c         buffers, varints, CRC-32C
src/codec.c        TLV record body encoding
src/log.c          framed append-only log and recovery
src/index.c        in-memory ordered index
src/model.c        Canon fold over the supersession DAG
src/btree.c        paged B+tree index snapshot
src/db.c           archive open/inscribe/read API
src/main.c         command-line interface
tests/             unit and integration tests
```
