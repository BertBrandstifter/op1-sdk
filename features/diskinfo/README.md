# Flash usage panel (debug)

Shows what fills the OP-1's internal flash (YAFFS2, `/yaffs2`): free and total space, then the
biggest folders and files in MB: album sides, tape, user samples, factory content, databases.
Read-only: it only lists directories and reads file sizes, with the same TE functions TE's own
`/log/files.txt` dump uses (`fat_utils.cpp`, `0x01998c14`). Nothing is ever written.

## Open it on the OP-1

Not part of `op1plus`. Build the debug image and flash it:

```sh
python3 tools/compose.py recipes/op1disk.json ~/path/to/op1_246.op1
python3 tools/flash.py build/apps/op1disk/build/op1disk.op1 --eject     # te-boot upload mode
```

## Controls

- shift + speech: show / hide the panel. Each opening measures once (a short pause is normal).

## Reading it

- `FREE x OF y MB`: `yaffs_freespace` / `yaffs_totalspace` of `/yaffs2`.
- `FILES n SUM s`: number and total size of all files found; `MODE n`: TE's edit mode now (which
  screen has the keys and knobs, `TE_EDIT_MODE`), live, to look up a screen's id.
- Rows: paths below `/yaffs2`, cut after 2 parts (`content/display`) or 3 under `user/` and
  `content/audio/` (`audio/drum`, `album/side_a.wav`, `audio/factory_drum`); `user/` and `content/`
  are left off. Biggest first, the rest as OTHER.
- `total - free` is larger than `SUM`: YAFFS keeps spare blocks and its own checkpoint.

## TE functions (R246)

| addr | function |
|---|---|
| `0x01a9b450` / `0x01a9b648` / `0x01a9b74c` | `yaffs_opendir` / `yaffs_readdir` (name at +11) / `yaffs_closedir` |
| `0x01a9a69c` | `yaffs_stat`: mode at +0x08 (0x4000 dir, 0x8000 file), size at +0x1c |
| `0x01a9b1c0` / `0x01a9b284` | `yaffs_freespace` / `yaffs_totalspace` (free chunks × bytes per chunk, under the YAFFS lock) |

Found in TE's walker `0x01998a5a` and dump `0x01998c14`. TE writes that dump to `/log/files.txt`
when disk mode is entered with shift held, but then skips exporting the drum / synth samples
to the disk, so this panel is the safer way to look.

Status: experimental. License: MIT.
