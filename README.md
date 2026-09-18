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

## Schisms: competing claims are never hidden

Most databases silently resolve a conflict by overwriting: last write wins.
Nisaba refuses to do that. A new write normally supersedes the current head,
but `--fork` records a *competing* claim instead. A key with two or more live
heads is a **schism**, and every read of that key reports it rather than
guessing:

```text
nisaba> inscribe project.owner "alice"
1

nisaba> inscribe project.owner "bob" --fork
2

nisaba> canon project.owner
schism: 2 competing claims
$ echo $?
4

nisaba> schisms project.owner
1  "alice"
2  "bob"

nisaba> history project.owner
1  "alice"
2  "bob"
```

`canon` returns exit code `4` and prints no value, so a script or agent cannot
mistake a schism for one. The diagnostic goes to stderr; stdout carries only
the machine-readable form. With `--json` that is an explicit `"schism":true`
object:

```console
$ nisaba --json memory.db canon project.owner 2>/dev/null
{"key":"project.owner","value":null,"schism":true,"heads":2}
$ echo $?
4
```

To resolve a schism you must get back to exactly one live head. Every path
appends records; none rewrites the log. A single `--supersedes` edge removes
only the one head it names, so it cannot merge two heads by itself:

```text
# Simplest: withdraw the losing claim. One head remains.
nisaba> retract 1
3

nisaba> canon project.owner
"bob"

# Or install a new preferred claim over one head, then withdraw the other.
nisaba> inscribe project.owner "bob" --supersedes 1
3

nisaba> canon project.owner
schism: 2 competing claims   # heads are now 2 and 3

nisaba> retract 2
4

nisaba> canon project.owner
"bob"
```

The general rule: keep only one path of live claims. Superseding a head
creates a new one, so pair each `--supersedes` with a `retract` of the head
it does not cover, or retract down to a single head and re-assert.

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

The rules are few and deterministic. Everything else follows from them.

- **Append only.** Every write appends an immutable record carrying a
  log-sequence id (its inscription id and its transaction time), a witness,
  and an optional supersession edge. Nothing is ever updated in place.
- **Canon is derived, not stored.** The current value of a key is folded from
  the log on demand (and cached in the index, which is disposable).
- **Supersession is a DAG.** A new claim replaces the current single head.
  `--supersedes ID` links to a specific predecessor. `--fork` adds a second
  live head instead of replacing, which is what creates a schism.
- **`canon` is three-valued.** The unique live head's value; `null` when no
  claim is live; a reported schism when two or more heads compete.
- **Exit codes are part of the contract.** `canon` exits `4` on a schism and
  `1` when a key has no live claim, so the ambiguity is visible to scripts.
- **Conflict is never silently resolved.** Nisaba will not pick a winner.
  Resolution is an explicit `inscribe --supersedes` or `retract`.

## Peculiarities and sharp edges

These are deliberate choices. Each one is a place where Nisaba differs from
"just overwrite the value."

1. **Newest-by-wall-clock never wins.** Canon depends only on the append
   order (`lsn`) and explicit `supersedes` edges. Timestamps are recorded,
   but they never decide anything, so replaying a log always yields the same
   answer even under clock skew or equal timestamps.

2. **`recorded_at` is informational.** It defaults to the wall clock at
   append but has no authority. `--valid-time T` stores a caller-supplied
   validity time; it is data for the caller, not an input to Canon.

3. **Retraction does not resurrect.** `retract ID` withdraws that record. It
   does *not* restore what the record superseded. If the retracted record was
   the only live head, the key becomes `null` (exit `1`), not its predecessor.

4. **Retraction wraps an existing record.** `retract ID` fails with "not
   found" if `ID` does not exist. It appends a new record (with a new id)
   whose target is the named one.

5. **A retraction is indexed under its target's key.** Even though the
   tombstone record is its own frame with its own id, it participates in the
   key's chain, so `history KEY` shows both the claim and its withdrawal.

6. **Forking an already-forked key stays forked.** When a key already has two
   or more heads, a plain `inscribe` does not quietly pick a winner.

7. **A `--supersedes` edge removes exactly one head.** It cannot merge two
   competing heads into one. Resolving a two-head schism needs either a
   `retract` of the losing head, or a `--supersedes` of one head plus a
   `retract` of the other. Superseding a head *adds* a new head (the new
   record), so it only reduces the head count when it retires the named one.

8. **`schisms` with no competing heads is empty, not an error.** On a key
   with one head or none, `schisms KEY` succeeds and prints nothing. It
   errors only when the key itself is unknown.

9. **Prefix scan is a byte range, not a hierarchy.** There are no namespaces
   or shards. `scan project.` matches keys whose bytes begin with `project.`.
   The entire key space is one ordered byte space in one log.

10. **The index is a cache you may delete.** `rm memory.db.idx` loses nothing:
    the next open rebuilds it from the log. Reads prefer the B+tree only when
    its snapshot is exactly caught up with the log, so a stale index can never
    change an answer.

11. **Only compaction removes bytes.** Absent `compact`, no operation ever
    destroys a recorded fact, including retraction, which is itself a record.
    This is the headline guarantee.

12. **A crash can only lose a tail.** Recovery truncates an incomplete final
    frame. It never corrupts or rewrites earlier records, because none were
    written in place.

13. **Themed names are CLI-only.** "Inscription", "Canon", "witness", and
    "schism" appear in the command surface. The storage code and headers use
    ordinary names (`Record`, `Log`, `Index`, `BTree`, `KeyState`), and the
    file format is self-describing TLV, so the theme never constrains it.

14. **Canon reads are O(chain).** Resolving a key walks that key's records;
    the index gives the chain and its head, but `--as-of` folds the prefix.

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
