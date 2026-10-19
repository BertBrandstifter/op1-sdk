# Album off: its disk space back

The OP-1 keeps disk mode's FAT volume as one file, `user/tape_c.raw` (384 MiB): the tape tracks,
the album sides, the drum / synth exports live inside it, in place (the tape reads and writes its
audio right in `tape/track_N.aif`). The album's two sides are 6 minutes of stereo each, reserved at
full length: **121 MB**. noalbum makes them 1 s long and the image **256 MiB**, so about **128 MB
more free flash** (e.g. 75.8 → ~204 MB on the disk panel).

## Open it on the OP-1

Nothing to open: the album screen (album key) shows "ALBUM IS OFF"; REC and PLAY do nothing there. Not part of `op1plus`.

**First boot after flashing:** the image size changed, so TE rebuilds disk mode's volume ("Creating
FAT image, this takes a few minutes..."): the tape and the album are empty afterwards; drum / synth
sounds and presets live outside the image and stay.

## How

| addr | stock | new | what |
|---|---|---|---|
| `0x019761e6` / `0x019761ea` | `0x18000000`, `0xbfffc` | `0x10000000`, `0x7fffc` | FAT disk geometry (size, sectors) |
| `0x01996e80` | `== 0x18000000` | `== 0x10000000` | image open: a mismatch rebuilds the image (TE's own path) |
| `0x019972c0` / `0x019972c4` | `size < 0x18000000` | `size != 0x10000000` | boot: image create ("Creating FAT image"); `!=` so an existing larger image (384 MiB) is rebuilt too (0.2; 0.1 kept the old image) |
| `0x01a9a098` | `0x18000000` | `0x10000000` | image file size (`0x01a99fb4`, called only by `fat_manager`) |
| `0x019987c0` / `0x019987e6` | 15 876 000 frames | 44 100 | album side A / B created (6 min → 1 s, stereo) |
| `0x01996cf2` | 15 876 000 frames | 44 100 | album import from disk mode: longer files are deleted by TE |

- The album recorder is in TE's encrypted L1 code: its own length limit is unknown, so it must
  never run into a 1 s file (it would write past it into the next file of the image). `na_key`
  (first on `on_key`) takes REC and PLAY on the album screen (`TE_EDIT_MODE` 6, read on the device
  with the disk panel), with their key ups.
- Space inside the 256 MiB: tape 127 MB, album 0.35 MB, ~125 MB for drum / synth exports and files
  dropped in disk mode.

Status: experimental. 0.1 on the device: booted fine but kept the 384 MiB image (the boot check only rebuilt smaller ones); 0.2 rebuilds it: device-checked, "Creating FAT image" ran fine, `tape_c.raw` 256 MiB. Right after, the
disk panel's FREE rose only 75.3 → 82.5 MB: YAFFS counts deleted chunks as free once their blocks are
erased, which its garbage collection does lazily (mostly when writing).
License: MIT.
