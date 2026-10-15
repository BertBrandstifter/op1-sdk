# TRIO synth engine — spec

Status: **v0.1** (2026-10-13): built and sim-verified, awaiting device test. Code: `features/trio/`
(engine host, tag 4). Idea: sloop's TRIO (GPL, ideas only, own code): three chip oscillators with
ring and sync into a gritty multimode filter, early 8-bit home-computer style.

## What it is

| page | blue | green | white | orange |
|---|---|---|---|---|
| 1 | **WAVE** (16 sets) | **CUTOFF** 30 Hz .. 16 kHz | **RES** (gritty at the top) | **PW** 50 % .. 3 % |
| 2 | **INT 2** osc 2 interval −24..+24 st | **INT 3** osc 3 interval | **DETUNE** osc 2 up / 3 down, 0..50 ct | **MODE** LP BP HP NOTCH |
| 3 | **F.ENV** cutoff up to +6 octaves at note on | **F.DECAY** 5 ms .. 2 s | **DRIVE** into the filter | **NOISE** white noise in |
| 4 | the sound's FX knobs (engine host) | | | |

- Page 1 lives in knob slots 1-4, so **TE's LFO can move cutoff and pulse width** (the classic
  PWM / filter wobble: pick the element LFO with the knob as target).
- Pages 2 and 3 are 6-bit fields: intervals step one semitone per detent, the others 64 steps
  (faster turns step more).
- Sets: SAW3, PULSE3, TRI3, SAW+PLS, TRI+SAW, PLS+TRI, SAW&TRI and PLS&SAW (the AND of the two
  16-bit waves), PLS+NOISE, NOISE (pitched: a new value 16x a cycle), SYNC SAW / SYNC PLS / SYNC3
  (osc 2 restarts with the silent osc 1: INT 2 sweeps the sync timbre), RING (osc 1 triangle,
  sign flipped by the silent osc 3: INT 3 sets the ring ratio), RING+SAW, RING+SYNC.
- Engine list **trio**; preset browser folder **trio** with one preset **trio** (C64 BASS);
  **shift + blue turn** browses 16 built-in sounds: C64 BASS, SID LEAD, ARP CHIP, SYNC SCREAM,
  RING BELL, METAL, HOOVER, ACID, PWM PAD, NOISE HAT, SNARE, THUMP, BRASS, STRINGS, GRIT BASS,
  NOTCH SWEEP.

## How

- Host: `features/enginehost`, tag 4: slots 5-8 = `0x4000 | hi << 6 | lo` (hi = page 2, lo =
  page 3), page 1 in slots 1-4 as 0..16383; all inside drwave's ranges.
- DSP (`trio_core.c`), three passes per 128-sample block: (1) the oscillators, one loop per set
  (wave types as constants), naive waves at 88.2 kHz (the chip character; TE's decimation removes
  what lies above 22 kHz), osc 2 / 3 increments per block from TE's pitch x interval x detune;
  (2) noise, drive (each only when on); (3) a trapezoidal state-variable filter (Simper; stable at
  any cutoff, one pass per sample), coefficients 4x per block from CUTOFF + F.ENV, one loop per
  mode, states clamped so high resonance turns gritty.
- First version used a Chamberlin SVF: unstable near the top of the range once per sample
  (oscillated: TRI3 measured brighter than SAW3), and 2x oversampled it cost 37k+ cycles.
- Cost (sim, per 128-sample voice block): SAW3 12.8k, RING+SYNC + drive + noise 25.0k, + resonance
  + filter env 23.3k (WHEEL worst 24.3k, TE's voltage 16.7k).

## Tests

1. `features/trio/test_trio_core.c` (32 checks, ASan/UBSan clean): saw harmonics, every set
   sounds without clipping, open LP stable (triangle duller than saw), intervals, detune beats,
   sync keeps the fundamental, ring removes it, LP closes, HP removes the fundamental, filter
   envelope, PW, drive adds harmonics, extremes.
2. `sim/run_triotest.py`: plain drwave untouched, tag 4 routing, 441 Hz pitch, restart, 4 pages, page-1
   clamp, 6-bit fields (INT 2 max +24, other field untouched, tag kept), bank (16 in range,
   wraps), page-4 FX knob, engine-list rename, cycles, screen draw.
3. Demo `build/demo/trio_demo.wav`, screen `build/demo/trio_screen.png`.
