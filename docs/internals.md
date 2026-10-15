# OP-1 R246 internals — how the SDK works

Reference material: the facts the SDK builds on. Most of these are declared with verified
signatures in `include/op1/te246.h`. The local analysis artifacts (`analysis/`,
`hooks_op1r10.json`) are TE-derived and not in git — regenerate them with `tools/memimage.py` +
objdump + `tools/xref.py`.

## The pipeline

```
 your code (C / asm)          your copy of op1_246.op1 (stock R246)
          \                              /
        tools/build.py    (one project: examples/, features/)
        tools/compose.py  (a recipe of features: recipes/op1plus.json)
                        |
             an installable update .op1
                        |
        tools/flash.py  →  te-boot "Firmware Upload" mode  →  your OP-1
```

Every tool checks the stock image's SHA-256
(`c5315218f825f143b415ca554516541898abee843d3a236df0b54c04e1fb13a9`) first and refuses to work
on any other firmware. Every hook is checked against the original instruction bytes at its
address — if the firmware does not look exactly like the SDK expects, the build fails before
anything is written. The pipeline is verified offline: it rebuilds the modified firmware
op1r 10's `OP1_vdk.ldr` byte for byte (SHA-256 `79de254a…`), see `tools/verify_op1r_rebuild.py`.

`tools/flash.py` refuses any volume that is not te-boot's upload disk (macOS `/Volumes` by
default; `--volume` overrides).

## Repository layout

```
tools/
  build.py               project dir + project.json + stock .op1 -> installable update (C and asm)
  compose.py             features + recipe -> composed project (shared hook dispatchers) -> image
  flash.py               copy an update to te-boot's Firmware Upload disk (refuses the normal disk)
  features.py            feature catalog: list / show / points / SQL query (builds features.db),
                         check / pack / install / new for published packages
  fpkg.py                package standard: validation, install (features.lock.json), scaffold
  test_all.py            run every host unit test and simulator bench
  unpack_op1.py          .op1 -> directory (CRC-32 + LZMA alone + tar)
  op1pack.py             .op1 read/write in memory (original tar order, op1repacker LZMA settings)
  ldr.py                 Blackfin BF52x LDR parser: blocks, header checksums, memory segments
  memimage.py            LDR -> flat memory dumps (one .bin per segment + layout.json)
  ldrpatch.py            LdrPatcher: write(addr, bytes), add_block(addr, code), build()
  bfin_branch.py         decode/encode 32-bit CALL / JUMP.L (pcrel25m2)
  hooks.py               diff two memory dumps -> branch hooks / pointer redirects / data edits
  xref.py                objdump listing -> functions, call graph, string refs, source-file names
  op1r_edits.py          op1r manifest edits -> memory addresses
  verify_op1r_rebuild.py proof: stock + op1r blob + hooks.json == op1r's OP1_vdk.ldr
  ldr2elf.py             LDR / memory dumps -> ELF the simulator and gdb can load
  midi_send.swift        CoreMIDI sender: `swift midi_send.swift OP-1 note 60 100` (device tests)
  sdk.ld                 linker script: one contiguous image at SDK_LOAD
include/op1/te246.h      TE R246 API: strings, tables, enums, MIDI, keys/knobs, audio/FX vtables,
                         heap, std::string ctor/dtor, memcpy/snprintf, cache flush, hook sites
include/op1/gfx.h        overlay drawing in UI coordinates + 3x5 font (source in lib/gfx.c)
lib/                     SDK libraries (gfx, rt)
features/<name>/         the feature library: synth engines, effects, performance features,
                         each with feature.json manifest, tests and sim bench — docs/features.md
recipes/*.json           compose features into images (op1plus.json = the default set)
examples/                minimal hook projects: hello (asm), counter_c (C), midi_events, fb_test
sim/                     simulator benches for the SDK itself (hooktest, ctest, miditest, fbtest,
                         gentest, overlaytest) + simtest.py helpers
docs/                    features.md (library + shared hook points), feature-package.md (the
                         packaging standard), toolchain.md, internals.md, ideas.md (backlog)
op-1 key mapping/        the device-verified key/knob id sheet (csv)
toolchain/bfin/bin/      GNU binutils 2.43.1 + GCC 14.2.0 + GDB 16.3 for bfin-elf (as, ld, gcc,
                         objdump, gdb, run = simulator), built locally (not in git)
analysis/                disassembly (stock SDRAM, L1, op1r blob), strings, stock_funcs.json
                         (local only, TE-derived: regenerate with tools/memimage.py + objdump + tools/xref.py)
hooks_op1r10.json        every edit op1r 10 makes, classified (local only, TE-derived)
```

## Chip and image

**Chip:** ADSP-BF524C2 (string in the image). TE code is VisualDSP++ C++ (Dinkumware `std::string`:
16-byte SSO buffer at +4, size at +0x14, capacity at +0x18), with the VDK RTOS.

**Image** `OP1_vdk.ldr`: 801 LDR blocks, all header checksums valid. Block 0 is `FIRST|IGNORE`, and its
argument is the file length − 16. The last loaded block is `FINAL`; after it comes one `IGNORE` block
(0xF0 bytes, not checked).

| Range | What |
|---|---|
| `0x01000000–0x01aa7078` | code + data (code ≈ `0x018ab7cc–0x01aa7078`, about 12,700 `LINK` sites) |
| `…–0x01dfff34` | zeroed BSS (FILL blocks) |
| `0x01e00000–0x01ffffff` | **free**, about 2 MB; op1r loads its 109 KB here |
| `0x02000000…`, `0x02040000…` (8 MB), `0x03000000…` | buffers (FILL) |
| `0xffa00000` 48 KB | L1 instruction SRAM (hot code) |
| `0xff800000`, `0xff900000` | L1 data A/B |

## How to add code

Exactly what op1r does:

1. Add one plain data block (flags `0x0006`) right before the `FINAL` block.
2. Write block 0's argument (= file length − 16) and fix its header checksum.
3. Redirect TE, either:
   - **call-site hook**: rewrite a `CALL` instruction at a call site (`call_sites` in project.json), or
   - **entry hook** (`entry_hooks`): overwrite the first 4 bytes of a function with `JUMP.L`
     (build.py checks the original bytes against `expect`; `expect` may be longer than 4 bytes when
     an instruction straddles +4). The hook re-runs those instructions and jumps back.
   - **pointer redirect** (`pointers`): replace a 32-bit code pointer (vtable slot, callback), checked
     against its stock value — op1r's way of replacing FX/voice functions.

   With `"c": "fn"` instead of `"to"`, build.py writes the trampoline and the replay itself; options
   `"filter"` (C returns nonzero → TE function skipped), `"after"` (TE first, then C), project-wide
   `"init"` (runs once). Entry hooks export `te_orig_<addr>()` to call the unhooked original.

## Flashing: why te-boot's upload mode

Updates only install from te-boot's **Firmware Upload** mode, not from the normal disk mode
(te-boot firmware.cpp, loop at te-boot `0x6d4`): te-boot installs every file ending in `.OP1`
from its own RAM disk (label `OP-1`, buffer `0x01000000`); names containing `FRC` set a force
flag, `RBT` a reboot flag, the rest of the name is free. The normal disk mode
(`album/ drum/ synth/ tape/`, ~400 MB) is user storage served by the main firmware; te-boot never
reads it, and an update copied there is silently dropped. Restoring stock works the same way with
the original `op1_246.op1` (te-boot itself is never modified).

## Booting TE's firmware in the simulator (blocked)

The image's L1 blocks (flags `CALLBACK|INDIRECT`: L1 instruction `0xffa00000` 48 KB, plus most L1
data blocks) are **encrypted**. In every 24 bytes, the first 8 bytes (one 64-bit block) are
scrambled; SDRAM code is plaintext. The reset/startup code lives in L1, so the firmware cannot
start in the simulator without decrypting it first.

- te-boot contains XTEA (`0xffa0bee6` in its L1, 32 rounds), used to decrypt a hex key file with a
  key built from two tables (`key[i] = A[i] ^ B[15-i]`). It also reads and writes the chip's OTP
  memory (boot ROM `0xEF000018/1A/1C`) and prints "Signature Key programming succeeded!". So the L1
  key is probably stored in each OP-1's OTP, not in the update files.
- Not yet found: te-boot's loader code that applies the CALLBACK to OP1_vdk.ldr blocks.

## Simulator and benches

GDB's Blackfin simulator has a `bf524` model: core, L1 memory, interrupt controllers, DMA, timers,
GPIO, PPI, SPI, TWI, UART, NAND controller, SDRAM controller. Its SPORT (audio) and MUSB (USB) are
only empty memory stubs. TE's own firmware cannot boot in it (encrypted L1, above), so the benches
run your code against drivers and stubs that emulate the TE side. Run them all:
`python3 tools/test_all.py`.

- **Hook test bench:** `sim/hooktest/run_hooktest.py` loads the hello hook, a driver and a check
  stub (at the TE target address) into `bfin-elf-run --model bf524`. It checks R0/P0 are kept, RETS
  points back to the caller, SP is restored and the counter is 1. `HLT` = exit 0 = pass, `ABORT` =
  exit 1. Use `HLT`/`ABORT` to end tests: the libgloss exit syscall (`EXCPT 0`) faults in this
  model. `--environment operating` crashes GDB when it loads a program; use the default
  environment. The default environment maps only SDRAM + scratchpad; `sim/simtest.py` adds L1
  data A/B with `--memory-region` (SDK code reads TE state there).
- **C hook test bench:** `sim/ctest/run_ctest.py` builds `examples/counter_c`, then its driver
  constructs a real `te_string` ("pattern", SSO form) in free SDRAM and calls the hook through the
  C trampoline. The check stub additionally verifies the C code read the string: `last_name` must
  equal "pattern". Addresses come from the project's ELF, so the bench follows the code.
- **MIDI entry-hook bench:** `sim/miditest/run_miditest.py` builds `examples/midi_events`.
  The driver feeds three `te_bytes` messages (note-on, CC, pitch bend) through the hook; the check
  stub sits at the dispatcher address starting with the original `LINK 0x10`, so the replay path
  runs for real. Verifies register/stack state after the replayed LINK plus all counters and
  last-event values (15 checks).
- **Framebuffer bench:** `sim/fbtest/run_fbtest.py` builds `examples/fb_test` (both hook
  types in one image), maps a zeroed 0x02000000 segment as the panel, and the stub checks 12
  pixel-exact results: 4 corner markers, note-bar top/end/below, CC block, counters, CPU state.
- **Per-feature benches:** every feature has one (`features/<name>/sim/run_*test.py`), built
  through the composer; `sim/gentest` covers the generated C trampoline (init-once, filter,
  replay) and `sim/overlaytest` the render-then-overlay order.

Simulator note: GDB's bfin sim executes some parallel instructions wrongly (a store in
`R0 = R7 + R1 || [P5++] = R0` took the new R0); when running TE code in the sim, check results.

## Names

887 assertion strings such as `..\src\dsp\Adsr.cpp:203 …` name the functions that use them.
`xref.py` currently names 556 of 5,337 call targets.

## Registration points

Found so far:

| Address | What |
|---|---|
| `0x018dd810` | FX table `{char* name, int id}` ×8: delay 0, grid 1, punch 2, phone 3, spring 4, filter 5, nitro 6, cwo 7 |
| `0x018fa634` | sequencer table ×6 in lookup order: pattern 0, tombola 2, endless 1, finger 3, sketch 4, arpeggio 5 |
| `0x018de8ec` | modes: synth 1, drum 2, tape 3, mixer 4, tempo 5, mic 6, album 7, seq 8, browser 9 |
| `0x01dd4d98` | runtime instrument-type table ×15 (BSS, filled at boot by `0x019b988c`); names at `0x018df7bc` (fm, digital, drwave, string, cluster, pulse, sampler, phase, drum, dbox, ls, dna, dsynth, voltage, iter) |
| `0x019b97c4` / `0x019b9810` | is-instrument-type / type-from-name (loop count 15 hard-coded) |
| `0x019b975c` | C string vs `std::string` compare |
| `0x01a422ec` / `0x01a42344` | `SeqIds.cpp` id → name / name → id |
| `0x019aad38` / `0x019aae44` | `fx_types.cpp` id → name / name → id |
| `0x01a9f37c` | `strncmp` used by all the lookups |
| `0x01919aa8` | assertion printer (fmt + up to 2 args) behind all 887 assertion strings. **Fatal after boot** (flag `0xff9002f4`): CLI, audio off, message drawn, endless IDLE |
| `0x01a28210`, `0x01a28da0`, `0x01a293bc` | `ReadInterface.cpp` param range checks (lfo / master fx / drum fx) |
| site in `0x01a0d050` | `PlayerManager.cpp` "unknown sequencer type" |
| `0x01a85c20` | `VoiceCtl.cpp` MIDI dispatcher: every message as `std::vector<uint8_t>*` in R0, routes status&0xf0 (0x80/0x90/0xb0/0xe0); registered as callback ptr at `0x01aadb24`, callers live in encrypted L1 — hook the entry |
| `0x01a84f8c` / `0x01a859e4` / `0x01a84efa` | MIDI CC / note-on / pitch-bend handlers; CC values land in the 128-entry table at `0x01dff138` |
| `0x02000000` / `0x03000000` | **framebuffers, double-buffered**: 16-bit 5-6-5, 512-byte stride, panel rotated vs the 320×156 UI (UI (x,y) → line 319−x, column y). GUI thread `0x019ab880`: pend semaphore → render `0x019b2674(buf)` (clears buf first) → cache flush `0x01aa185c` → present `0x01a2fb18` (repoints DMA0 descriptor `0xff801940`). Overlay hook: call site `0x019ab8b2` with `"after": true` |
| `0x019b6c74` / `0x019b6c0c` | `on_key(id, down)` / `on_knob(knob, int16 *diff)` from the KeyHandler thread (call sites `0x019c2d3e` / `0x019c2e94`); raw scan is in L1 |
| `0x019f09d6` / `0x019f0a6a` | main thread: call to empty fn once before the main loop / every main-loop pass — app init + tick hook sites |
| `0xff80036c` | audio block callback pointer `cb(tx, rx)`, 64 stereo frames, SPORT0 I2S 32-bit slots |
| `0x01a1b5f0` / `0x01a1b6d0` / `0x01a1b7d4` | stereo FX / mono FX / synth engine factories (dsp/producer_factory.cpp); FX vtable +0x14 process, op1r redirects filter slots `0x01910ca8` / `0x01910cc8` |
| `0x01a5e3b0` / `0x01a5e310` | operator new / delete (TE heap, 8.25 MB); std::string ctor `0x019baac6`, dtor `0x01a17bd8` |
| `0x01a2fb68` | DMA0 frame-done ISR body (wrapper `0x01a2fd04`): flips back buffer flag `0x01de6106`, wakes the GUI thread; draws the text console only in that mode |
| `0x018f738a` | boot text `R. 00246` (8 bytes) |

GUI stack: AGG (Anti-Grain) rasterizer + rapidxml SVG loader (`src/gui/lib/AGG/agg.cpp`, pool
allocator limit 0x19000 = one 320×160×2 frame), screens in `src/gui/screens/*.cpp`, hardware layer
in `src/bf/*.cpp` (RGB, SPI, audio, gpio, nand, twi, usb). Box2D drives the physics screens.

op1r hooks 37 call sites and 14 table pointers (see the local-only `hooks_op1r10.json`).
`0x01e00000` in op1r is not a one-time init: it re-clamps 3 × 8 L1 parameter slots to op1r's
extended ranges and is called by the wrappers around the ReadInterface range checks. Its boot hook
is call site `0x019f07d2` (screen registration), which also loads op1r's SVGs. New FX/engines are
multiplexed onto the filter FX and the iter voice via a parameter selector rather than added as
new ids.
