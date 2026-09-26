---
name: nisaba
description: Drive Nisaba, the append-only temporal/provenance key-value store, through its CLI — inscribe/canon/history/scan/retract, schism detection and resolution, --as-of time travel, witness/source provenance, --json mode, and the exit-code contract — plus building and testing the nisaba repository. Use whenever reading or writing nisaba .db archives or changing this repo's code. Use ONLY for nisaba; not for other databases.
---

# Nisaba — Agent Control Skill

Nisaba is a tiny append-only key-value database: the record log on disk is the
source of truth, the current value of a key ("Canon") is a deterministic fold
over that log, and the on-disk index is a disposable cache. Nothing is ever
mutated in place. Wielding it well means trusting the exit codes and never
assuming a read resolved a conflict for you.

## Entry points

| Need | Command |
| --- | --- |
| One-shot CLI (agents should always use this) | `./nisaba [options] DB command [args...]` |
| Batch via stdin | `printf 'canon k\n' | ./nisaba --quiet DB` |
| Interactive REPL (humans) | `./nisaba DB` |
| Build binary + library | `make` |
| Test suite | `make test` |
| C API | `include/nisaba.h` |

All examples below abbreviate `./nisaba` to `nisaba`.

## Data isolation (always do this)

Read commands never create a file; write commands auto-create. Experiments
belong in a scratch directory, never in a database you did not create:

```sh
d=$(mktemp -d)
nisaba "$d/t.db" inscribe k v     # creates $d/t.db (and $d/t.db.idx on checkpoint)
```

`nisaba missing.db canon k` fails with exit 3 and creates nothing.

## Golden rules

1. **stdout is data, stderr is diagnostics, exit codes are the contract.**
   Parse stdout; branch on exit code. Never scrape the diagnostics.
2. **Nothing is ever overwritten.** `inscribe` and `retract` only append
   records with dense ids starting at 1.
3. **Newest timestamp never wins.** Canon depends only on append order and
   explicit `supersedes` edges. `recorded_at` and `--valid-time` are
   informational only.
4. **Canon is three-valued**: a value (exit 0), `null` / no live claim
   (exit 1), or a schism (exit 4). Check `$?` before trusting canon output.
5. **Conflict is never silently resolved.** If a key has two live heads, every
   read reports it; resolution is an explicit `retract` or
   `inscribe --supersedes`.

## Invocation

Global options must come **before** the DB path; after it they are a usage
error (exit 2).

```sh
nisaba --json DB canon k          # right
nisaba DB canon k --format=raw    # WRONG: usage error, exit 2
```

| Option | Effect |
| --- | --- |
| `--json` | One JSON object per result line on stdout |
| `--format=raw` | Values without quotes (human mode) |
| `--readonly` | **Only** prevents creating a missing file — see sharp edges |
| `--no-index` | Do not load or create the `.idx` snapshot |
| `--quiet` | Suppress the interactive banner |
| `--help` | Usage text |

## Command reference

```text
inscribe KEY VALUE [--witness W] [--source S] [--supersedes ID]
                   [--valid-time T] [--fork]      (alias: put)
canon KEY [--as-of ID]
history KEY
scan PREFIX [--limit N]
retract ID
schisms KEY
witness ID
checkpoint | verify | init | stats
```

- `inscribe` prints the new record id; every option also accepts an `=` form
  (`--witness=...`, `--supersedes=7`, ...).
- `canon` prints the value (strings quoted, ints bare) or `null`.
- `history` prints the key's full chain, oldest first; retractions render as
  `ID  retract(TARGET)`, claims as `ID  "value"  [supersedes=N]`.
- `scan` is an ordered byte-prefix range over one flat keyspace; an empty
  prefix lists every key as `key  value`.
- `schisms KEY` lists the competing heads; empty output with exit 0 means no
  schism, exit 1 means the key does not exist.
- `witness ID` prints the provenance of any record: witness, source, and
  `recorded` (unix seconds, informational).
- `init` creates the archive; on an existing one it is a no-op.
- `checkpoint` rewrites the index snapshot atomically (prints nothing).
- `verify` validates the **index snapshot only** — run `checkpoint` first;
  with no snapshot on disk it fails with exit 3.
- `stats` prints record count, next id, and index path.
- `quit`/`exit` leave the interactive REPL only.

## Exit codes

| Code | Meaning | Typical source |
| --- | --- | --- |
| 0 | ok | everything |
| 1 | not found / no live claim | `canon` on absent or fully retracted key, `retract` of missing id, `schisms` of unknown key |
| 2 | usage error | unknown command, bad flags, options after DB path |
| 3 | I/O error | cannot open DB, `verify` without a snapshot |
| 4 | schism | `canon` with 2+ live heads |
| 5 | corrupt | unrecoverable frame damage |
| 6 | internal | out of memory or another engine condition (`NIS_ERR`, `NIS_EXISTS`, `NIS_BUSY`) the command surface does not name — the diagnostic on stderr says which |

Every result code is mapped explicitly; nothing falls back to a code that
would mislabel the failure.

## JSON contract

Shapes as emitted by `--json` (verified against the binary):

```text
inscribe  {"id":1}
canon ok  {"key":"owner","id":10,"value":"carol","witness":""}
canon null  {"key":"nope","value":null}                          (exit 1)
canon schism {"key":"owner","value":null,"schism":true,"heads":2} (exit 4)
history/schisms  {"id":8,"op":"put","key":"n","value":42,"supersedes":0,"retracts":0,"witness":""}
                 {"id":9,"op":"retract","key":"owner","value":null,"supersedes":0,"retracts":6,"witness":""}
scan  {"key":"n","id":5,"value":43}
retract  {"id":9,"retracts":6}
witness  {"id":10,"witness":"alice ok","source":"review #12","recorded_at":1790446304}
stats  {"records":12,"next_id":13,"index":"t.db.idx"}
```

`init`, `checkpoint`, and `verify` print nothing. In `history`, `retracts` is
the retraction target (0 on claims); `supersedes` is the supersession edge
(0 when none). In `scan`, a headless or schismatic key shows `"id":0,
"value":null` with **no** schism flag — confirm with `canon` + exit code.

## Value typing and quoting

- The CLI types a value as int when it is an optional sign followed by digits
  only; anything else (including empty) is a string. Bool, double, bytes, and
  null exist only through the C API.
- Ints print bare; strings print quoted in human mode, unquoted with
  `--format=raw`.
- Keys and values with spaces need double quotes (`"multi word value"`); the
  tokenizer honors backslash escapes.
- Leading-zero digit strings (e.g. `007`) stay int-typed and are emitted as
  raw bytes in `--json` (`"value":007`, invalid JSON). Use `--format=raw` for
  such values.

## Schisms: detect, then resolve to one head

```sh
nisaba t.db inscribe owner alice
nisaba t.db inscribe owner bob --fork     # competing claim, not an overwrite
nisaba t.db canon owner; echo $?          # stderr "schism: 2 competing claims", exit 4
nisaba --json t.db canon owner            # {"key":"owner","value":null,"schism":true,"heads":2}
nisaba t.db schisms owner                 # 6  "alice" / 7  "bob"
```

Every resolution appends records; none rewrites the log:

```sh
# Recipe A — withdraw the losing head. One head remains.
nisaba t.db retract 7

# Recipe B — supersede one head, then retract the other.
nisaba t.db inscribe owner carol --supersedes 6   # now heads: 7 and 8
nisaba t.db retract 7
```

Invariants:

- A plain `inscribe` auto-links to the current head **only when there is
  exactly one**. On a forked key it adds another head (heads=3) instead of
  picking a winner.
- `--supersedes ID` removes exactly the one head it names. It can never merge
  two heads by itself, and it disables the auto-link.
- `retract ID` does **not** resurrect what `ID` superseded. Retracting the
  only live head leaves the key `null` (exit 1), not its predecessor.
- `retract ID` fails with exit 1 if `ID` does not exist; when it succeeds it
  appends a new tombstone record with a fresh id, indexed under the target's
  key, so `history KEY` shows claim and withdrawal together.

## Time travel and provenance

```sh
nisaba t.db inscribe cfg "v1"
nisaba t.db inscribe cfg "v2"
nisaba t.db canon cfg --as-of 1        # "v1" — folds the log prefix up to and including id 1

nisaba t.db inscribe owner carol --witness "alice ok" --source "review #12"
nisaba t.db witness 10                 # id / witness / source / recorded
```

`--valid-time T` stores a caller-supplied validity timestamp for the caller's
own reasoning; it never influences canon.

## Standard recipes

```sh
N=./nisaba; d=$(mktemp -d)

# KV roundtrip
$N "$d/t.db" init
$N "$d/t.db" inscribe project.status active
$N "$d/t.db" canon project.status; echo "rc=$?"

# Batch without a REPL
printf 'inscribe a 1\ncanon a\n' | $N --quiet "$d/t.db"

# Inventory every key (dotted prefixes are convention, not structure)
$N --json "$d/t.db" scan "" | jq -r '.key'
$N "$d/t.db" scan project. --limit 20

# Audit record with provenance
$N "$d/t.db" inscribe deploy.sha abc123 --witness "$USER" --source "ci run 42"

# Maintenance pass (checkpoint before verify: verify checks the snapshot)
$N "$d/t.db" checkpoint && $N "$d/t.db" verify && $N "$d/t.db" stats
```

## Sharp edges (each one verified against the binary)

1. **Global options before the DB path.** `nisaba DB canon k --json` is
   exit 2.
2. **`--readonly` does not block writes.** It only prevents creating a
   missing file; inscribing into an existing archive still succeeds. Do not
   treat it as a safety interlock — this is an implementation gap, not a
   guarantee.
3. **`scan` hides schisms.** A forked key scans as `key null` with no flag;
   only `canon` (exit 4) and `schisms` report the conflict.
4. **`verify` needs a snapshot.** Without a prior `checkpoint` on disk it is
   exit 3, and it validates the index only — it is not a log audit.
5. **There is no `compact` command** in the CLI. Absent it, nothing ever
   destroys a recorded fact, including retractions.
6. **Reads never create; writes always do.** A typo'd path plus a write
   command silently mints a brand-new database. Check `stats` if unsure.
7. **Int-typed digit strings bypass JSON quoting** (see typing section).
8. **A crash only ever loses an incomplete tail.** Invalid trailing frames are
   truncated on open; earlier records are never rewritten. Appending junk to a
   log loses exactly that junk.
9. **Delete `DB.idx` freely.** The next open rebuilds it from the log; a stale
   snapshot is never consulted. `--no-index` skips it entirely.

## Maintaining this repository

```sh
make            # libnisaba.a + ./nisaba (cc, C11, -Wall -Wextra -Werror)
make test       # full suite
make lint       # strict compile + clang-tidy hard gate
make ci-local   # lint + build + test + ASan/UBSan
make install    # bin, lib, header into ~/.local (PREFIX= to override)
```

- `include/nisaba.h` is the entire public API and record model;
  `src/internal.h` is private. The module map is in `README.md` (log, codec,
  btree, model fold, db, cli).
- The Sumerian theme (inscribe, canon, schism) lives only at the CLI surface;
  storage code uses plain names (`Record`, `Log`, `Index`, `BTree`,
  `KeyState`). Keep it that way.
- Tests are plain C in `tests/nisaba_tests.c` and must stay warning-free.

## Troubleshooting

- `cannot open …: i/o error` (exit 3) → path does not exist and the command
  was a read (or `--readonly`); `init` or any write command creates it.
- `canon` printed `null` (exit 1) → no live claim: key unknown, or its only
  head was retracted. Retraction never resurrects.
- exit 4 → schism; follow the resolution recipes above, never a plain
  `inscribe` on a forked key.
- exit 2 with a usage dump → bad flag order or unknown command; global
  options precede the DB path.
- `verify` exit 3 → no snapshot yet; run `checkpoint` first.
