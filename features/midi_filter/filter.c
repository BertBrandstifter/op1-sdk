/* First project with no assembly: build.py generates the trampoline from project.json.
 *
 * filter_init()   "init": runs once, before the first hook call.
 * filter_midi()   entry hook on the TE MIDI dispatcher with "filter": true. It gets
 *                 the dispatcher's argument (const te_bytes *msg); returning nonzero
 *                 drops the message (TE never sees it), 0 lets it through.
 *
 * Here: drop CC number `blocked_cc` (default 12) and count what passes. A real app
 * could remap notes, split channels, or turn CCs into its own controls. */
#include <op1/te246.h>

unsigned int filter_blocked_cc;
unsigned long filter_init_runs, filter_midi_passed, filter_midi_dropped;

void filter_init(void)
{
    filter_blocked_cc = 12;
    filter_init_runs++;
}

int filter_midi(const te_bytes *msg)
{
    if (te_bytes_size(msg) >= 3 && (msg->begin[0] & 0xf0) == TE_MIDI_CC
        && msg->begin[1] == filter_blocked_cc) {
        filter_midi_dropped++;
        return 1;
    }
    filter_midi_passed++;
    return 0;
}
