/* On-screen probe: shows what the SDK sees, drawn over every TE frame.
 *
 *   probe_overlay(buf)   after TE renders a frame (GUI thread call site 0x019ab8b2): draws the panel
 *   probe_key(id, down)  entry of on_key 0x019b6c74: last key id (decimal) + up/down + count
 *   probe_knob(k, diff)  entry of on_knob 0x019b6c0c: last knob, its raw diff and running sum
 *   probe_midi(msg)      entry of the MIDI dispatcher: last 3 bytes
 *
 * Panel (top-left of the UI, black box):
 *   K 38 DN  N 12        key 38 (shift) went down, 12 key events so far
 *   KNOB 0 1024 S 3072   knob 0 (blue?), last diff, sum of diffs
 *   MIDI 90 3C 64
 *   [F800][07E0][001F]   colour swatches: expected red / green / blue if the panel is RGB565
 * Press keys one by one and note the ids -> fills in the TE_KEY_* table. */
#include <op1/te246.h>
#include <op1/gfx.h>

static volatile int last_key = -1, last_down, key_events;
static volatile int last_knob = -1, last_diff, knob_sum[4];
static volatile uint8_t midi[3];
static volatile int midi_events;

void probe_key(int key, int down)
{
    last_key = key;
    last_down = down;
    key_events++;
}

void probe_knob(int knob, int16_t *diff)
{
    last_knob = knob;
    last_diff = *diff;
    if ((unsigned int)knob < 4)
        knob_sum[knob] += *diff;
}

void probe_midi(const te_bytes *msg)
{
    unsigned int i, n = te_bytes_size(msg);

    for (i = 0; i < 3; i++)
        midi[i] = i < n ? msg->begin[i] : 0;
    midi_events++;
}

void probe_overlay(uint16_t *buf)
{
    int x, y = 2;

    gfx_begin(buf);
    gfx_rect(0, 0, 164, 52, GFX_BLACK);

    x = gfx_text(2, y, "K ", GFX_WHITE, 2);
    x = gfx_dec(x, y, last_key, GFX_WHITE, 2);
    x = gfx_text(x, y, last_down ? " DN  N " : " UP  N ", GFX_WHITE, 2);
    gfx_dec(x, y, key_events, GFX_WHITE, 2);

    y += 12;
    x = gfx_text(2, y, "KNOB ", GFX_WHITE, 2);
    x = gfx_dec(x, y, last_knob, GFX_WHITE, 2);
    x = gfx_text(x, y, " ", GFX_WHITE, 2);
    x = gfx_dec(x, y, last_diff, GFX_WHITE, 2);
    x = gfx_text(x, y, " S ", GFX_WHITE, 2);
    gfx_dec(x, y, last_knob >= 0 && last_knob < 4 ? knob_sum[last_knob] : 0, GFX_WHITE, 2);

    y += 12;
    x = gfx_text(2, y, "MIDI ", GFX_WHITE, 2);
    x = gfx_hex(x, y, midi[0], 2, GFX_WHITE, 2);
    x = gfx_text(x, y, " ", GFX_WHITE, 2);
    x = gfx_hex(x, y, midi[1], 2, GFX_WHITE, 2);
    x = gfx_text(x, y, " ", GFX_WHITE, 2);
    gfx_hex(x, y, midi[2], 2, GFX_WHITE, 2);

    y += 12;
    gfx_rect(2, y, 30, 10, 0xf800);
    gfx_rect(36, y, 30, 10, 0x07e0);
    gfx_rect(70, y, 30, 10, 0x001f);
}
