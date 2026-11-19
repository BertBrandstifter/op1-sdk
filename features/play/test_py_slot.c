/* host test for py_slot.c (tools/test_all.py builds it with its sibling py_slot.c; py_song.c is
 * included here).
 *
 * A song with patterns through a memory file: the round trip (song, swings, stored and empty
 * patterns), the file's size and masks, the header summary, every damaged byte refused with
 * nothing changed, a short / long file, other pattern sizes, no file, a pattern that cannot be
 * read at once (retried) or never (BUSY), a failing write; the patterns the song plays, which
 * patterns differ from the ones now, a load keeping some, a v0.10.0 file (no "plays" field); the
 * sounds the song plays (the track's, a chain row's, CUR's) stored, compared, put back, kept or
 * refused; a PSL1 file (v0.10 / v0.11: no sounds); where an imported file goes (py_slot_pick). */
#include <stdio.h>
#include <string.h>
#include "py_song.c"
#include "py_slot.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define NSZ 20                                  /* stand-in pattern sizes */
#define DSZ 36

static uint8_t pats[2][PSL_PATS][DSZ];          /* the editors' patterns */
static uint8_t empty[2][DSZ];
static uint8_t file[200000];
static uint32_t flen, fmax = sizeof(file);
static int get_fail, put_n, write_fail_at = -1, writes;
static uint32_t scratch[(DSZ + 3) / 4];
static uint8_t snd[PSL_SOUNDS][PSL_SND];        /* TE's synth sounds */
static int sput_n, sok_no = -1;                 /* sok refuses this slot */
static uint8_t songbuf[PS_FILE_BYTES];

static int get(void *ctx, int kind, int p, void *out)
{
    (void)ctx;
    if (get_fail) {
        if (get_fail > 0)
            get_fail--;
        return -1;
    }
    memcpy(out, pats[kind][p], kind ? DSZ : NSZ);
    return 0;
}

static void put(void *ctx, int kind, int p, const void *in)
{
    (void)ctx;
    memcpy(pats[kind][p], in, kind ? DSZ : NSZ);
    put_n++;
}

static int wr(void *ctx, const void *buf, uint32_t n)
{
    (void)ctx;
    if (writes++ == write_fail_at || flen + n > fmax)
        return -1;
    memcpy(file + flen, buf, n);
    flen += n;
    return 0;
}

static int rd(void *ctx, uint32_t off, void *buf, uint32_t n)
{
    (void)ctx;
    if (off >= flen)
        return 0;
    if (n > flen - off)
        n = flen - off;
    memcpy(buf, file + off, n);
    return (int)n;
}

static uint32_t file_crc(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xffffffffu;
    int k;

    while (n--)
        for (c ^= *p++, k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    return ~c;
}

static int sget(void *ctx, int sl, uint8_t *rec)
{
    (void)ctx;
    memcpy(rec, snd[sl], PSL_SND);
    return 0;
}

static void sput(void *ctx, int sl, const uint8_t *rec)
{
    (void)ctx;
    memcpy(snd[sl], rec, PSL_SND);
    sput_n++;
}

static int sok(void *ctx, int sl, const uint8_t *rec)
{
    (void)ctx;
    (void)rec;
    return sl != sok_no;
}

static psl_io io = { 0, { NSZ, DSZ }, { empty[0], empty[1] }, scratch, get, put, wr, rd, 0, sget, sput, sok };

static ps_song song, back;

static void make(void)
{
    int k, p, i;

    py_song_clear(&song);
    song.trk[0].type = PS_SYNTH;
    song.trk[1].type = PS_DRUM;
    song.trk[2].type = PS_AUDIO;
    strcpy(song.trk[2].file, "SONG.WAV");
    song.trk[0].level = -6;
    song.cell[0][0] = 3;
    song.cell[4][1] = 5;
    song.cell[9][2] = PS_ON;
    song.chain[3][0].pat = 2;
    song.chain[5][0].pat = 7;
    song.chain[5][1].pat = 8;
    song.chain[5][1].tsp = -12;
    song.trk[0].sound = 4;                      /* sounds: the track's 4, chain 3 row 1's 6 */
    song.chain[3][1].pat = 2;
    song.chain[3][1].snd = 6;
    song.chain[5][0].snd = 1;                   /* (a DRUM row's kit: no synth sound) */
    for (k = 0; k < PSL_SOUNDS; k++)
        for (i = 0; i < PSL_SND; i++)
            snd[k][i] = (uint8_t)(k * 40 + i);
    memset(pats, 0, sizeof(pats));
    memset(empty, 0, sizeof(empty));
    empty[0][0] = 16;                           /* an empty pattern need not be zeros */
    empty[1][1] = 16;
    for (k = 0; k < 2; k++)
        for (p = 0; p < PSL_PATS; p++)
            memcpy(pats[k][p], empty[k], DSZ);
    for (i = 0; i < NSZ; i++)                   /* NOTE ROLL 0 and 13, DRUM ROLL 2 */
        pats[0][0][i] = (uint8_t)(i * 3 + 1), pats[0][13][i] = (uint8_t)(200 - i);
    for (i = 0; i < DSZ; i++)
        pats[1][2][i] = (uint8_t)(i ^ 0x5a);
}

static int save(void)
{
    static const uint8_t sw[2] = { 33, 66 };

    flen = 0;
    writes = 0;
    return py_slot_save(&song, sw, &io, songbuf);
}

int main(void)
{
    static uint8_t keep[2][PSL_PATS][DSZ], good[sizeof(file)], ksnd[PSL_SOUNDS][PSL_SND];
    uint8_t sw[2] = { 0, 0 };
    psl_info info;
    uint32_t i, good_len;
    int r;

    /* round trip */
    make();
    CHECK(save() == PSL_OK);
    CHECK(flen == PSL_HDR + PS_FILE_BYTES + 2 * NSZ + DSZ + 2 * PSL_SND + 4);
    memcpy(keep, pats, sizeof(pats));
    memcpy(ksnd, snd, sizeof(snd));
    CHECK(py_slot_peek(&io, &info) == PSL_OK);
    CHECK(info.mask[0] == (1 | 1 << 13) && info.mask[1] == 1 << 2 && info.bytes == flen);
    CHECK(info.type[0] == PS_SYNTH && info.type[1] == PS_DRUM && info.type[2] == PS_AUDIO && info.type[3] == PS_NONE);
    CHECK(info.rows == 10 && info.chains == 2 && info.swing[0] == 33 && info.swing[1] == 66);
    CHECK(info.used[0] == 1 << 2 && info.used[1] == (1 << 7 | 1 << 8));    /* chain 3 on SYNTH, 5 on DRUM */
    CHECK(info.smask == (1 << 4 | 1 << 6) && info.hdr == PSL_HDR);
    {   /* the sounds: one edited differs, a refused one is reported; load: back, kept, refused */
        uint16_t diff[3], kp[3] = { 0, 0, 0 };

        CHECK(py_slot_diff(&io, &info, diff) == PSL_OK && !diff[2] && !info.sbad);
        snd[6][7] ^= 1;
        snd[4][0] ^= 1;
        snd[2][0] ^= 1;                         /* (not the song's: not compared, not put) */
        sok_no = 6;
        CHECK(py_slot_diff(&io, &info, diff) == PSL_OK && diff[2] == (1 << 4 | 1 << 6) && info.sbad == 1 << 6);
        sput_n = 0;
        CHECK(py_slot_load(&back, sw, &io, songbuf, kp) == PSL_OK && sput_n == 1);
        CHECK(!memcmp(snd[4], ksnd[4], PSL_SND) && memcmp(snd[6], ksnd[6], PSL_SND) && memcmp(snd[2], ksnd[2], PSL_SND));
        sok_no = -1;
        snd[4][0] ^= 1;
        kp[2] = 1 << 4;                         /* 4 kept: only 6 back */
        sput_n = 0;
        CHECK(py_slot_load(&back, sw, &io, songbuf, kp) == PSL_OK && sput_n == 1);
        CHECK(memcmp(snd[4], ksnd[4], PSL_SND) && !memcmp(snd[6], ksnd[6], PSL_SND));
        memcpy(snd, ksnd, sizeof(snd));
        memcpy(pats, keep, sizeof(pats));
    }
    {   /* nothing differs now; then NOTE ROLL 0 edited, 5 not empty any more, DRUM ROLL 2 emptied */
        uint16_t diff[3], kp[3];

        CHECK(py_slot_diff(&io, &info, diff) == PSL_OK && !diff[0] && !diff[1] && !diff[2]);
        pats[0][0][3] ^= 1;
        pats[0][5][0] = 99;
        memcpy(pats[1][2], empty[1], DSZ);
        CHECK(py_slot_diff(&io, &info, diff) == PSL_OK && diff[0] == (1 | 1 << 5) && diff[1] == 1 << 2);
        get_fail = -1;
        CHECK(py_slot_diff(&io, &info, diff) == PSL_BUSY);
        get_fail = 0;
        kp[0] = 1 << 5;                         /* NOTE ROLL 5 kept, the others as saved */
        kp[1] = kp[2] = 0;
        put_n = 0;
        CHECK(py_slot_load(&back, sw, &io, songbuf, kp) == PSL_OK && put_n == 2 * PSL_PATS - 1);
        CHECK(pats[0][5][0] == 99 && memcmp(pats[0][0], keep[0][0], NSZ) == 0 && memcmp(pats[1][2], keep[1][2], DSZ) == 0);
        kp[0] = 1 << 13;                        /* a stored one kept: the ones after it still right */
        kp[1] = 1 << 0;
        memset(pats, 0xee, sizeof(pats));
        CHECK(py_slot_load(&back, sw, &io, songbuf, kp) == PSL_OK && pats[0][13][0] == 0xee && pats[1][0][0] == 0xee);
        CHECK(memcmp(pats[0][0], keep[0][0], NSZ) == 0 && memcmp(pats[1][2], keep[1][2], DSZ) == 0);
        CHECK(memcmp(pats[1][1], empty[1], DSZ) == 0);
        memcpy(pats, keep, sizeof(pats));
    }
    CHECK(py_slot_check(&io, &info) == PSL_OK);
    memset(pats, 0xee, sizeof(pats));           /* the editors hold something else now */
    py_song_clear(&back);
    put_n = 0;
    CHECK(py_slot_load(&back, sw, &io, songbuf, 0) == PSL_OK);
    CHECK(memcmp(&back, &song, sizeof(song)) == 0);
    CHECK(sw[0] == 33 && sw[1] == 66);
    CHECK(put_n == 2 * PSL_PATS);
    r = 1;
    for (i = 0; i < PSL_PATS; i++)
        r &= memcmp(pats[0][i], keep[0][i], NSZ) == 0 && memcmp(pats[1][i], keep[1][i], DSZ) == 0;
    CHECK(r);                                   /* stored ones back, the rest empty again */
    CHECK(memcmp(pats[0][5], empty[0], NSZ) == 0);

    /* every damaged byte: refused, nothing changed */
    memcpy(good, file, flen);
    good_len = flen;
    for (i = 0; i < good_len; i++) {
        file[i] ^= 0x10;
        memset(pats, 0xee, sizeof(pats));
        py_song_clear(&back);
        put_n = 0;
        sput_n = 0;
        r = py_slot_load(&back, sw, &io, songbuf, 0);
        if (r == PSL_OK || put_n || sput_n || back.trk[0].type != PS_NONE) {
            CHECK(!"a damaged byte was taken");
            printf("  byte %u: r %d puts %d\n", i, r, put_n);
            break;
        }
        file[i] ^= 0x10;
    }
    /* short, long */
    flen = good_len - 1;
    CHECK(py_slot_check(&io, &info) == PSL_BAD);
    flen = good_len + 1;
    file[good_len] = 0;
    CHECK(py_slot_check(&io, &info) == PSL_BAD);
    flen = good_len;
    CHECK(py_slot_check(&io, &info) == PSL_OK);
    flen = 10;
    CHECK(py_slot_peek(&io, &info) == PSL_BAD);
    flen = 0;
    CHECK(py_slot_peek(&io, &info) == PSL_NOFILE);
    CHECK(py_slot_load(&back, sw, &io, songbuf, 0) == PSL_NOFILE);

    /* a v0.10.0 file: no "plays" field, read as the stored ones */
    memcpy(file, good, good_len);
    flen = good_len;
    memset(file + 28, 0, 4);
    i = file_crc(file, flen - 4);
    file[flen - 4] = (uint8_t)i, file[flen - 3] = (uint8_t)(i >> 8);
    file[flen - 2] = (uint8_t)(i >> 16), file[flen - 1] = (uint8_t)(i >> 24);
    CHECK(py_slot_check(&io, &info) == PSL_OK && info.used[0] == (1 | 1 << 13) && info.used[1] == 1 << 2);

    /* a track on CUR: TE's current sound then is stored */
    make();
    song.trk[0].sound = PS_EMPTY;
    io.cur_sound = 2;
    CHECK(save() == PSL_OK && py_slot_peek(&io, &info) == PSL_OK && info.smask == (1 << 2 | 1 << 6));
    io.cur_sound = 0;

    /* a PSL1 file (v0.10 / v0.11): 32-byte header, no sounds; read and loaded */
    make();
    song.trk[0].type = PS_DRUM;                 /* no SYNTH track: no sounds */
    CHECK(save() == PSL_OK && py_slot_peek(&io, &info) == PSL_OK && !info.smask);
    memmove(file + PSL_HDR1, file + PSL_HDR, flen - PSL_HDR);
    flen -= PSL_HDR - PSL_HDR1;
    file[3] = '1';
    file[8] = (uint8_t)flen, file[9] = (uint8_t)(flen >> 8), file[10] = (uint8_t)(flen >> 16), file[11] = 0;
    i = file_crc(file, flen - 4);
    file[flen - 4] = (uint8_t)i, file[flen - 3] = (uint8_t)(i >> 8);
    file[flen - 2] = (uint8_t)(i >> 16), file[flen - 1] = (uint8_t)(i >> 24);
    CHECK(py_slot_check(&io, &info) == PSL_OK && info.hdr == PSL_HDR1 && !info.smask);
    memset(pats, 0xee, sizeof(pats));
    sput_n = 0;
    CHECK(py_slot_load(&back, sw, &io, songbuf, 0) == PSL_OK && !memcmp(&back, &song, sizeof(song)) && !sput_n);
    CHECK(!memcmp(pats[0][13], keep[0][13], NSZ) && !memcmp(pats[1][2], keep[1][2], DSZ));
    {
        uint16_t diff[3];

        CHECK(py_slot_diff(&io, &info, diff) == PSL_OK && !diff[0] && !diff[1] && !diff[2]);
    }

    /* other pattern sizes (another version of an editor) */
    memcpy(file, good, good_len);
    flen = good_len;
    io.size[1] = DSZ - 4;
    CHECK(py_slot_peek(&io, &info) == PSL_OTHER);
    io.size[1] = DSZ;

    /* a pattern read inconsistently a few times: retried; always: BUSY, a failing write: IO */
    make();
    memcpy(pats, keep, sizeof(pats));
    get_fail = 3;
    CHECK(save() == PSL_OK && flen == good_len && memcmp(file, good, flen) == 0);
    get_fail = -1;
    CHECK(save() == PSL_BUSY);
    get_fail = 0;
    write_fail_at = 2;
    CHECK(save() == PSL_IO);
    write_fail_at = -1;

    /* nothing but empty patterns, no song */
    make();
    py_song_clear(&song);
    for (i = 0; i < NSZ; i++)
        pats[0][0][i] = pats[0][13][i] = empty[0][i];
    memcpy(pats[1][2], empty[1], DSZ);
    CHECK(save() == PSL_OK && flen == PSL_HDR + PS_FILE_BYTES + 4);   /* (no SYNTH track: no sounds) */
    CHECK(py_slot_peek(&io, &info) == PSL_OK && info.rows == 0 && info.chains == 0 && !info.mask[0] && !info.mask[1]);
    CHECK(py_slot_load(&back, sw, &io, songbuf, 0) == PSL_OK && back.trk[0].type == PS_NONE);

    /* where an imported file goes */
    CHECK(py_slot_pick("SONG05.PSL", 0, 0) == 4 && py_slot_pick("song05.psl", 0, 0) == 4);
    CHECK(py_slot_pick("SONG05.PSL", 1 << 4 | 1, 0) == 1);     /* taken: the first free one */
    CHECK(py_slot_pick("SONG17.PSL", 0, 0) == 0 && py_slot_pick("SONG00.PSL", 1, 0) == 1);
    CHECK(py_slot_pick("MYTUNE.PSL", 0x0007, 0) == 3 && py_slot_pick("SONG05.PSLX", 0, 0) == 0);
    CHECK(py_slot_pick("SONG05.PSL", 0xffff, 0) == -1 && py_slot_pick("SONG05.PSL", 0, 1 << 9) == -2);

    if (fails)
        printf("%d checks failed\n", fails);
    else
        printf("py_slot: all checks passed\n");
    return fails != 0;
}
