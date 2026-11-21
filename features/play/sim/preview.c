/* preview: PLAY's screens (py_draw.c) drawn on the host from sample models, written as raw
 * RGB565 frames (320 x 156, the panel's layout) for preview.py. No TE, no simulator: seconds, so
 * the look can be tuned before the 4-minute sim bench (run_playtest.py) checks the real thing.
 *
 *   cc -I include -I features/play features/play/sim/preview.c -o preview && ./preview <outdir>
 *   (preview.py does this and converts the frames to PNG) */
#include <stdio.h>
#include <string.h>

/* lib/gfx.c, but gfx_begin without TE's canvas registers (static fb is reachable here) */
#define gfx_begin gfx_begin_te
#include "../../../lib/gfx.c"
#undef gfx_begin
void gfx_begin(uint16_t *buf) { fb = buf; }

#include "../../enginehost/eh_aa.c"
#include "../py_song.c"
#include "../py_draw.c"

static uint16_t frame[320 * GFX_STRIDE];

static void begin(void)
{
    memset(frame, 0, sizeof(frame));
    gfx_begin(frame);
    eh_aa_begin(frame);
    eh_aa_gamma = 1;
}

static void save(const char *dir, const char *name)
{
    char path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s.rgb565", dir, name);
    f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    fwrite(frame, sizeof(frame), 1, f);
    fclose(f);
    printf("%s\n", path);
}

static py_hm header(const char *title, int playing)
{
    py_hm h;

    memset(&h, 0, sizeof(h));
    h.title = title;
    h.title_col = D_TEXT;
    h.num = -1;
    h.ctx = playing ? "4 TRK \1 CPU 31" : "4 TRK";
    h.bpm = 120;
    h.bar = 3;
    h.beat = 2;
    h.row = -1;
    h.state = (uint8_t)playing;
    return h;
}

static void track(py_lt *t, int k, int type, const char *name, int slot, int seq, int song, int voices,
                  int eco, int level, int mute, uint16_t steps, int now)
{
    memset(t, 0, sizeof(*t));
    t->k = (uint8_t)k;
    t->type = (uint8_t)type;
    t->name = name;
    t->slot = (int8_t)slot;
    t->seq = (int8_t)seq;
    t->seq_song = (uint8_t)song;
    t->voices = (uint8_t)voices;
    t->eco = (uint8_t)eco;
    t->level = (int8_t)level;
    t->mute = (uint8_t)mute;
    (void)steps;
    (void)now;
    t->cpu = 0xffff;
}

/* the design's sample: 4 tracks, playing, track 1 selected and muted */
static void list_model(py_lm *m, int layout, int playing)
{
    int now = playing ? 4 : -1;

    memset(m, 0, sizeof(*m));
    m->layout = (uint8_t)layout;
    m->n = 4;
    m->cursor = 0;
    m->add_room = 1;
    m->add_type = PS_SYNTH;
    m->add_ok = 1;
    m->cut = -1;
    track(&m->t[0], 0, PS_SYNTH, "ACID 3", 1, 1, 0, 2, 1, -9, 1, 0x8a51, now);
    track(&m->t[1], 1, PS_SYNTH, "MALLET", 2, 1, 0, 4, 1, -9, 0, 0x2421, now);
    track(&m->t[2], 2, PS_SYNTH, "STATION 1", 2, 2, 1, 4, 1, 0, 0, 0xc1c4, now);
    track(&m->t[3], 3, PS_DRUM, "DRUM", 3, 5, 1, 0, 0, -9, 0, 0xd4b5, now);
    if (playing) {
        m->t[1].cpu = 120;
        m->t[2].cpu = 160;
        m->t[3].cpu = 40;
        py_d_meter_set(0, -60 * 16, -60 * 16);
        py_d_meter_set(1, -8 * 16, -6 * 16);
        py_d_meter_set(2, -2 * 16, 0);
        py_d_meter_set(3, -20 * 16, -12 * 16);
    } else {
        int k;

        for (k = 0; k < 8; k++)
            py_d_meter_set(k, -60 * 16, -60 * 16);
    }
}

static void draw_list(const py_lm *m, int playing)
{
    py_hm h = header("PLAY", playing);

    h.nobar = 1;
    begin();
    py_d_header(&h);
    py_d_list(m);
}

/* a song: 4 columns, chains, a loop 04-07 x2, cursor on row 05 column 3 */
static ps_song song;

static void song_model(py_sm *m, uint8_t *top, uint8_t *left)
{
    static const uint8_t cells[10][4] = {
        { 0x01, 0xff, 0xff, 0x01 }, { 0xff, 0xff, 0xff, 0x01 }, { 0x02, 0x01, 0xff, 0x02 },
        { 0xff, 0xff, 0x01, 0x02 }, { 0x02, 0x03, 0x02, 0x03 }, { 0x03, 0xfe, 0x02, 0x03 },
        { 0x04, 0x03, 0x04, 0x04 }, { 0x04, 0xff, 0xff, 0x04 }, { 0xff, 0x05, 0x04, 0x05 },
        { 0xff, 0xff, 0xff, 0x06 } };
    int r, c, i;

    py_song_clear(&song);
    for (c = 0; c < 4; c++)
        song.trk[c].type = c == 3 ? PS_DRUM : PS_SYNTH;
    for (r = 0; r < 10; r++)
        for (c = 0; c < 4; c++)
            song.cell[r][c] = cells[r][c];
    for (i = 1; i < 7; i++) {                   /* chains 01..06: a few rows each */
        song.chain[i][0].pat = (uint8_t)i;
        song.chain[i][1].pat = (uint8_t)(i + 1);
    }
    memset(m, 0, sizeof(*m));
    m->s = &song;
    m->n = 4;
    for (c = 0; c < 4; c++) {
        m->list[c] = (uint8_t)c;
        m->c[c].k = (uint8_t)c;
        m->c[c].type = song.trk[c].type;
        m->c[c].slot = (int8_t)(c == 3 ? 3 : c == 0 ? 1 : 2);
        m->c[c].cue = -1;
    }
    m->c[0].head = 5;
    m->c[1].head = 4;
    m->c[2].head = 5;
    m->c[3].head = 6;
    m->playing = 1;
    m->row = 5;
    m->col = 2;
    m->top = top;
    m->left = left;
    m->loop_a = 4;
    m->loop_b = 7;
    m->loop_n = 2;
}

static void draw_song(py_sm *m)
{
    py_hm h = header("SONG", 1);

    h.ctx = "SLOT 02";
    h.bare = 1;
    h.row = m->row;
    h.bar = 9;
    h.beat = 1;
    h.loop_a = m->loop_a;
    h.loop_b = m->loop_b;
    h.loop_n = m->loop_n;
    begin();
    py_d_header(&h);
    py_d_song(m);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    static py_lm m;
    static py_sm s;
    uint8_t top = 0, left = 0;

    list_model(&m, 0, 1);
    draw_list(&m, 1);
    save(dir, "lanes");
    list_model(&m, 0, 0);
    draw_list(&m, 0);
    save(dir, "lanes_stopped");
    list_model(&m, 1, 1);
    draw_list(&m, 1);
    save(dir, "rows");

    /* 6 tracks: an AUDIO track, a busy SYNTH, a drum; cursor on track 6, then on ADD */
    list_model(&m, 0, 1);
    m.n = 6;
    track(&m.t[4], 4, PS_AUDIO, "REND01.AIF", 0, 0, 0, 0, 0, -12, 0, 0, 7);
    m.t[4].amode = 1;
    track(&m.t[5], 5, PS_SYNTH, "MONO 3", 5, 3, 0, 1, 0, -6, 0, 0x1111, 4);
    m.t[5].busy = 1;
    m.t[5].pend = 1;
    m.cursor = 5;
    py_d_meter_set(4, -10 * 16, -9 * 16);
    py_d_meter_set(5, -14 * 16, -12 * 16);
    draw_list(&m, 1);
    save(dir, "lanes_6");
    m.cursor = 0;
    draw_list(&m, 1);
    save(dir, "lanes_6_left");
    m.cursor = 5;
    m.cursor = 6;
    m.add_type = PS_DRUM;
    draw_list(&m, 1);
    save(dir, "lanes_6_add");
    m.layout = 1;
    m.cursor = 5;
    m.t[3].err = "NOT IN PLAY";
    m.t[2].solo = 1;                            /* solo: the others dimmed */
    draw_list(&m, 1);
    save(dir, "rows_6");
    m.cursor = 6;
    m.note = "INSERT AGAIN: ADD";
    m.note_col = D_GREEN;
    m.note2 = "DRUM";
    draw_list(&m, 1);
    save(dir, "rows_6_add");

    song_model(&s, &top, &left);
    draw_song(&s);
    save(dir, "song");
    s.n = 3;                                    /* three tracks: wider columns */
    s.col = 1;
    draw_song(&s);
    save(dir, "song3");
    return 0;
}
