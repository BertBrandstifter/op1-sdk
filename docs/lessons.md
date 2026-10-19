# Lessons learned

## 2026-10-18 — MONO 3 0.3

### Audio / DSP

- 0.2 was "sim-verified" and sounded wrong on the device: the bench checked routing, ranges and
  monotonic parameters, not the sound. Its bank's cutoffs sat at the bottom of a 20 Hz-based scale, so
  plucks closed the filter within ~60 ms: single clicks. Listening tests and level / brightness
  measurements over the whole bank catch what parameter sweeps cannot.
- A ladder's resonance sits below its poles when the feedback is delayed: tuning the table so the
  *resonance* lands on the cutoff (and the passband falls where it falls) sounded better than an
  analogue-accurate ladder.
- Blackfin: 64-bit multiplies / divisions per 32 samples (cutoff in Hz) cost more than the whole
  ladder loop; 16 × 16 mixer multiplies (`(int16_t)(y >> 2) * lv`) and a tanh whose unit is the
  limit (no scaling multiplies around it) took the voice from 43.8k to 38.3k cycles; a parameter table indexed by id (not searched) 36.8k.

### OP-1 / engine host

- **More than 48 bits per sound**: slots 1-3 keep their LFO-able value in the upper bits and fields
  in the low 6 (the LFO's offset only moves the continuous value by < 0.4 %); the render reads the
  fields from the stored knobs (`TE_SYNTH_KNOBS`, found by slots 5-8, which the LFO never touches),
  not from the copy TE hands over. Slot 4 (0..32767) holds 15 bits when it is not LFO-able.
- drwave's voice vtable has no note-off (the slot after note_on returns 0): an engine only learns
  a key up from the key events; sequencer / MIDI notes end by TE's envelope.

## 2026-10-18 — STATION 1, drum kits, disk space

### OP-1 / TE internals found

- **`user/tape_c.raw` is disk mode's FAT volume** (384 MiB), not a tape file. The tape reads and
  writes its audio in place inside `tape/track_N.aif` (track offsets from `fat_manager`,
  `0x019970a6`); the album's two 6-minute stereo sides are reserved inside it at full length
  (121 MB, padded with an AIFF `SKIP` chunk). Disk mode needs no copying because of this; it also
  means a compressed tape would mean rewriting both the tape storage and disk mode.
- **TE's boot check only rebuilds a smaller image** (`0x019972c4`: `size < limit`); a larger one is
  mounted as it is. Shrinking the image needed `!=` (`features/noalbum`). The rebuild itself is
  TE's own path ("Creating FAT image") and is reversible: flash the old image, TE rebuilds back.
- **YAFFS frees space lazily**: truncated / overwritten chunks count as free only after garbage
  collection erases their blocks (mostly when writing). The disk panel's FREE lags behind; the file
  sizes (SUM) show the real change.
- **Factory drum kit** = `content/audio/factory_drum/<name>.raw` (exactly the AIF's SSND PCM) + a
  `drum_presets` row (same JSON as the AIF's `op-1` APPL chunk). Picking a kit copies the `.raw`
  into the slot file `user/audio/drum/drum_<n>.raw`; the sampler loads that whole file into RAM
  (`SampleBuffer.cpp`). Drum positions are frames × 4058 with sub-frame offsets.
- **Edit modes** (`TE_EDIT_MODE`, read live with the disk panel): synth 0, drum 1, tape 2, mixer 3,
  tempo 4, album 6, sequencer 7. Key 44 opens the album.
- **The album recorder (and other hot code) is in encrypted L1**: its limits cannot be read or
  patched; guard it from the outside (key filter) instead.
- **TE keeps rendering a voice through its release and beyond**: "rendered recently" is not
  "sounding". Note starts carry glide / LFO / bend: `inc[0]` at note on is not the played note —
  take the note from the key event, only the octave from the voice.
- **Limits**: the update must stay below 15 MiB (now 14.36 MB); the SDK region is 2 MB of RAM
  (`0x01e00000`, ~336 KB free). Duplicated default slot samples cost ~2 MB of the update.
- **The OP-1 has 64 MB of SDRAM; TE's image defines ~25 MB, but the rest is not free**: it is TE's
  two VisualDSP malloc heaps (table `0x018e628c`): heap 0 `0x100` + 16 MB, heap 1 `0x03040000` +
  15.75 MB (samples, updates). Static data linked there loaded fine but TE's heap headers landed in
  it at every boot (features/meminfo). "Unchanged while I watched" is not "free": find the owner
  (here the heap table) before using memory. To use more RAM, allocate it from TE's allocators:
  big read-only data = a content file of the update, read at runtime (`TE_FILE_*`) into a
  `TE_HEAP_MALLOC(TE_HEAP_SAMPLES, n)` buffer from the main loop (STATION 1's samples, m1 0.4).

### Audio / DSP

- Many 44.1 kHz samples carry little above 11 kHz: measuring each sample's real bandwidth
  and resampling to ~2× it halved the data at no audible cost. 4-bit block ADPCM (16 samples in 9
  bytes) then gives ~35 dB, signal-following noise; 6.32 MB → 0.93 MB for all 100 multisounds.
- Loops must stay exactly periodic when resampled: integer loop length, the rate follows.
  Short loops can be tuned to their exact period (one single-cycle saw was 7 cents flat by its root key).
- **Pre-render instead of compute** (octave-down copies, "mipmaps"): right pitch and bounded CPU on
  high notes. Single-cycle waves need a step ≤ 1 at 88.2 kHz, or harmonics fold back from above
  44.1 kHz (TE's decimation only removes 22-44 kHz).
- Blackfin GCC spills registers in loops with many live values: split into tight passes (decode the
  block's source samples into a buffer, then interpolate) — 61k → 7.7k cycles per voice block.
- Sim benches load `op1plus` at `0x01E00000` with drivers at `0x01F00000`: large data does not fit
  next to them, so bench builds define `SDK_BENCH` (built-in waves); a second run measures the real
  data alone.

### How we work

- **Measure, don't read**: A DECAY ran backwards (0 = hold, 1 = fastest) and reading the code missed
  it; a sweep test (every parameter through its range, its effect may only move one way) found it
  and now guards all of them. Check that a new test fails on the old bug.
- **Check the measurement before blaming the code**: B an octave up interferes with A's own 2nd
  harmonic by phase (use a fifth); band-limited waves ripple around zero (count crossings with
  hysteresis); a narrow Goertzel misses a pitch that is a few cents off.
- **One risky device change per flash**, read-only probes first (disk panel before changing the
  disk); verify patched bytes against stock and disassemble new instructions before flashing.
- **Say what is verified and what is assumed**: "the album only uses what is recorded" was a guess
  and wrong; the measurement (image layout) corrected it.
- TE's data stays local (`build/`, never committed or packed); third-party sample data is never packed (the
  STATION 1 library is committed in `features/m1/build/` with a NOTICE, not MIT); `content_files` with
  `optional` leave a file and its SQL out together, so no preset row points at missing audio.
