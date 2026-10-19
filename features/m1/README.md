# STATION 1 synth engine (feature m1) — spec

Status: **v0.5** (2026-10-19): cleaner samples, stored at ≥ 16 kHz (small samples at their
source rate), ~1.4 MB, no added hiss above 5 kHz. v0.4 (2026-10-18): the samples are a file of the
update, loaded at runtime into TE's sample memory (they no longer take ~0.95 MB of the 2 MB SDK
region; see **Data**). Code: `features/m1/` (engine host, tag 3). A late-80s-style sample
workstation voice: two of 100 sampled multisounds (piano, organs, strings, choir, brass, bells,
basses, single-cycle waves ...) layered, into a low-pass filter with its own envelope, and a bank
of 28 ready-to-play sounds. Own code; the samples come from a sample set you own and stay on your
machine (see **Data**).

## Open it on the OP-1

Part of the default `op1plus` image. Pick **STATION 1** in the synth engine list, or load the preset `station 1` (browser folder station 1). Sounds: **shift + blue turn** (bank of 28); knob push cycles pages.

Without the sample data (see **Data**) the engine plays 8 built-in waves (saw, square, pulse,
triangle, sine, organ, e.piano, voice) and the bank has 8 sounds.

## What it is

| page | blue | green | white | orange |
|---|---|---|---|---|
| 1 | **WAVE A** (100 multisounds) | **WAVE B** (off + 100) | **MIX** A .. B | **CUTOFF** 30 Hz .. 16 kHz, top = off |
| 2 | **B PITCH** −24..+24 st | **DETUNE** A down, B up, 0..25 ct | **F.ENV** cutoff up to +6 octaves at note on | **F.DECAY** 5 ms .. 2 s |
| 3 | **RES** (gentle) | **KEY TRK** cutoff follows the key | **A DECAY** 0 = hold, more = faster: 8 s .. 50 ms | **B FADE** instant, or fade in 10 ms .. 4 s |
| 4 | the sound's FX knobs (engine host) | | | |

- Page 1 lives in knob slots 1-4, so **TE's LFO can move wave, mix and cutoff**. WAVE A / B step
  one multisound per detent (faster turns step more); the waves are taken at note on.
- **A DECAY + B FADE** give the classic workstation layers: a piano that decays while strings swell in.
- TE's envelope (ADSR) shapes the whole note afterwards, as for every engine.
- Multisounds with a fixed key (the 12 "NT" sounds, BASSTHUMB1 / 2) play one pitch on every key,
  as on the original.
- Engine list **station 1**; preset browser folder **station 1** with one preset **station 1** (M1 PIANO);
  **shift + blue turn** browses **P--** (the sound as it was before browsing: its settings come
  back, per sound slot) and 28 built-in sounds, round in both directions: M1 PIANO, PIANO PAD, UNIVERSE, HOUSE ORGAN,
  MAGIC ORGAN, E.PIANO, DWGS EP, CLAV, HARPSICHORD, FRETLESS, E.BASS, SYNTH BASS, STRINGS, CHOIR,
  BRASS, TENOR SAX, PAN FLUTE, KALIMBA, MALLETS, BELL PAD, KOTO, LORE, RHYTHM, WIND BELLS, BOTTLES,
  SAW LEAD, SQUARE, DIGI BELL.

## Screen

- The station (253 × 81, centred, room to the sides and to the knobs): **fader** = CUTOFF; left LEDs = A, B, F.ENV, RES on; **LCD** = the
  bank sound (`P01 M1 PIANO`, `*` when edited) and its waves (`A PIANO  B STRINGS +12`), or
  `P-- <wave A>` and `SHIFT+BLUE: SOUNDS` for P-- (the sound itself, in front of the bank); under it
  the 4 **page** segments (blue = current); right LEDs = A DECAY, B FADE, KEY TRK on; top right the **page**
  (P1-P4, red) above the blue **MIX** bar, green LED = B sounds; **knob** = this page's blue knob;
  **keyboard** C2-B6 (5-pixel keys, room around it) lights the keys being played.
- Lit keys: a key held on the OP-1 lights its own note until it is released (the octave from the
  voice it started, rounded, so glide / LFO / bend at the note start do not move it); notes
  without a key (sequencer, MIDI, CHORD+'s extra notes) flash for ~145 ms. Folded into C2-B6 by
  octaves. (TE keeps rendering a voice through its release and beyond, so rendering alone does not
  mean sounding.)
- Below, as on every SDK engine: the page's four knob names and bars, the next page; page 4 = the
  sound's FX (engine host).
- Design after "01 Station · overview" (1280 × 640 = 4 × the OP-1's screen); bench screenshots:
  `build/demo/m1_screen.png`, `build/demo/m1_screen_fx.png` (`sim/run_m1test.py`).

## Data

The samples are not ours: they are never committed or packed. `gen_m1.py` converts *your* own
sample set (a SoundFont of the 100 multisounds) into `build/` (git-ignored, skipped by `features.py pack`): `m1_data.inc` (the tables,
compiled in), `m1_rom.bin` (the sample data, ~1.4 MB) and `m1_rom.inc` (the same as a C array for
the host tests and the sample bench). Without them `m1_core.c` uses the built-in waves
(`m1_builtin.inc`, own, MIT).

**Loading** (v0.4): `m1_rom.bin` goes into the update as `content/m1_rom.bin` (an optional content
file). The first time a sound slot holds a STATION 1 sound, `m1_main` (main loop) opens
`/yaffs2/content/m1_rom.bin`, takes a buffer from TE's sample memory (VisualDSP heap 1, the heap
`SampleBuffer.cpp` loads samples into: `TE_HEAP_MALLOC`), reads 64 KB per main-loop pass (the UI
keeps running, ~23 passes), checks the build's sum and hands the data to `m1_core`. Until then
notes are silent and the LCD's second line says `LOADING SAMPLES n/100`; errors stay there in red:
`SAMPLE FILE MISSING`, `SAMPLE FILE DAMAGED`, `NO SAMPLE MEMORY FREE` (TE's samples fill heap 1;
retried every ~2 s). The buffer stays until power-off. Device (2026-10-18): all presets sound, also when booting straight into a STATION 1 sound; the
load is over before the screen shows it (well under a second). Why not static data outside the SDK region:
that memory is TE's two malloc heaps (`features/meminfo`, results).

```sh
python3 features/m1/gen_m1.py --sf2 "<path>/<sample set>.sf2"
python3 tools/compose.py recipes/op1plus.json <path>/op1_246.op1     # then flash as usual
```

| step | what | why |
|---|---|---|
| 1 | cut at the loop end (looped) / the −60 dB tail (one-shots, 64-sample fade) | nothing after the loop is ever played |
| 2 | short loops (≤ 4096 samples) tuned to their exact period | single-cycle waves in tune to 0.0 cents (one was 7 cents flat by its root key) |
| 3 | resample each sample to 1.8-2.4 × its measured bandwidth (smoothed spectrum above −60 dB), ≥ 16 kHz; samples of ≤ 8 KB (as ADPCM) keep their source rate | many samples carry little above 11 kHz; lower rates put ADPCM noise and interpolation images into the audible band (v0.4's organ 2 at 8 kHz: +8 dB at 5-10 kHz, +20 dB at 10-16 kHz) |
| 4 | loops stay exactly periodic: integer loop length L', the rate follows (rate × L' / L) | no clicks, no pitch change |
| 5 | octave-down copies (half rate each) where a sample plays high: up to C8 the step stays ≤ 1 for single cycles (nothing folds above 44.1 kHz) and ≤ 4 for sampled sounds; picked at note on (≤ 1 / ≤ 2) | right pitch and less CPU on high notes (pre-rendered instead of computed) |
| 6 | 4-bit block ADPCM: 16 samples in 9 bytes (shift + one of 8 predictors, then nibbles); PCM16 for tiny samples (< 2048) | 3.6 × smaller than 16 bit; the decoder is a few adds per sample |
| 7 | fit `--budget` (default 1 500 000 bytes): the rate factor drops 2.4 → 1.8 until it fits | the update's 15 MiB limit (op1plus 14.77 MB) and TE's sample memory |

Result for the full set (6.32 MB of 16-bit samples): **1.40 MB** (1 469 179 bytes),
100 multisounds, 220 samples + 167 octave-down copies (249 KB of them), rate factor 2.3, ADPCM SNR
median 42.8 dB (noise follows the signal level per block, so it sits under the sound rather than as
hiss). Single-cycle waves stay 16-bit. Stored rates per zone:
piano 16-44.1 kHz, strings 22-25.8 kHz, organ 2 22-32 kHz (source rate), clav 37.7-44.1 kHz
(v0.4: piano from 12.2, organ 2 from 7.9 kHz).

Measured (v0.5): each multisound at keys 36-84 against its source played back ideally
(band-limited), the energy our playback adds at 2-20 kHz (third octaves where ours is > 3 dB
above): median over the 100 multisounds −53 dB of the note (v0.4: −44 dB). The rest is ADPCM noise
(16-bit at v0.4's rates: −65 dB). Noise-shaped ADPCM encoding was tried: 1-5 dB less noise at the top,
20-30 dB more below, not used. 4-point Hermite interpolation was tried: no fewer images than linear
(the images come from the stored rate), not used.

Listen before flashing: `build/demo/m1_demo.wav` (the 28 bank sounds) and `build/demo/m1_ab.wav`
(piano, strings, organ 2, brass, choir, clav: the source, then the converted sample).

## Drum kits

`m1_drumkit.py` makes two OP-1 drum sampler kits from the sample set's drum samples. Built
into the image they are factory kits: drum preset browser, folder **station 1**, `m1 drums` and
`m1 perc` (`content_files`: `content/audio/factory_drum/<name>.raw` + a `drum_presets` row, both left
out when the kits were not made). The `.aif` files also work on a stock OP-1 (disk mode, below).

```sh
python3 features/m1/m1_drumkit.py "<path>/<drum samples folder>"
```

- `build/drum/m1 drums.aif` (7.1 s, 614 KB), keys low F upwards: KICK 1-3, SIDE STICK, SNARE 1-4,
  CLAPS, CLOSED HH 1-2, OPEN HH 1-2, SHAKER, TAMBOURINE, TOM 1-2, E.TOM, RIDE, CRASH, COWBELL,
  CONGA 1-2, RAP.
- `build/drum/m1 perc.aif` (5.1 s, 444 KB): KICK 1, SNARE 1, SIDE STICK, CLAPS, CLOSED HH1,
  OPEN HH2, COWBELL (so it plays on its own), then TIMBALES 1-2, WHIP, POLE, BLOCK, FINGER SNAP,
  DROP, VIBE HIT, HAMMER, METAL HIT, PLUCK, FLEXATONE, WIND BELLS, TUBULAR, BELL RING, METRONOME 1-2.
- In the image: run `m1_drumkit.py` before `compose.py` (the kits add ~1.08 MB to the update,
  op1plus 14.77 MB of the OP-1's 15 MiB limit).
- On a stock OP-1: copy both `.aif` into the drum folder in disk mode (e.g. `drum/m1/`); TE imports
  them on unplug.
- Format as the OP-1 writes it (AIFC `sowt` 16-bit mono 44.1 kHz; FVER, COMM, APPL `op-1` + kit
  JSON, SSND; positions = frame × 4058). Tails cut at −60 dB, levels as in the source, 64 frames of
  silence between hits. The tool parses each file back and checks every key's region.

## How

- Host: `features/enginehost`, tag 3: slots 5-8 = `0x3000 | hi << 6 | lo` (hi = page 2, lo =
  page 3), slots 1-4 = page 1 as 0..16383 (all inside drwave's ranges).
- Note on (`m1_note_on`): key from TE's increment (nearest semitone), zone of each multisound
  (osc B at key + B PITCH), the octave-down copy for that pitch.
- Per 128-sample voice block (`m1_render`): each oscillator decodes exactly the source samples the
  block needs into a buffer (whole ADPCM blocks with one header read; predictor history restored at
  the loop start, the loop's block replayed up to it), then interpolates linearly at the block's
  mean TE increment (88.2 kHz). Step cap: 4 (ADPCM) / 16 (PCM16), reached only by bends / LFO
  beyond C8. Levels (A decay, B fade, mix) and filter coefficients per 32 samples; filter:
  trapezoidal state-variable low-pass (Simper), skipped when fully open with no envelope and no
  resonance.
- Cost (sim, cycles per 128-sample voice block; TE's voltage engine 16.7k, TRIO up to 25k):
  built-in waves: one 7.5k, two + filter 15.6k, everything on 16.6k; sample data (keys 36..108):
  worst one wave 22.3k (C8), two waves 30.5k (both at C8).

## Tests

1. `features/m1/test_m1_core.c` (60 checks with the sample data, 32 without; ASan / UBSan clean):
   decoder bit-exact with `gen_m1.py` (test vectors, two loop wraps, loop start off a block
   boundary); key ↔ increment for all 128 keys; pitch (441 Hz, B an octave up; C7 and C8 through
   the octave-down copies); every multisound at 6 keys: no clipping, sounding, loops keep sounding;
   filter closes / envelope opens / resonance bounded; A DECAY, B FADE, MIX; restart identical;
   every page-2 / page-3 parameter, MIX and CUTOFF stepped through its range: its effect moves one
   way only (catches 0.2's A DECAY); with the data: 100 multisounds within the budget, the 14 fixed-key sounds, single cycles in
   tune (±3 cents) at C4 / C7 / C8.
2. `sim/run_m1test.py`: plain drwave untouched, tag 3 routing, 441 Hz pitch, restart, 4 pages,
   WAVE A / B one multisound per detent, page-1 clamp, 6-bit fields (B PITCH max +24, other field
   untouched, tag kept), bank (8 in range, wraps), page-4 FX knob, engine-list rename ("station 1"
   only), cycles, screens (`m1_screen.png`, `m1_screen_fx.png`), keyboard: a held key lit long
   after its note on (voice starting 2 semitones off), dark after release; with the data, a second run measures it (worst cycles, < 30k with every stage on).
3. `sim/run_m1load.py` (with the data): the loader against stub TE file / heap calls serving
   `build/m1_rom.bin`: no STATION 1 sound = nothing loaded; file missing; sample memory full, retried
   after ~250 passes and not before; one damaged byte = DAMAGED and silent; the good file in 64 KB
   steps, the data byte for byte in the heap buffer, notes sound, nothing loaded twice. Screens
   `m1_load_progress.png`, `m1_load_nomem.png`, `m1_load_ready.png`.
4. Demos `build/demo/m1_demo.wav`, `build/demo/m1_ab.wav`, screen `build/demo/m1_screen.png`.

## Limits

- Mono voices (the source's pan is ignored); no velocity (TE gives engines none).
- Waves are taken at note on: turning WAVE A / B changes the next note.
- Notes bent / modulated far beyond C8 stop rising at the step cap.
