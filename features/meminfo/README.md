# SDRAM usage panel (debug)

Answers: how big is the OP-1's SDRAM, how full is TE's heap, and which parts of SDRAM does TE
(or anything else) ever write at runtime? The stock image only defines about 25 MB of RAM; the
second screen buffer at `0x03000000` means the chip is at least 48 MB (so almost certainly
64 MB), and nothing in TE's code refers to `0x00029294–0x01000000`, `0x02800000–0x03000000` or
above `0x03040000`. If those stay untouched under heavy use, large SDK data (M1 samples, kits)
can be loaded there instead of the 2 MB SDK region.

Read-only: it reads memory-mapped registers and memory, and writes only its own variables.

## Open it on the OP-1

Not part of `op1plus`. Build the debug image (op1plus + meminfo, same boot text) and flash it:

```sh
python3 tools/compose.py recipes/op1mem.json ~/path/to/op1_246.op1
python3 tools/flash.py build/apps/op1mem/build/op1mem.op1 --eject     # te-boot upload mode
```

## Controls

- shift + speech: off → page 1 (map) → page 2 (CPLBs) → off. The **first** opening starts the
  scan; it keeps running when the panel is closed (until power-off).

## Page 1

- `SDRAM n MB  SDBCTL x`: bank size from the memory controller (`EBIU_SDBCTL` EBSZ);
  `CPLB ON/OFF`: data CPLBs enabled (`DMEM_CONTROL` bit 1); `PASS n`: full scans done (pass 1 is
  the baseline); `AT`: the scan's address now.
- `HEAP USED / PEAK`: TE's heap statistics (TLSF 2.4 at `0x0106b7cc`, 8.25 MB, used at +4, peak
  at +8); `FREE / BIG / N`: free bytes, largest free block, block count, by walking the blocks
  (`TORN` = TE changed the heap mid-walk, try again next update).
- The map: one cell = 64 KB, one row = 4 MB (label in MB). Blue = TE's / our image unchanged,
  amber = TE's / our image changed (expected: heap, buffers), green = other memory unchanged,
  teal = other memory all zero and unchanged, **red = other memory that changed**, grey = not
  covered by a data CPLB right now (skipped), dark = not scanned yet.
- `CHANGED OUTSIDE TE/SDK`: the first 4 red ranges.

## Page 2

`DMEM_CONTROL` and the 16 data CPLB entries (address, page size, raw data word; grey = invalid).
Shows which memory the CPU can access without a CPLB miss.

## How

- Main loop (`main_tick`): 16 KB per pass, a 64 KB chunk every 4 passes, rotate-xor hash and a
  zero count per chunk; ~30 s for a full 64 MB pass. A chunk is read only if the currently
  valid data CPLBs cover all of it (or CPLBs are off), so the scan never causes an access the
  CPU could fault on; uncovered chunks are retried every pass.
- Heap walk (GUI thread, about once a second): blocks from `pool + 0xc88`, `size & ~3` = payload,
  bit 0 = free, next = block + 8 + size, size 0 = end; bounded and range-checked, read-only.
- Layout knowledge: TE heap from `te_new.cpp`'s init (`0x01a5e37c`: `init_memory_pool(0x840000,
  0x0106b7cc)`, TLSF signature `0x2a59fa59`); known ranges from the stock LDR blocks.

## Test plan

1. Open the panel once right after boot (starts the scan), wait for `PASS 2`.
2. Use the OP-1 hard: record and lift / paste tape, sample (synth + drum), album, USB disk mode,
   browse and load presets, all engines and FX.
3. Read page 1 (red ranges, heap peak) and page 2 (CPLBs); power-cycle and repeat once.

## Results (device, R246, 2026-10-18)

- **SDRAM 64 MB** (`SDBCTL 0x0025`). TE heap (TLSF): 4.46 MB used, peak 5.18 MB, largest free
  3.4 MB.
- Data CPLBs (static, all 4 MB): L1 data + every 4 MB of SDRAM **except `0x02800000`** (no entry,
  never accessed). `0x00000000–0x00ffffff` locked; `0x02000000` and `0x03000000` write-through
  (screen buffers); `0x02c00000` without the dirty bit.
- **The memory outside TE's image is not free: it is TE's two malloc heaps** (VisualDSP heap table
  at `0x018e628c`): heap 0 = `0x00000100` + 16 MB (`malloc`, written at runtime from `0x00030000`
  up, growing with tape editing), heap 1 = `0x03040000` + 15.75 MB, used by `SampleBuffer.cpp`
  (synth / drum samples) and `tar.cpp` (firmware updates from disk mode). The map's "unchanged"
  only meant "not written while measured": sample buffers are allocated at boot and filled when a
  sample loads.
- How it was found: M1's samples linked at `0x03100000` (a second LDR block) loaded, but some
  presets were silent. A per-32-byte check against the build showed 3 bad lines, the same on
  every boot (cold too): `00081348 00000001 00000000` at `0x031426b0` and `0x031c39f8`, 0x81348
  apart = heap 1 block headers (sample buffers of 529,224 bytes), plus a few changed bytes next to
  them. The data went back into the SDK region.
- Left over: `0x02c00000–0x02ffffff` (mapped, outside the heaps; te-boot zero-fills
  `0x02040000–0x02f40000` when it starts) and `0x02800000–0x02bfffff` (no CPLB). Not used.
