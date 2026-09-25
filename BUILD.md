# Building and running

```sh
cc -O2 -Wall -o jungle src/jungle.c $(sdl2-config --cflags --libs)
```

Requires SDL2 (`brew install sdl2`). Builds native on Apple Silicon.

## Run

```sh
./jungle orig/cd/JUNGLE/JUNGOPTS.BIN      # open a window and browse bitmaps
./jungle orig/cd/JUNGLE/JUNGOPTS.BIN --verify   # headless: decode everything, report
```

Arrow keys or space browse, `q` or escape quits. The window scales by integer factors and
letterboxes, so it is resolution-independent already.

## Verification

`--verify` decodes every bitmap in a container without opening a display, so the codecs can be
checked in CI or over SSH. Across all 13 containers:

```
11,506 bitmaps    11,506 decoded    0 failed    111,562,116 pixels
```

That matches the Python reference implementation exactly.

## Modes

| Command | What it does |
|---|---|
| `./jungle FILE.BIN` | open a window, browse bitmaps (arrows/space), play audio (`a`), quit (`q`) |
| `./jungle FILE.BIN --verify` | headless: decode every bitmap, report failures |
| `./jungle FILE.BIN --audio` | headless: decode every audio clip, report format and duration |
| `./jungle FILE.BIN --script` | headless: run the script VM over the scene scripts |

## Native results

Bitmaps, all 13 containers:

```
11,506 bitmaps    11,506 decoded    0 failed    111,562,116 pixels
```

Audio: every clip decodes at 22050 Hz mono 16-bit, durations matching the Python reference to a
tenth of a second.

Script VM: 4,177 expressions executed, 0 failures.

The C walker is a full graph walk, following both arms of every branch, every switch case, and
backward jumps (loops). Its opcode length table is **generated** from `tools/opcode_table.py`, which
in turn parses the decompiled interpreter — so it cannot drift from the source of truth by hand.

Coverage, C against the Python reference:

```
C   3,306 / 3,323 resources fully walked  (99.5%)
PY  3,302 / 3,323                         (99.4%)
```

Getting there took three steps, each found by measuring rather than guessing:

1. **Hand-written opcode table: 84.9%.** Replacing it with one *generated* from
   `tools/opcode_table.py` (which parses the decompiled interpreter) gave **94.6%** in one step.
   That is the argument for generating the table rather than transcribing it.
2. **Opcode 38 missing: 95.8%.** Present in the Python walker, absent in C.
3. **Terminator records not marked as covered: 99.5%.** Opcode 44 and opcode 0 have zero length, and
   the C walker broke out of the loop without marking the opcode word itself. Every affected
   resource came out exactly one or two bytes short of complete — which reads as a parsing failure
   and is actually an accounting one.

Step 3 was worth 3.7 points on its own. Two earlier fixes aimed at plausible causes — the worklist
size cap and unvalidated branch targets — changed nothing; they are still correct and were not the
bottleneck.

## Cross-validation

`--dump` writes indexed-colour PNGs using the game's own palette, so output is directly comparable
with the Python extractor's.

```
JUNGOPTS   123 byte-identical, 0 differing
JUNGMAIN 1,861 byte-identical, 0 differing
```

**1,984 files, zero differences.** Two independent implementations — one Python, one C — agree
bit-for-bit across container parsing, the resident/streamed addressing split, both bitmap codecs,
palette mapping, bottom-up row order, and PNG encoding.

That is the strongest correctness evidence available short of the matching decompilation itself: a
shared misunderstanding would have to produce the same wrong bytes twice, through separately written
code.

### Audio cross-validation, and a bug it caught

```
JUNGMAIN audio: 99 byte-identical, 0 differing
```

Getting there exposed a real defect. The C loader read the ADPCM tables at offset `0x1E0` of the
**DLL file**, but those offsets are relative to the DLL's **data segment**, which starts at file
offset `0x4B50`. C was reading unrelated bytes as its step table.

The failure mode is the dangerous kind: the bogus table began with zeros, so every delta was zero
and the decoder emitted **perfect silence**. No crash, no noise, no warning — output that looks like
a quiet clip. Without a byte-for-byte comparison against a second implementation it could have gone
unnoticed indefinitely.

The fix locates segment 2 through the NE header rather than hardcoding `0x4B50`, so it stays correct
for the other modules too.

### Full cross-validation

Every asset on the disc, C build against the Python reference:

```
             PNG                WAV
JUNGBUGD     813/813            75/75
JUNGBURP     847/847            79/79
JUNGCRED      19/19              1/1
JUNGHIPP    2133/2133          157/157
JUNGINT1     544/544             1/1
JUNGINT2     275/275             1/1
JUNGLE        28/28              1/1
JUNGMAIN    1861/1861           99/99
JUNGOPTS     123/123             1/1
JUNGPINB    1253/1253           98/98
JUNGPRTY    1372/1372            5/5
JUNGSCOR     327/327            19/19
JUNGSHOT    1911/1911           91/91

TOTAL     11,506/11,506       628/628
```

**12,134 files, zero differences.**

### VM cross-validation

`--vmtrace` prints `resource offset result error` per executed expression, directly diffable against
the Python VM.

On `JUNGOPTS` the two agree exactly — 343 lines each, all identical. **That does not generalise.**
Across all thirteen containers:

```
TOTAL   C 14,977   Python 15,506   identical 14,927   (96.3%)

JUNGOPTS  343/343  exact          JUNGSHOT  2,356 vs 2,683, 2,343 identical
JUNGSCOR  516/516  exact          JUNGPINB  2,772 vs 2,902, 2,771 identical
JUNGPRTY  143/143  exact          JUNGHIPP  3,746 vs 3,770, 3,727 identical
JUNGCRED/INT1/INT2/JUNGLE exact   JUNGMAIN    275 vs   267,   266 identical
```

Two distinct problems remain, and they point in opposite directions:

- **579 expressions Python reaches that C does not**, concentrated in the gameplay containers
  (`JUNGSHOT` 340, `JUNGPINB` 131).
- **50 where C and Python disagree on the result**, and `JUNGMAIN` where C emits *more* records than
  Python — so it is not simply that one walker is a subset of the other.

Tuning against a single container produced an exact match there and hid both. The lesson is the
measurement's scope, not its arithmetic: a green result on one sample said nothing about the other
twelve.

Neither walker is a superset of the other. On `JUNGSHOT` the C walker **completes more resources**
than Python (309 vs 308) while executing **fewer expressions** (2,356 vs 2,683). They diverge in
which records get executed inside a resource, not in overall reach — so this is not a case of one
implementation simply being behind.

The single differing result found so far (`JUNGHIPP` resource 6917, offset 368: C −400, Python 0) is
the expression `*slot5124 = load(slot5124) - slot5128`. Both implementations handle those opcodes
and that addressing identically, so the difference is inherited from preceding state — a record one
walker ran and the other did not. Chasing it means aligning the walkers first, not the VMs.

The earlier fixes below were still real improvements. C marked every **byte** of a record as seen and used that to
decide whether an offset had been visited. A branch target can legitimately point *inside* the span
of an already-walked record, and conflating byte coverage with visited record starts made the walker
refuse to follow it — losing 60 expressions.

Tracking visited starts separately from covered bytes fixed it. Python had always kept them apart,
which is why it found those records and C did not.

Two harness bugs surfaced first, and both looked like semantic disagreements:

- **Shared VM memory.** C reused one VM across all resources while Python resets per resource, so a
  value written by an earlier script leaked into a later one and changed its result. Results
  depended on directory order.
- **Different empty-expression filters.** Python skipped records with `len <= 4`; C ran them.

Neither was a VM defect. Both would have been invisible without a line-by-line comparison against a
second implementation.

### The remaining walker gap, isolated

Diffing the two walkers record-by-record on `JUNGSHOT` resource 68:

```
C 22 records, Python 24 records
offsets only in Python : 15, 215
offsets only in C      : none
differing spans        : none
```

That is a precise and small discrepancy. **Every record length agrees** — the two walkers compute
identical spans wherever both visit an offset, so the opcode table and length rules are correct in
both. C simply fails to visit two branch destinations.

Since C is never ahead and spans never differ, this is a seeding or visit-ordering issue in the C
worklist, not a decoding one. It accounts for the 579 expressions Python reaches that C does not,
and for the one differing result (`JUNGHIPP` 6917/368), which inherits state from a record C never
ran.

Chasing this exposed two instrumentation faults before any real defect:

1. The first comparison used `paste`, splitting columns at the wrong offset because C emits five
   fields per line and Python four — a misleading alignment in both directions.
2. The offsets "only in Python" were **terminator records**. C visited them; the trace printed
   records *after* the terminator check, so they never appeared. The walker was correct and the
   instrumentation was lying.

Fixing the trace brought that resource to 24 records against Python's 24, exactly. It did **not**
change overall parity (still 14,927 of 15,506), because terminators carry no expression — so the
579-expression gap is a separate issue and remains open.

Two rounds of investigation here produced two tooling fixes and no engine fix. That is worth
recording: when a comparison disagrees, the comparison is a suspect too.

### The 579-expression gap: current evidence

Per-resource expression counts on `JUNGSHOT`:

```
res 1930   C  16   PY  32       res  243   C   9   PY  18
res 2022   C   8   PY  16       res  254   C  18   PY  22
res  646   C 249   PY 548       res  213   C   3   PY   2
```

Several resources are **exactly double**, which points at one walker re-entering a region the other
visits once — a revisit or loop-unrolling difference rather than a decoding one. But `res 213` has C
ahead of Python, so it is not a uniform factor and not simply "Python walks twice".

Both walkers agree on every record length wherever they meet, so the opcode table is not implicated
in either direction. The difference is in traversal: which offsets get queued, and whether an offset
already walked from one path can be walked again from another.

The obvious explanation — Python re-walking a region — is **ruled out**: every offset Python yields
is distinct, with zero duplicates in any resource checked.

Total record counts show the divergence is larger and runs both ways:

```
res 1930   C 26 records   PY 50      C behind
res  243   C 17           PY 35      C behind
res  213   C 29           PY 11      C nearly 3x ahead   (1,605-byte resource)
```

So this is not "one walker is behind". On `res 213` the C walker reaches nearly three times as many
records as Python in a large resource; on others it reaches half. Both compute identical record
lengths wherever they meet, so the opcode table is sound in both — the difference is entirely in
**which offsets get queued and in what order**, and it is substantial in both directions.

Open, with the two cheap explanations eliminated (duplicate yields, and a uniform factor). Recording
the contradiction rather than a tidy story: a fix that only addressed C being behind would make
`res 213` worse.

### The coverage metric is inflated by forward jumps

Record spans in `JUNGSHOT` res 213, from the C walker:

```
off   0  op 77  span   25
off  25  op 56  span   32
off  57  op 37  span 1540     <-- jump distance, not a record length
off 118  op 37  span 1479     <--
```

**Opcode 37 is the jump.** Its field is where execution continues, not how long the record is. The
record itself is four bytes: opcode plus the signed advance. Everything between `p+4` and
`p+advance` is code the jump *skips over* — reachable from somewhere else, or dead.

Both implementations treat a positive opcode-37 field as a record span and mark every byte of it as
covered. For a 1,540-byte forward jump that marks 1,536 bytes as "walked" without decoding a single
record in them.

**So the reported coverage figures are inflated**, including the headline 99.5%. Resources counted
complete may contain large regions never actually decoded. The two walkers agreeing at 99.4% and
99.5% partly reflects a shared flaw rather than shared correctness — which is exactly what
cross-validation between two implementations cannot catch, because the error is in the shared model,
not in either translation of it.

It also explains the `res 213` anomaly: C emits more records there because it re-enters skipped
regions through branch targets that Python queues differently. Both then report the resource as
fully covered.

Fixed in both implementations: opcode 37 is now a 4-byte record plus a jump edge, in both
directions. Coverage dropped as predicted:

```
              before (inflated)   after (correct)
C                     99.5%             89.2%
Python                99.4%             89.1%
```

**Every coverage figure reported before this point was overstated by about ten points.** The two
implementations still agree closely, which is the point: they agreed at 99.4/99.5 too, and were both
wrong. Cross-validation catches translation errors, not errors in the shared model — and this was in
the model.

### Regression check after the jump fix

```
clean build          ok, no warnings
bitmaps              11,506 total, 11,506 decoded, 0 failed
PNG cross-check      1,253/1,253 byte-identical (JUNGPINB)
WAV cross-check         98/98   byte-identical (JUNGPINB)
VM parity            96.3%, unchanged by the jump fix
```

The asset pipeline is unaffected by the walker correction, as expected — bitmaps and audio do not
route through the scene-record walker.

### The honest remaining gap

With jump spans corrected:

```
358 partial resources
30,729 bytes unreached, of 150,038 bytes in those resources (20.5%)
largest single gap 5,464 bytes
```

These are regions no path from offset 0 reaches. Round 25 established that scripts are entered at
engine-supplied record pointers, so a type-14 resource is a collection of handlers rather than one
linear program — a walk seeded only at zero cannot reach all of it by design.

That makes this gap expected rather than a defect, but it is not yet *proven* to be the explanation.
Confirming it needs the entry-point source, which round 25 also showed is not statically resolvable
from the decompiled output.

## Status at a glance

```
$ make report
=== jungle-recomp status ===
bitmaps      11506 total, 11506 decoded, 0 failed
audio        628 clips, 2246 seconds
scene walker 2965 complete / 358 partial (89.2%)
script VM    14974 expressions, 3 failures
```

37 minutes of audio, every bitmap on the disc, and the scene VM running the game's own bytecode —
all native, no Python in the loop.

The three VM failures are divide- and modulo-by-zero, which the original defines (handler `0x6F`)
and which are expected when running expressions without engine state.
