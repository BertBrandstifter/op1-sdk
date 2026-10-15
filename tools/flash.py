#!/usr/bin/env python3
"""Copy an update to the OP-1's te-boot "Firmware Upload" disk (and only there).

    flash.py <update.op1> [--volume /Volumes/OP-1] [--eject]

How te-boot installs (te-boot 2.30, firmware.cpp, loop at te-boot SDRAM 0x6d4):
  - Firmware Upload mode (OP-1 off, hold COM while switching on, pick firmware upload)
    exposes a small FAT RAM disk formatted by te-boot itself (label "OP-1", buffer at 0x01000000).
  - After eject, every file whose name ends in ".OP1" is installed ("Found firmware package...",
    then "Done!" or "No firmware written. Try again."). Names containing "FRC" set a force flag,
    names containing "RBT" a reboot flag. Any name works otherwise.
  - The normal disk mode (volume with album/ drum/ synth/ tape/, ~400 MB) is user storage served by
    the main firmware. te-boot never reads it: an update copied there is silently dropped.
This tool refuses any volume that looks like the normal disk.
"""

import argparse, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import op1pack  # noqa: E402

USER_DIRS = {"album", "drum", "synth", "tape"}
MAX_RAMDISK = 64 << 20  # te-boot's RAM disk lives in 32 MB SDRAM; the user disk is ~400 MB


def candidates():
    return [os.path.join("/Volumes", v) for v in sorted(os.listdir("/Volumes"))]


def check_volume(vol):
    """return None if vol looks like the te-boot upload disk, else the reason it doesn't"""
    if not os.path.ismount(vol):
        return "not mounted"
    names = {n.lower() for n in os.listdir(vol)}
    if names & USER_DIRS:
        return "normal disk mode (has album/drum/synth/tape): te-boot never reads this disk"
    st = os.statvfs(vol)
    size = st.f_blocks * st.f_frsize
    if size > MAX_RAMDISK:
        return f"{size / 2**20:.0f} MB volume, too large for te-boot's RAM disk"
    return None


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("update")
    ap.add_argument("--volume", help="mount point (default: the one te-boot disk found)")
    ap.add_argument("--eject", action="store_true", help="eject after copying (starts the install)")
    a = ap.parse_args()

    members = op1pack.read(a.update)  # validates CRC + archive before touching the device
    if not any(m.name == "OP1_vdk.ldr" for m, _ in members):
        sys.exit(f"{a.update}: no OP1_vdk.ldr inside")
    name = os.path.basename(a.update)
    if not name.lower().endswith(".op1"):
        sys.exit("te-boot only installs files ending in .op1")

    if a.volume:
        vol = a.volume
        why = check_volume(vol)
        if why:
            sys.exit(f"{vol}: {why}")
    else:
        found = [(v, check_volume(v)) for v in candidates()]
        ok = [v for v, why in found if why is None and os.path.basename(v).upper().startswith("OP-1")]
        if len(ok) != 1:
            for v, why in found:
                print(f"  {v}: {why or 'candidate'}")
            sys.exit(
                "no unique te-boot upload disk. Switch the OP-1 off, hold COM while switching it on,"
                " choose firmware upload, then run again (or pass --volume)."
            )
        vol = ok[0]

    dst = os.path.join(vol, name)
    shutil.copyfile(a.update, dst)
    subprocess.run(["sync"], check=True)
    print(f"{a.update} -> {dst}")
    if a.eject:
        subprocess.run(["diskutil", "eject", vol], check=True)
        print("ejected: the OP-1 should show 'Found firmware package...' and then 'Done!'")
    else:
        print(f"now eject it (diskutil eject '{vol}') to start the install")


if __name__ == "__main__":
    main()
