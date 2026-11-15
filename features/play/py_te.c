/* py_te: the disk-sync hook (play.h, README.md).
 *
 * TE deletes the disk image's root entries in one cleanup function, 0x01997de4(dir, depth),
 * called with ("", 0) at three sites (static RE 2026-11-15): 0x01997584 (disk mode left, no
 * changes in synth/drum/album/tape), 0x01996986 (the sync after a restart, after TE's own import)
 * and 0x01996f6a (the defragment check, every boot and once more on the way out). PLAY's call-site
 * hook runs before each; py_import_stage does nothing when there is no play/ folder (any more). */
#include <op1/te246.h>
#include "play.h"

void py_cleanup_hook(const char *dir, int depth)
{
    if (depth == 0 && dir && !dir[0])
        py_import_stage();
}
