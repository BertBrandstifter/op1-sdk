# l1dump — firmware plaintext dumper

Reads the decrypted firmware out of a running OP-1 and sends it back to the Mac.

**Why:** the stock `OP1_vdk.ldr`'s L1 blocks (instruction SRAM `0xffa00000` 48 KB, plus L1 data
blocks) are encrypted in the update file — te-boot decrypts them into the chip's L1 SRAM while
loading, so the plaintext exists **only inside the running chip** (`docs/internals.md`, "Booting
TE's firmware in the simulator (blocked)"). A dump of a booted device is the one way to get it.

**What it dumps** (on the first **shift + speech**, into a CRC-framed static image; nothing runs
at boot, so a fault can never hang the device before the UI — a lesson from the first hardware
run, which hung at the boot screen with boot-time capture + callback wrap):

| tag | region | size |
|---|---|---|
| `L1I0` | L1 instruction SRAM `0xffa00000` | 48 KB |
| `L1A0` | L1 data A `0xff800000` | 32 KB |
| `L1B0` | L1 data B `0xff900000` | 32 KB |
| `SCRP` | scratchpad `0xffb00000` | 4 KB |

Image layout: `"OP1D"` + version + region table (tag/addr/len/crc32 each) + payloads + image
crc32 (all little-endian). Nothing is written anywhere besides this heap buffer; the feature
is read-only towards TE state.

The three data regions are copied with TE's `memcpy`; L1 instruction SRAM cannot be read by a
normal core load (a `memcpy` from `0xffa00000` faults and hangs the device — the first hardware
press did exactly that), so it is read 8 bytes at a time through the core's DTEST data-array
interface, the same sequence as the kernel's `arch/blackfin/mm/isram-driver.c`.

## Get the dump over USB (no cable beyond the OP-1's USB)

1. On the Mac, make the carrier file and copy it to the OP-1 in disk mode:

   ```sh
   python3 tools/l1dump_rx.py mkfile 256 L1DUMP.BIN
   # copy L1DUMP.BIN to the OP-1's disk root (COM -> disk mode)
   ```

2. Flash the dumper image (your own firmware copy, as always):

   ```sh
   python3 tools/compose.py recipes/op1dump.json /path/to/op1_246.op1
   python3 tools/flash.py build/apps/op1dump/build/op1dump.op1 --eject
   ```

3. Restart the OP-1. The bottom-left status line shows `L1D WAIT`. Press **shift + speech**
   once: it captures (~3 ms) and starts the modem — the line becomes `L1D 116K TX ON USB 0`.
   Press shift + speech again to stop the sound; the capture and the USB patching stay armed.
   Enter disk mode and copy `L1DUMP.BIN` back to the Mac. The USB counter rises as the Mac
   reads. (macOS may cache the file — eject and re-plug if it looks unchanged.)

4. Carve the dump:

   ```sh
   python3 tools/l1dump_rx.py extract L1DUMP.BIN analysis/l1dump/
   # -> l1i.bin l1a.bin l1b.bin scrp.bin + layout.json (all CRC-verified)
   ```

How it works: the OP-1's USB disk is served by the main firmware from a backing file
(`src/bf/usb_ms_yaffs.cpp` on the smx USB device stack). The feature after-hooks the sector
read (`read(buf, sector, nsectors)`, entry `0x01a7153c`): whenever the Mac reads sectors that
belong to the carrier file — recognizable by their 512-byte `OP1DMPY1` + index + `0xA5` fill
pattern — the hook replaces them with the dump slices in the read buffer. Everything happens
in RAM; the disk image on NAND is never written. A block is only replaced when the whole
512-byte pattern matches, so no user data can be hit by accident.

## Get the dump over audio (fallback, works without USB)

Press **shift + speech**: the status line shows `TX ON` and the image loops as an FSK stream
on the audio out (8 tones, 3 bits/symbol, one symbol per 64-frame audio block, a sync tone
framing each 120-byte packet; ~8 minutes per loop, looping until toggled off or reboot).

Record it (line out into an interface, or the speaker into the Mac's mic) to a WAV — any
sample rate, mono is enough (`afconvert -f WAVE -d LEI16 recording.m4a recording.wav` if you
recorded with QuickTime Player). Then:

```sh
python3 tools/l1dump_rx.py decode recording.wav analysis/l1dump/
```

The decoder finds the device clock from the sync tone, re-synchronizes at every packet and
CRC-checks each one; missed packets are taken from the next loop. Press shift + speech again
to stop the sound.

## Restoring

Flash your original `op1_246.op1` — te-boot is never touched, and the dumper changed nothing
persistent on the device (the carrier file can simply be deleted in disk mode).

## Legal note

The dumps are Teenage Engineering's firmware: for your own device, keep them local like the
other analysis artifacts (`analysis/` is gitignored) and don't redistribute them.

## Bench

`features/l1dump/sim/run_l1dumptest.py` proves the whole path without hardware: planted L1
content -> boot capture -> audio callback -> WAV -> decoder -> byte-exact compare, plus the
carrier-block patching of the USB read hook. Under `SDK_BENCH` the regions are 1 KB each so
the captured audio fits the simulator's memory, and L1I is an SDRAM mirror: this bench runs in
the simulator's default environment, which has no MMU/DTEST device, so the DTEST read itself is
device-only (the operating environment that has it is used by `tools/op1sim.py`).

## The dump also boots the firmware in the simulator

The plaintext L1 is what the simulator was missing: `tools/op1sim.py OP1_246.op1
analysis/l1dump/` merges it with the LDR's plaintext SDRAM and starts TE from reset in the
operating environment (see `docs/internals.md`). The dump is TE-derived: keep it local.
