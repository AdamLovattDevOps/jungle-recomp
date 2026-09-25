# Working agreement

Before starting another round of work, both must be true — otherwise halt and say so:

1. The round materially moves toward **testing a port on the Mac** — something runnable, not more
   format archaeology.
2. The short-term session token budget has **over 80% left** before the next reset.

Condition 2 cannot be checked programmatically. The claude.ai usage endpoint sits behind a
Cloudflare JS challenge and returns 403 to any non-browser client, and the clearance cookie expires
within hours, so it is not automatable. Adam states the number; it is not assumed.

## The goal is byte accuracy

The target is a **matching decompilation**: C that recompiles to byte-identical object code against
the original binary, verified by diff. Not an approximate reimplementation.

Consequences:

- No crude scaffolds, hard-coded screens, or "runnable stub now, real engine later".
- Matching requires the **original 1995 toolchain**. Modern compilers cannot reproduce Microsoft C's
  register allocation and instruction selection. Target Microsoft C 7.0 or Visual C++ 1.5 (16-bit)
  under DOSBox.
- Progress is measured as **percent of bytes matched**, by a build-and-diff harness.
- The native SDL port comes only after the matched tree exists; that tree then stays frozen as the
  reference implementation.

`docs/PIPELINE.md` phase 5 already describes this workflow.
