# Performance findings (R246)

What we measured on the OP-1 and what we found in the firmware image. Date of each finding in
brackets. Measuring tool: `features/cpuinfo` (shift + speech, debug image `recipes/op1cpu.json`).

## Clock and audio block (device, 2026-11-14)

| | value |
|---|---|
| CCLK (core clock) | **400 MHz**, steady |
| CLKIN (crystal) | 25.0 MHz |
| PLL_CTL | `0x2000`: MSEL 16, DF 0 → 25 MHz × 16 = 400 MHz |
| VR_CTL | `0x70b0` |
| cycle counter (SYSCFG.CCEN) | already on (TE enables it) |
| audio block | 64 frames at 44.1 kHz = 1.451 ms |
| cycles per block | **≈ 580 K** |

TE's synth voices render 128 samples at 88.2 kHz per block (twice the output rate).

## Audio load (device, 2026-11-14)

Load = cycles inside TE's audio block callback / cycles between two callbacks.

| situation | average | peak | block average |
|---|---|---|---|
| idle (synth screen, no notes) | 10.3 % | 17.4 % | 60 K |
| most synth engines playing | 25-30 % | about +5 % | 150-200 K |
| dsynth playing | up to 60 % | up to 65 % | up to 350 K |

Share of the SDK engines (cycles in our engines' renders, same blocks):

| engine | share |
|---|---|
| most SDK engines | 10-15 % |
| MALLET (highest seen) | 18.7 % |

Not yet read separately: MONO 3, ACID 3 under a held chord / fast notes.

### Rules of thumb

- **Budget:** about 520 K cycles per block are free when idle; the red line on the cpuinfo graph
  is 70 % (406 K).
- **Base load:** TE alone (mixer, master FX, tape, UI-driven work) costs about 10 %.
- **Synth cost:** a typical synth engine adds 15-20 %.
- **dsynth:** by far the most expensive TE engine, about 50 % on its own.
- **SDK engines** cost about what TE's light engines cost. An extra engine instance that plays at
  the same time would add its share again (10-19 %).
- **Simulator vs device:** cycle counts from the Blackfin simulator are a lower bound (no SDRAM /
  cache latency). The earlier simulator estimate for MONO 3 (~25 %) is above anything measured on
  the device so far: re-measure MONO 3 before trusting either.

## Simulator cost per engine and what the 2026-11-15 review changed

Cycles per voice block (128 samples at 88.2 kHz) on the Blackfin simulator, lightest .. heaviest
sound of each engine; one block has about 580 K cycles.

| engine | cycles per voice block | voices |
|---|---|---|
| LOFI | 6.5 .. 11.6 K | 6 |
| STATION 1 | 7.7 .. 20.6 K | 6 |
| WARP | 8 .. 18 K | 6 |
| PARTY | 8.4 .. 22 K | 6 |
| MALLET | 10.8 .. 24 K | 6 |
| TRIO | 12.8 .. 25 K | 6 |
| WHEEL | 13 .. 25.5 K | 6 |
| PALETTE | 11.3 K per tone, 46 K for four | 12 tones |
| MONO 3 | 23 .. 37 K | 4 |
| ACID 3 | 45 K (was 74 K), with FILTER FM 125 K | 1 |

Changed in the review (the first three bit for bit the same output):

- **PLAY's decimator**: 1024 64-bit multiply calls per block -> a 14-instruction loop.
- **PLAY's drum voices**: 2 64-bit multiplies per sample -> 32-bit.
- **fxpack**: WAH 19.2 K -> 13.1 K per block, DJ and LADDER without their 64-bit divide.
- **ACID 3**: oscillator and filter at 88.2 kHz with fitted coefficients (176.4 kHz only while
  FILTER FM is on): 74 K -> 45 K.

Not worth it (measured): -O3 (2-7 %), constant divisions per block.

## Where TE spends flash and code space (static analysis, 2026-11-14)

### The update file (stock 13.04 MB, LZMA)

- **Samples, about 90 %:**
  - factory drum kits 7.08 MB
  - drum slot files 3.6 MB
  - factory synth samples 2.2 MB
- **`OP1_vdk.ldr`:** 0.99 MB compressed.
- **Done:**
  - drumtrim: −1.43 MB of silent tails
  - slotlink: −2.1 MB of byte-identical slot copies
  - nophysics: −92 KB
  - op1plus is now 12.94 MB

### SDRAM code (≈ 1.6 MB) and read-only data (≈ 0.45 MB)

| part | size |
|---|---|
| SQLite | ~390 KB |
| GUI (AGG rasterizer, SVG, screens) | ~190 KB |
| Box2D (removed by nophysics) | ~110 KB |
| named DSP code | ~83 KB |
| YAFFS2 (NAND file system) | ~53 KB |
| disk / FAT (USB disk mode) | ~52 KB |
| db2 | ~51 KB |
| jsoncpp | ~25 KB |
| USB stack | ~17 KB |
| not identified | ~885 KB |

### Free room

- **SDK region:** `0x01e00000..0x02000000` (2 MB); about 1.26 MB free after M1's samples moved to
  a file (2026-10-18).
- **nophysics:**
  - 144,888 bytes of TE code are RTS now, including 115 KB in one piece at
    `0x0191fe8c..0x0193bff0`.
  - Usable for SDK code once the build can place code in a second region.
- **Not free:** SDRAM outside TE's image is TE's malloc heaps (see
  `op1-ram-investigation` notes: heap 0 at 0x100, heap 1 at 0x03040000).

## Boot / RAM effects of nophysics (2026-11-14, not measured)

TE built two Box2D worlds at every boot (Tombola, Lander) and parsed `lander.svg` (171 KB) and
`tombola.svg`. nophysics stops all three. The heap and boot time saved are not measured yet: compare
meminfo's heap peak and the boot time with and without nophysics.

## Open measurements

- **Idle vs playing with nophysics:** do the freed physics worlds change heap use or idle load?
- **MONO 3 and ACID 3 share:** held chords, fast notes.
- **Worst-case peak (WORST on the panel):** after a long session with tape playing and recording.
