#!/usr/bin/env python3
"""Build PLAY's demo songs (gen_demos.c) with the host cc and write SONG13..16.PSL next to this file.

    python3 features/play/demos/gen_demos.py [out dir] [--preview <dir>]

--preview also writes a rough WAV of each song (plain waveforms, not PLAY's engines) to hear the
notes and the arrangement on a computer. The files are PLAY song slots: copy them into the disk's play_s/ folder in disk mode (they go into
free slots when disk mode is left), or let the image carry them (feature.json content_files:
/yaffs2/content/play_demo_13..16.psl, copied into free slots once at the first boot)."""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PLAY = os.path.dirname(HERE)
FEAT = os.path.dirname(PLAY)
SDK = os.path.dirname(FEAT)


def main():
    args = sys.argv[1:]
    pv = []
    if "--preview" in args:
        i = args.index("--preview")
        pv = [args[i + 1]]
        os.makedirs(pv[0], exist_ok=True)
        del args[i:i + 2]
    out = args[0] if args else HERE
    exe = os.path.join(tempfile.mkdtemp(), "gen_demos")
    src = [os.path.join(HERE, "gen_demos.c"), os.path.join(PLAY, "py_song.c"), os.path.join(PLAY, "py_slot.c"),
           os.path.join(FEAT, "noteroll", "nr_core.c"), os.path.join(FEAT, "drumroll", "dr_core.c")]
    inc = ["-I", os.path.join(SDK, "include"), "-I", PLAY, "-I", os.path.join(FEAT, "noteroll"),
           "-I", os.path.join(FEAT, "drumroll")]
    subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-O2", *inc, "-o", exe, *src, "-lm"], check=True)
    sys.exit(subprocess.run([exe, out, *pv]).returncode)


if __name__ == "__main__":
    main()
