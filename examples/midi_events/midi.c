/* MIDI event recorder: counts messages by type and remembers the last
 * note/CC/bend. All state is in .bss (zeroed by the boot loader). Reading
 * these from TE-side code or a future debug channel is left to the app. */
#include <op1/te246.h>

unsigned long midi_total, midi_note_on, midi_note_off, midi_cc, midi_bend, midi_other;
unsigned int last_note, last_velocity, last_cc_num, last_cc_val, last_bend;

void midi_event(const te_bytes *msg)
{
    unsigned int n = te_bytes_size(msg);
    unsigned int status;

    midi_total++;
    if (n < 3) {
        midi_other++;
        return;
    }
    status = msg->begin[0] & 0xf0;
    switch (status) {
    case TE_MIDI_NOTE_ON:
        last_note = msg->begin[1];
        last_velocity = msg->begin[2];
        midi_note_on++;
        break;
    case TE_MIDI_NOTE_OFF:
        last_note = msg->begin[1];
        midi_note_off++;
        break;
    case TE_MIDI_CC:
        last_cc_num = msg->begin[1];
        last_cc_val = msg->begin[2];
        midi_cc++;
        break;
    case TE_MIDI_PITCHBEND:
        last_bend = msg->begin[1] | (msg->begin[2] << 7);
        midi_bend++;
        break;
    default:
        midi_other++;
        break;
    }
}
