# STATION 1 synth engine (feature m1) — spec

Status: **v0.8** (2026-10-20): the 100 multisounds are now the M1's own ROM samples (the KORG
Collection M1's ROM, extracted) instead of a SoundFont recorded from it: bit-exact source data at the
M1's own rate (31.25 kHz), no resampling, the M1's own key splits (468 zones, was 220), root keys,
fine tuning and zone levels (read from the M1 plugin's tables). Same compression (4-bit block ADPCM,
PCM16 for small samples), 1.43 MB; the room left in the budget keeps the 17 samples where ADPCM would
hiss most (E.Piano, BellRing, Brass 1 top zones ...) as PCM16. Measured against v0.7 through the
voice core: pitch median 0.0 cents (383 tonal notes, 4 beyond 10 cents), level within about 1 dB.
See **Data**. P--: the preset `station 1` plays all A (MIX 0, was all B: PIANO 16'), and once
 changed the LCD's second line shows wave B. v0.7 (2026-10-20): the 28 bank sounds now come from the M1's own internal program
bank (MPC-00, `Int_M1.syx`) instead of hand-picked waves: the original programs' multisounds,
octave transpositions and layer levels (`m1_bank.py` -> `m1_bank.inc`). Many M1 programs play a
multisound an octave down (16') or up: a one-oscillator program is played as B only at -12/+12
(STATION 1 has no A transpose), so its pitch matches the original. Filter / envelope settings stay
the v0.6 values. v0.6 (2026-10-20): clean output. The user heard crackle playing four high notes
together: the voices were too hot for TE's voice mix (one wave peaked at 72 % of full scale, and
MIX A..B added both layers at full level: +6 dB at the centre) and the open filter hard-clipped.
Now: a voice peaks at ~50 % (one wave 16000, was 24000), MIX is a real crossfade (the two layers
add to one wave's level), and a soft knee (linear to 3/4 full scale, as WHEEL) replaces the hard
clip. All 28 bank sounds at C6: peak 3-13k, RMS 1-10k. v0.5 (2026-10-19): cleaner samples, stored
at ≥ 16 kHz (small samples at their source rate), ~1.4 MB, no added hiss above 5 kHz. v0.4
(2026-10-18): the samples are a file of the
update, loaded at runtime into TE's sample memory (they no longer take ~0.95 MB of the 2 MB SDK
region; see **Data**). Code: `features/m1/` (engine host, tag 3). A late-80s-style sample
workstation voice: two of 100 sampled multisounds (piano, organs, strings, choir, brass, bells,
basses, single-cycle waves ...) layered, into a low-pass filter with its own envelope, and a bank
of 28 ready-to-play sounds. Own code; the samples (KORG M1 multisounds) are not ours: committed in
`build/` with a [NOTICE](build/NOTICE), not MIT, never packed (see **Data**).

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
- Multisounds with a fixed key (the 14 "NT" sounds, BASTHUMNT1 / 2 among them) play one pitch on every key,
  as on the original.
- Engine list **station 1**; preset browser folder **station 1** with one preset **station 1** (PIANO on A, MIX all A);
  **shift + blue turn** browses **P--** (the sound as it was before browsing: its settings come
  back, per sound slot) and 28 built-in sounds, round in both directions: M1 PIANO, PIANO PAD, UNIVERSE, HOUSE ORGAN,
  MAGIC ORGAN, E.PIANO, DWGS EP, CLAV, HARPSICHORD, FRETLESS, E.BASS, SYNTH BASS, STRINGS, CHOIR,
  BRASS, TENOR SAX, PAN FLUTE, KALIMBA, MALLETS, BELL PAD, KOTO, LORE, RHYTHM, WIND BELLS, BOTTLES,
  SAW LEAD, SQUARE, DIGI BELL.
- The bank's waves, transpositions and layer mixes are the original M1 programs' (`m1_bank.py`,
  see **Bank from the M1 program bank**). One-oscillator programs that the M1 plays an octave down
  or up (16'/4') are stored as **B only** with B PITCH -12/+12, so the pitch matches the original;
  on those sounds A DECAY has nothing to act on (TE's ADSR shapes the note).

## Screen

- The station (253 × 81, centred, room to the sides and to the knobs): **fader** = CUTOFF; left LEDs = A, B, F.ENV, RES on; **LCD** = the
  bank sound (`P01 M1 PIANO`, `*` when edited) and its waves (`A PIANO  B STRINGS +12`), or
  `P-- <wave A>` and `SHIFT+BLUE: SOUNDS` for P-- (the sound itself, in front of the bank), the
  second line `B <wave B> +12` once the P-- sound is changed (until browsing back to P--); under it
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
  `build/demo/m1_screen.png`, `build/demo/m1_screen_fx.png`, `build/demo/m1_screen_own.png` (a changed
  P--) (`sim/run_m1test.py`).

## Data

The samples are not ours (KORG M1 multisounds): the converted library is committed in `build/` with
a [NOTICE](build/NOTICE) (not MIT) and skipped by `features.py pack`, so the feature's zip stays
MIT-only. A fresh clone builds the image with them as it is. `gen_m1.py` regenerates them from the
M1 ROM (v0.8, `--rom`) or a SoundFont of the 100 multisounds (up to v0.7, `--sf2`) into `build/`:
`m1_data.inc` (the tables, compiled in), `m1_rom.bin` (the sample data, ~1.4 MB) and `m1_rom.inc` (the same as a C array for
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
python3 features/m1/gen_m1.py --rom "<path>/public_m1_samples" --pcm "<path>/full_decrypted_44k1_mono_FULL.wav"
python3 tools/compose.py recipes/op1plus.json <path>/op1_246.op1     # then flash as usual
```

**Source (v0.8): the M1 ROM.** `--rom` is the sample extraction of the KORG Collection M1's ROM
(`manifest.tsv` + one WAV per zone, 3386 zones; `TECHNICAL_SUMMARY.md` there). Only bank 0, slots
0-99 are used: the M1's 100 factory multisounds (910 WAVs, 3.71 MB of 16-bit samples); the M1EX /
card / drum sounds in the same folder are left out (no room). What the extraction does not carry:

- **Rate.** The WAVs say 44.1 kHz; the samples are at the M1's 31.25 kHz (209 sampled zones measured
  against their root keys: median 5.96 semitones sharp at 44.1 kHz = 31 255 Hz).
- **Key splits, root keys, tuning, levels.** Read from the M1 plugin (`--plugin`, default the
  installed `M1.vst`): its multisound table (20-byte records: zone start, flags, zone count, name)
  and zone table (24 bytes: top key, ?, fine tune in cents (signed), root key, level, ..., start,
  loop, end). Both tables are found by the factory names and checked zone by zone against the
  manifest's start / loop / end. A zone plays keys from the previous zone's top + 1 to its own top
  (the last one up to 127); its pitch at the root key is the root less the fine tune; root > 127
  marks an "NT" multisound (one pitch on every key, at the ROM rate). Level: 0.1 dB per step
  (negative = quieter; fitted to the SoundFont's zone levels); all samples × 0.79 (−2 dB, the
  SoundFont's median level), so the bank keeps v0.6's headroom.
- **Single cycles.** The 23 DWGS / synth waves (8 zones each) loop back before the WAV's start: the
  extraction kept half the cycle. Their loops (512 .. 4 samples, one cycle each) come from the full
  decrypted ROM (`--pcm`).
- Loops: loop = end is a one-shot; any other loop is played as the M1 does (looped). Step 2 below
  leaves loops of a fading tail alone.

| step | what | why |
|---|---|---|
| 1 | cut at the loop end (looped) / the −60 dB tail (one-shots, 64-sample fade) | nothing after the loop is ever played |
| 2 | short loops (≤ 4096 samples) tuned to their exact period | single-cycle waves in tune to 0.0 cents (one was 7 cents flat by its root key) |
| 3 | every sample at its source rate when that fits the budget (the M1 ROM: 31.25 kHz, fits); else resample each to 1.8-2.4 × its measured bandwidth (smoothed spectrum above −60 dB), ≥ 16 kHz; samples of ≤ 8 KB (as ADPCM) keep their source rate | the ROM's own data, nothing lost; resampling the ROM to its bandwidth was measured worse (Fretless +17 dB hiss in a third octave instead of +3). For the SoundFont (44.1 kHz): many samples carry little above 11 kHz; lower rates put ADPCM noise and interpolation images into the audible band (v0.4's organ 2 at 8 kHz: +8 dB at 5-10 kHz, +20 dB at 10-16 kHz) |
| 4 | loops stay exactly periodic: integer loop length L', the rate follows (rate × L' / L) | no clicks, no pitch change |
| 5 | octave-down copies (half rate each) where a sample plays high: up to C8 the step stays ≤ 1 for single cycles (nothing folds above 44.1 kHz) and ≤ 4 for sampled sounds; picked at note on (≤ 1 / ≤ 2) | right pitch and less CPU on high notes (pre-rendered instead of computed) |
| 6 | 4-bit block ADPCM: 16 samples in 9 bytes (shift + one of 8 predictors, then nibbles); PCM16 for tiny samples (< 2048) | 3.6 × smaller than 16 bit; the decoder is a few adds per sample |
| 7 | fit `--budget` (default 1 500 000 bytes): the rate factor drops 2.4 → 1.8 until it fits | the update's 15 MiB limit (op1plus 14.79 MB) and TE's sample memory |
| 8 | at the source rate with room left: samples whose ADPCM noise rises above the sound in a third octave (1 kHz up) stay PCM16, chosen to remove the most such noise (dB, 0/1 knapsack) in the room left | band-limited samples (E.Piano, MagicOrgan) have quiet upper bands that ADPCM fills with noise; noisy sounds (Rhythm, WindBells) mask theirs |

Result, v0.8 (the M1 ROM, 3.71 MB of 16-bit samples): **1.43 MB** (1 496 874 bytes), 100
multisounds, 468 samples at 31.25 kHz + 13 octave-down copies (9 KB), no resampling. PCM16: 323 samples
under 2048 (184 of them the single-cycle zones) and 17 kept for step 8 (E.Piano 2 z1-3, E.Piano 1 z2, BellRing,
MagicOrgan z2, Brass 1 z4-5, Flute z1/2/4, WindBells, Pick Bass z0/1/4, Choir z2, Lore z4); left as
ADPCM with noise above the sound in a third octave: MagicOrgan z0/z1 (+18 / +22 dB; as PCM16 about
115 KB more than the budget), Brass 1 z3 (+6), Piano z0 (+4) and 14 more at +0..+3 dB. ADPCM SNR median
34.3 dB (lower than the SoundFont's 42.8: source-rate data fills its band, so less to predict).
Through the voice core against v0.7 (keys 36-84, every multisound): pitch median 0.0 cents (383
tonal notes; 4 beyond 10 cents: Piano +5..7, Sitar 2 +6..9, the M1's own tuning), level within
about 1 dB. Cost: worst one wave 10.1k cycles per voice block (v0.7: 22.3k), two waves + every
stage 20.6k (the ROM's lower rate: fewer source samples per output block).

v0.5-v0.7 (the SoundFont, 6.32 MB of 16-bit samples): **1.40 MB** (1 469 179 bytes),
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

## Bank from the M1 program bank

The 28 bank sounds used to be hand-picked multisounds. They are now generated from the M1's
internal factory programs (the `MPC-00` card dump, `Int_M1.syx`, SysEx 4C: 100 programs of 143
bytes; that file is program data, not samples). `m1_bank.py` reads the program for each bank
sound and takes its oscillator multisounds, octave transpositions and oscillator levels:

```sh
python3 features/m1/m1_bank.py --syx "<path>/MPC-00/Int_M1.syx"
# -> features/m1/m1_bank.inc (committed; included by m1.c)
```

| bank sound | M1 program | | bank sound | M1 program |
|---|---|---|---|---|
| M1 PIANO | Piano 16' | | STRINGS | Strings |
| PIANO PAD | MagicPiano | | CHOIR | Choir |
| UNIVERSE | Universe | | BRASS | Brass 1 |
| HOUSE ORGAN | Organ 2 | | TENOR SAX | Tenor Sax |
| MAGIC ORGAN | MagicOrgan | | PAN FLUTE | Pan Flute |
| E.PIANO | E.Piano 1 | | KALIMBA | Kalimba |
| DWGS EP | E.Piano 3 | | MALLETS | SynMallet |
| CLAV | Clav | | BELL PAD | Bell Ring + Strings |
| HARPSICHORD | Harpsicord | | KOTO | Koto Trem |
| FRETLESS | Fretless | | LORE | Lore |
| E.BASS | Slap Bass | | RHYTHM | Rhythm |
| SYNTH BASS | SynthBass1 | | WIND BELLS | WindBells |
| SAW LEAD | Solo Synth | | BOTTLES | Bottles |
| SQUARE | SQUARE WAVE | | DIGI BELL | Digi-Bells |

The M1's octave field is a program transposition relative to the multisound (`Piano 16'` is
`Piano 8'` an octave down). A one-oscillator program at -1/+1 is stored as B only
(`MIX` = 100 %, `B PITCH` = -12/+12) because STATION 1's A oscillator has no transpose; a two-
oscillator program is normalized so A is the oscillator at octave 0. Layer mixes come from the
original oscillator levels; the filter, F.ENV / F.DECAY, resonance, key tracking, A DECAY and
B FADE settings are STATION 1's own (the M1 VDF/envelopes differ).

## Drum kits

`m1_drumkit.py` makes two OP-1 drum sampler kits from the sample set's drum samples. Built
into the image they are factory kits: drum preset browser, folder **station 1**, `m1 drums` and
`m1 perc` (`content_files`: `content/audio/factory_drum/<name>.raw` + a `drum_presets` row, both left
out when the kits were not made). Both kits are committed in `build/drum/` (not MIT, see the NOTICE).
The `.aif` files also work on a stock OP-1 (disk mode, below).

```sh
python3 features/m1/m1_drumkit.py "<path>/<drum samples folder>"
```

- `build/drum/m1 drums.aif` (7.1 s, 614 KB), keys low F upwards: KICK 1-3, SIDE STICK, SNARE 1-4,
  CLAPS, CLOSED HH 1-2, OPEN HH 1-2, SHAKER, TAMBOURINE, TOM 1-2, E.TOM, RIDE, CRASH, COWBELL,
  CONGA 1-2, RAP.
- `build/drum/m1 perc.aif` (5.1 s, 444 KB): KICK 1, SNARE 1, SIDE STICK, CLAPS, CLOSED HH1,
  OPEN HH2, COWBELL (so it plays on its own), then TIMBALES 1-2, WHIP, POLE, BLOCK, FINGER SNAP,
  DROP, VIBE HIT, HAMMER, METAL HIT, PLUCK, FLEXATONE, WIND BELLS, TUBULAR, BELL RING, METRONOME 1-2.
- In the image: committed, so `compose.py` takes them as they are (the kits add ~1.08 MB to the update,
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
  beyond C8. Levels (A decay, B fade, mix) and filter coefficients per 32 samples; MIX A..B is a
  linear crossfade (the layers add to one wave's level); one voice peaks at ~half scale (GAIN_ONE
  16000) so chords leave room in TE's voice mix; output goes through a soft knee (linear to 3/4 of
  full scale, then approaches full scale) instead of a hard clip. Filter: trapezoidal
  state-variable low-pass (Simper), skipped when fully open with no envelope and no resonance.
- Cost (sim, cycles per 128-sample voice block; TE's voltage engine 16.7k, TRIO up to 25k):
  built-in waves: one 7.5k, two + filter 15.6k, everything on 16.6k; sample data (keys 36..108):
  worst one wave 10.1k, two waves 15.6k, two waves + every stage 20.6k (v0.8, the M1 ROM at 31.25
  kHz; v0.7's SoundFont: one wave 22.3k, two waves 30.5k, both at C8).

## Tests

1. `features/m1/test_m1_core.c` (65 checks with the sample data, 37 without; ASan / UBSan clean):
   decoder bit-exact with `gen_m1.py` (test vectors, two loop wraps, loop start off a block
   boundary); key ↔ increment for all 128 keys; pitch (441 Hz, B an octave up; C7 and C8 through
   the octave-down copies); every multisound at 6 keys: no clipping, peak ≤ 18000 (headroom for
   TE's voice mix), sounding, loops keep sounding; MIX is a crossfade (A = B at the centre is as
   loud as A alone) and a voice stays short of full scale; filter closes / envelope opens /
   resonance bounded; A DECAY, B FADE, MIX; restart identical;
   every page-2 / page-3 parameter, MIX and CUTOFF stepped through its range: its effect moves one
   way only (catches 0.2's A DECAY); with the data: 100 multisounds within the budget, the 14 fixed-key sounds, single cycles in
   tune (±3 cents) at C4 / C7 / C8. v0.8 (the M1 ROM data): 65 checks pass, ASan / UBSan clean.
2. `sim/run_m1test.py`: plain drwave untouched, tag 3 routing, 441 Hz pitch, restart, 4 pages,
   WAVE A / B one multisound per detent, page-1 clamp, 6-bit fields (B PITCH max +24, other field
   untouched, tag kept), bank (8 in range, wraps), page-4 FX knob, engine-list rename ("station 1"
   only), cycles, screens (`m1_screen.png`, `m1_screen_fx.png`, `m1_screen_own.png`), P--: hint until
   changed, then wave B on the second line (wave A unchanged on top), hint again after browsing back; keyboard: a held key lit long
   after its note on (voice starting 2 semitones off), dark after release; with the data, a second run measures it (worst cycles, < 30k with every stage on).
3. `sim/run_m1load.py` (with the data): the loader against stub TE file / heap calls serving
   `build/m1_rom.bin`: no STATION 1 sound = nothing loaded; file missing; sample memory full, retried
   after ~250 passes and not before; one damaged byte = DAMAGED and silent; the good file in 64 KB
   steps, the data byte for byte in the heap buffer, notes sound, nothing loaded twice. Screens
   `m1_load_progress.png`, `m1_load_nomem.png`, `m1_load_ready.png`.
4. `features/m1/m1_bank.py` (needs the MPC-00 dump): checks the known multisound numbers of the
   original programs (Piano 16' = 0, Strings = 31, Choir = 30, Organ 2 = 6 ...), keeps every B pitch
   within -24..+24 and every wave in range, then writes `m1_bank.inc`.
5. Demos `build/demo/m1_demo.wav`, `build/demo/m1_ab.wav`, screen `build/demo/m1_screen.png`.

## Limits

- Mono voices (the source's pan is ignored); no velocity (TE gives engines none).
- Waves are taken at note on: turning WAVE A / B changes the next note.
- One-oscillator M1 programs at 16'/4' use B only (A has no transpose), so A DECAY has no effect
  on them; use TE's envelope.
- Notes bent / modulated far beyond C8 stop rising at the step cap.
