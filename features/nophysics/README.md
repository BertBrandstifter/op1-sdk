# nophysics

Takes TE's Box2D physics library out of the firmware, together with its only two users: the
**Tombola** sequencer and the hidden **Lander** game.

## What changes on the OP-1

- **Sequencer picker:** choosing Tombola starts the **pattern** sequencer. The picker still shows
  the Tombola icon; it just leads to pattern.
- **Saved Tombola setting:** if Tombola was the synth or drum sequencer before the update, the OP-1
  starts with pattern instead. The next time a sequencer is picked, TE writes `pattern` into the
  setting.
- **Lander:** the hidden key combination does nothing.
- **Boot:**
  - No physics world is built. TE built two of them at every boot (one for Tombola, one for Lander),
    used or not.
  - `lander.svg` (171 KB) and `tombola.svg` are no longer parsed. Both files are an empty 303-byte
    SVG now.
- **Code:** 144,888 bytes of TE code are overwritten with `RTS`: all of Box2D (115 KB) plus the
  Lander and Tombola code nothing can reach any more. That space is free for later SDK code (the
  build does not place code there yet).
- **Update file:** about 92 KB smaller (LZMA packs the `RTS` runs and the empty SVGs to almost
  nothing).

## How

Everything is in `feature.json` (no C code); `build.py` checks each change against the stock image.
The 36 code pointers that led into the removed code now lead to `0x0191fe8c`, the first `RTS` of the
blanked Box2D range. That is TE's own code page, which Box2D's initialisers ran from, so nothing
depends on the SDK code region being mapped that early in boot.

| What | Where | Change |
|---|---|---|
| Box2D's 30 boot-time static initialisers | ctor table `0x01000030..` | → `RTS` at `0x0191fe8c` |
| Lander screen: "SVG loaded" (builds its b2World) / draw | vtable `0x019106e0` / `0x019106d8` | → `RTS` at `0x0191fe8c` |
| Lander contact listener (only Box2D calls it) | `0x019106a0` | → `RTS` at `0x0191fe8c` |
| Tombola screen: "SVG loaded" (physics bodies) / draw | `0x01911ee8` / `0x01911ee0` | → `RTS` at `0x0191fe8c` |
| Tombola knob handler (takes the physics lock) | `0x01911d10` | → `RTS` at `0x0191fe8c` |
| Lander key combination (held-key mask `0xa0` → mode 15) | `0x019b66aa` | compare with `0x100`: never matches |
| SeqIds: `"tombola"` → id | `0x018fa640` | 2 → 0 (pattern) |
| Player factory, case 2 (TombolaPlayer) | switch table `0x018f2f00` | → case 0 (pattern player) |
| Sequencer settings init: Tombola physics setup | `0x01a63fda` | `JUMP.S` over it |
| Box2D, dead Lander / Tombola code | 17 ranges | `RTS` (`blank`) |

TE has one name→id lookup (SeqIds, `0x01a42344`). The picker (`seq_types` row → name) and the saved
`synth_sequencer` / `drum_sequencer` setting both go through it, so id 2 can no longer occur. The
two other places that turn an id back into a name take it from the player manager, which only holds
lookup results.

`b2WorldCallbacks.cpp` (`0x0193bff0..0x0193c12c`) stays: the TombolaScreen object's boot
initialiser still calls one of its constructors.

## Why it is safe: `prove.py`

```
python3 features/nophysics/prove.py <stock op1_246.op1>     (needs the local analysis/ disassembly)
```

The proof is a greatest fixpoint over the stock image with the patches applied:

- **Chunks:** Box2D and the Lander / Tombola regions are cut into chunks only where the previous
  instruction cannot fall through.
- **Liveness:** a chunk stays live if anything outside the dead set refers to any byte of it. That
  covers branches and calls, `X.H` + `X.L` constants (every pairing within 10 instructions), and any
  32-bit word of the loaded image, including the L1 data blocks and L1 code.
- **Result:** every blanked range must lie inside dead chunks. 145,090 dead bytes found; 144,888
  are blanked. The two pieces left are the tails of live functions at the region edges.

**Assumptions** (each listed with its evidence in `prove.py`):

- **Not pointers:** 22 words that merely look like code addresses (16-bit number tables, floats,
  instruction bytes).
- **Box2D's own tables:** its vtables and switch tables are only loaded by Box2D code (checked).
- **Exit-time destructors:** TE's destructor-at-exit records are run only by `exit()`, which the
  OP-1 never reaches. With the `RTS` fill, such a call would just return.
- **L1 calls:**
  - The L1 code dump has some 8-byte blocks that did not decrypt. They hide the low half of 10
    `P1.H = x; CALL (P1)` calls from L1.
  - For the `0x191` ones, 23 of the 29 that decode go to TE's assert printer.
  - For the others, decoded calls nearby go elsewhere.
  - Again, a stray call into the range returns at once.

## Tests

`sim/run_nptest.py`:

1. **LDR span helpers:** synthetic stream with data and FILL blocks.
2. **Compose:** no op1plus hook lies in a blanked range; one inside is refused.
3. **Simulator:** the `RTS` fill is called like TE's ctor loop and a vtable call do it. It returns,
   and the stack and callee-saved registers are kept.
4. **Built update** (with the stock firmware): every blanked byte is `RTS`, the 36 pointers lead to
   the first one, the other 4 patches are in, nothing else changed, and the SVGs are the stub.
5. **Proof:** `prove.py` passes.

## Device test

1. Flash. It must boot as before.
2. Sequencer picker → Tombola: the pattern sequencer comes up and plays.
3. Power off and on: still pattern.
4. Try the old Lander combination: nothing happens.
5. Power off: the OP-1 switches off normally (exit path).
