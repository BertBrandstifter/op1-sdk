# PLAY tape probe (debug)

Before the PLAY tape screen (`new_pattern_tape.md`) is built, this panel answers on the device:

1. **Mix point:** can PLAY add its audio to TE's output after TE renders a block? Which output
   word is left and which is right? Does the volume knob act on audio added there?
2. **Disk:** can we read `play/` on disk mode's FAT volume (inside `user/tape_c.raw`), and does
   TE leave an unknown root folder alone?
3. **Read speed:** how fast do reads from the main loop go, and how bad is the worst read? This
   decides whether `A1` can stream.
4. **Sample heap:** how much of TE's sample memory can PLAY get for preloaded loops?
5. **CPU:** what do the 8-stream mix and the SDK engines (per voice) cost?
6. **Keys:** the key ids and edit mode of `tape`, `shift + tape` and T1-T4.

Nothing runs before the panel is opened once. The probe never writes to the disk. The only thing
it changes is the sound, and only while the test tone is on.

## Build and flash

Not part of `op1plus`. Build the debug image (op1plus + playprobe, same boot text) and flash it:

```sh
python3 tools/compose.py recipes/op1play.json ~/path/to/op1_246.op1
python3 tools/flash.py build/apps/op1play/build/op1play.op1 --eject     # te-boot upload mode
```

Flash `op1plus` again when done. Conflicts with `cpuinfo` (both wrap the audio callback), and with
`meminfo` / `diskinfo` (same key).

## Controls

- **shift + speech:** panel on / off. The first opening wraps TE's audio callback. While the panel
  is open, the four knobs and their presses belong to the probe. Every other key still goes to TE.
- **blue press:** test tone off → BOTH → WORD 0 → WORD 1 → off.
  **blue turn:** tone level, -60 / -48 / -36 (default) / -24 / -18 / -12 dB.
- **green press:** scan the disk. **green turn:** scroll the file list.
- **white press:** read test on / off. **white turn:** read size 4 / 16 / 64 / 128 KB per pass.
- **orange turn:** mix-cost streams, 0..8. **orange press:** sample-heap test.

## Device procedure

Note down every number (a photo of the panel works).

**A. Mix point**
1. Open the panel. Start with headphones at a low volume. Blue press → `BOTH`: a ~441 Hz buzz at
   -36 dB.
2. `WORD 0` and `WORD 1`: which ear does each one play in?
3. With the tone on, turn the OP-1's volume knob. Does the tone get louder / quieter?
4. Play a synth note with the tone on, then note `TX PEAK`, `LOW16` and `AT`.
   - `LOW16` = `0000` means TE writes 16-bit samples in the top half of each word.
   - Any other value means TE uses more bits.

**B. Disk**
1. Connect the OP-1 in disk mode. Create a folder `play` at the root, next to `album/ drum/ synth/ tape/`.
   Copy in:
   - `A1.wav` (16-bit, 44.1 kHz, stereo, a few minutes long);
   - one or two `.aif` loops;
   - one 48 kHz file.
2. Leave disk mode. Open the panel, green press.
   - Each file should show `OK`; the 48 kHz file should show `NOT 44.1 KHZ`.
   - Note `FAT16/32`, the cluster size and the scan time (MS).
3. Power-cycle, open the panel, scan again. Are the files still there? (Does TE leave `play/`
   alone?)

**C. Read speed** (after B, so it reads `A1`)
1. White press at each size (white turn): 4, 16, 64, 128 KB. Wait about 5 s at each.
2. Note `KB/S`, `AVG`, `WORST` and `LOOP` (main-loop passes per second).
   - One stereo stream needs **176 KB/s**.
   - `LOOP` dropping a lot means the reads stall TE's main loop.
3. While it reads: do the screen and keys feel slower? Does the audio stay clean?

**D. Sample heap**
1. Orange press with the mix and read tests off, after normal use (a synth and a drum kit loaded).
2. Note `LIST` / `BIG` / `N` (TE's free list), then `ALLOC BIG` / `SUM` / `IN n` (what
   heap_malloc really gave).
3. Do not load sounds during the press: the test holds all free sample memory for a few
   milliseconds.

**E. CPU**
1. Orange turn to 8 streams. Note `PROBE` (the 8-stream mix cost) and `PEAK`.
2. Pick an SDK engine (e.g. ACID, MONO 3, WARP). Note `SDK ENG` with one note held, then with
   chords (all voices).
3. Back to 0 streams.

**F. Keys** (the bottom line: id, D/U, S = shift, then the edit mode)
1. Tape screen, panel closed: press `tape`, then `shift + tape`, then T1..T4. Open the panel and
   read the log.
2. Note whether TE did anything visible on `shift + tape`.

## How it works

- **Mix point:** the wrapper calls TE's audio callback, then adds the tone to the 128 words TE
  wrote (64 stereo frames, 32-bit I2S slots, MSB first). The add saturates.
  - The DMA descriptors (set up at `0x0191ab40`) point at L1 data memory (`0xff800370`), so the
    words should need no cache flush. The panel shows the address TE actually passes (`AT`). If it
    is in SDRAM, the probe flushes the words after adding the tone.
  - Audio added here comes after everything TE does in the callback. TE's own ring copy of the
    output (`0x019e1a88`, likely used for resampling) does not contain it.
- **Load:** cycles inside TE's callback, plus the probe's work after it, plus the SDK engines
  (`eh_cycles`). All against the block period, averaged per 0.25 s, the same as `cpuinfo`.
- **Mix cost:** 512 KB from the sample heap (eight 64 KB stereo streams). Every block mixes
  n × 64 frames with gain, sum and a soft knee. The result is discarded, so nothing is heard.
- **Disk:** the SDK libraries `fat` (read-only FAT16/32 reader, `include/op1/fat.h`) and
  `audiofile` (WAV / AIFF header parser, `include/op1/audiofile.h`).
  - It reads through `yaffs_pread` (`0x01a99ca8`, found from `yaffs_read` at `0x01a99c90`), so
    there is no seek call.
  - It accepts a superfloppy volume or an MBR partition. Each file's chain is checked against its
    size: a loop or a short chain shows `BROKEN CHAIN`.
  - Headers: WAV PCM or EXTENSIBLE, AIFF, AIFC `NONE` / `twos` / `sowt`. Hidden / system entries
    (the macOS `._` files) are skipped.
- **Read test:** one read per main-loop pass. It reads A1's data, or else the first good file, or
  else the raw image, in a loop.
- **Heap:** read-only walk of heap 1's free list. The descriptor comes from `0x01a9f2e2`; nodes are
  `{size, heap, next}` from the head at `+0x18`. Then the greedy test runs: binary search for the
  largest block, hold it, repeat up to 8 times, then free everything. TE's `free` (`0x01aa0040`)
  finds the heap from the block header, so it frees heap-1 blocks.

## Tests

- `lib/test_fat.c` and `lib/test_audiofile.c` (host, the libraries):
  - FAT16 and FAT32 (behind an MBR) images built in memory: long names, `._` files, fragmented
    files, a directory spanning many clusters; reads at many offsets and lengths, checked against
    the reference bytes; corrupt volumes: loops, short chains, garbage, FAT12;
  - every header case, truncated headers, read errors.
  - Also checked once against a volume written by macOS (newfs_msdos + Finder copies + afconvert
    files).
- `sim/run_pptest.py` (simulator, the composed probe with TE stand-ins):
  - nothing runs before opening; the wrap;
  - the tone in each word, and saturation;
  - the mix test: allocated, costs cycles, never reaches the output, freed;
  - a scan without an image, then with a FAT16 image;
  - the read test, the heap test, key / knob routing, re-wrapping.
  - Saves `build/demo/playprobe_screen.png`.

Status: experimental, device-run 2026-11-15 (results below).
License: MIT.

## Device results (2026-11-15, op1play)

- Tone audible; word 0 = left, word 1 = right; the volume knob does not change it (TE applies the
  volume inside its render). `AT FF800770` (L1, no flush), `LOW16 FFFF` (full 32-bit words).
- Load: STATION 1 idle TE 10.1 %; 8 mix streams ~3.5 %; ACID 3 one voice `SDK ENG` 15.3 %.
- Read speed (raw image, one read per main-loop pass, ~125 passes/s): 4 KB 506 KB/s (0.6 ms),
  16 KB 1636 KB/s (2.3 ms), 64 KB 3855 KB/s (9.3 ms), 128 KB 5021 KB/s (18.6 ms); worst 29 ms.
- Sample heap free list: 2.08-2.2 MB, one block.
- Disk: TE deletes every root entry except TAPE / ALBUM when leaving disk mode (DRUM / SYNTH are
  recreated on entering it); a `play/` folder does not survive.
- Keys: `shift + tape` opens TE's tape-erase screen (edit mode 10; T1-T4 there erase a track).
