/* First pixels on the OP-1 screen from SDK code.
 *
 * seqids_touch()  runs on the first sequencer switch after boot and paints the
 *                 corner markers: top-left red, top-right green, bottom-right
 *                 blue, bottom-left white - a photo of the screen then tells
 *                 us the panel orientation and the real channel order.
 * midi_event()    runs on every MIDI message the dispatcher sees and draws
 *                 note-ons as white bars (x = note, height = velocity),
 *                 note-offs erase them, CCs as small blocks along the bottom.
 *
 * Framebuffer: 0x02000000, 16-bit pixels, 160 px/line, 320 lines, stride 512 B
 * (from TE's boot fill loop). PPI/DMA scans it continuously, so writes are
 * visible immediately; tearing is possible but harmless. */
#include <op1/te246.h>

#define FB      TE_FRAMEBUFFER
#define FB_W    TE_FB_WIDTH
#define FB_H    TE_FB_HEIGHT
#define FB_S    TE_FB_STRIDE

#define C_RED   TE_RGB565(31, 0, 0)
#define C_GREEN TE_RGB565(0, 63, 0)
#define C_BLUE  TE_RGB565(0, 0, 31)
#define C_WHITE TE_RGB565(31, 63, 31)
#define C_BLACK TE_RGB565(0, 0, 0)

static int markers_drawn;
unsigned long midi_total;

static void fb_pixel(int x, int y, unsigned short c)
{
    if ((unsigned int)x < FB_W && (unsigned int)y < FB_H)
        FB[y * FB_S + x] = c;
}

static void fb_rect(int x, int y, int w, int h, unsigned short c)
{
    int i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            fb_pixel(x + i, y + j, c);
}

static void draw_markers(void)
{
    fb_rect(0, 0, 8, 8, C_RED);
    fb_rect(FB_W - 8, 0, 8, 8, C_GREEN);
    fb_rect(FB_W - 8, FB_H - 8, 8, 8, C_BLUE);
    fb_rect(0, FB_H - 8, 8, 8, C_WHITE);
    markers_drawn = 1;
}

void seqids_touch(void)
{
    if (!markers_drawn)
        draw_markers();
}

void midi_event(const te_bytes *msg)
{
    unsigned int n = te_bytes_size(msg);
    unsigned int status, d1, d2;
    int x, h;

    if (!markers_drawn)
        draw_markers();
    midi_total++;
    if (n < 3)
        return;
    status = msg->begin[0] & 0xf0;
    d1 = msg->begin[1];
    d2 = msg->begin[2];
    if (status == TE_MIDI_NOTE_ON && d2) {
        x = (int)(d1 * FB_W / 128);
        h = (int)(d2 * (FB_H - 16) / 127) + 1;
        fb_rect(x, 0, 2, h, C_WHITE);
    } else if (status == TE_MIDI_NOTE_OFF || (status == TE_MIDI_NOTE_ON && !d2)) {
        x = (int)(d1 * FB_W / 128);
        fb_rect(x, 0, 2, FB_H - 16, C_BLACK);
    } else if (status == TE_MIDI_CC) {
        x = (int)(d1 * (FB_W - 8) / 128);
        fb_rect(x, FB_H - 14, 6, 6, d2 ? C_GREEN : C_BLACK);
    }
}
