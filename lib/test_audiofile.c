/* host test for lib/audiofile.c (tools/test_all.py builds it with its sibling audiofile.c).
 *
 * Every header case the parser must accept or reject, parsed from memory buffers: WAV PCM (with a
 * LIST chunk and its padding byte), WAVE_FORMAT_EXTENSIBLE, float WAV, 24-bit, AIFF mono / 48 kHz,
 * AIFC NONE / twos / sowt (with an SSND offset) / ima4, a data chunk longer than the file, a
 * truncated header, a chunk size running past the end, a non-audio file, a read error. */
#include <stdio.h>
#include <string.h>
#include <op1/audiofile.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { w16(p, v); w16(p + 2, v >> 16); }
static void b16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void b32(uint8_t *p, uint32_t v) { b16(p, v >> 16); b16(p + 2, v); }


static uint8_t file_buf[1 << 21];

static uint8_t pat(uint32_t i) { return (uint8_t)(i * 7 + (i >> 9)); }

/* WAV: fmt (tag, ch, rate, bits), an optional LIST chunk before it, data of n bytes;
 * declared != 0 overrides the data chunk's size field (a truncated file) */
static uint32_t make_wav(uint8_t *b, int tag, int ch, uint32_t rate, int bits, uint32_t n,
                         int list, uint32_t declared)
{
    uint32_t p = 12, i, flen = tag == 0xfffe ? 40 : 16;

    memcpy(b, "RIFF", 4);
    memcpy(b + 8, "WAVE", 4);
    if (list) {                             /* an odd-sized chunk: padding byte */
        memcpy(b + p, "LIST", 4); w32(b + p + 4, 13);
        memset(b + p + 8, 'i', 13);
        p += 8 + 14;
    }
    memcpy(b + p, "fmt ", 4); w32(b + p + 4, flen);
    w16(b + p + 8, (uint32_t)tag); w16(b + p + 10, (uint32_t)ch); w32(b + p + 12, rate);
    w32(b + p + 16, rate * (uint32_t)ch * (uint32_t)bits / 8); w16(b + p + 20, (uint32_t)(ch * bits / 8));
    w16(b + p + 22, (uint32_t)bits);
    if (tag == 0xfffe) {
        w16(b + p + 24, 22); w16(b + p + 26, (uint32_t)bits); w32(b + p + 28, 3);
        w16(b + p + 32, 1);                 /* KSDATAFORMAT_SUBTYPE_PCM */
        memcpy(b + p + 34, "\x00\x00\x00\x00\x10\x00", 6);
    }
    p += 8 + flen;
    memcpy(b + p, "data", 4); w32(b + p + 4, declared ? declared : n);
    for (i = 0; i < n; i++)
        b[p + 8 + i] = pat(i);
    p += 8 + n;
    w32(b + 4, p - 8);
    return p;
}

static void ext80(uint8_t *b, uint32_t rate)  /* integer -> 80-bit extended */
{
    int e = 31;

    while (!(rate & 0x80000000u)) {
        rate <<= 1;
        e--;
    }
    b16(b, 16383 + (uint32_t)e);
    b32(b + 2, rate);
    memset(b + 6, 0, 4);
}

/* AIFF / AIFC: COMM (ch, frames, bits, rate[, compression]), SSND with offset ofs */
static uint32_t make_aif(uint8_t *b, const char *comp, int ch, uint32_t rate, int bits,
                         uint32_t frames, uint32_t ofs)
{
    uint32_t p = 12, n = frames * (uint32_t)ch * 2, i, clen = comp ? 24 : 18;

    memcpy(b, "FORM", 4);
    memcpy(b + 8, comp ? "AIFC" : "AIFF", 4);
    if (comp) {
        memcpy(b + p, "FVER", 4); b32(b + p + 4, 4); b32(b + p + 8, 0xa2805140u);
        p += 12;
    }
    memcpy(b + p, "COMM", 4); b32(b + p + 4, clen);
    b16(b + p + 8, (uint32_t)ch); b32(b + p + 10, frames); b16(b + p + 14, (uint32_t)bits);
    ext80(b + p + 16, rate);
    if (comp) {
        memcpy(b + p + 26, comp, 4);
        b[p + 30] = 0; b[p + 31] = 0;       /* empty pstring + pad */
    }
    p += 8 + clen;
    memcpy(b + p, "APPL", 4); b32(b + p + 4, 5); memcpy(b + p + 8, "op-1\0", 5);
    p += 8 + 6;
    memcpy(b + p, "SSND", 4); b32(b + p + 4, 8 + ofs + n); b32(b + p + 8, ofs); b32(b + p + 12, 0);
    memset(b + p + 16, 0, ofs);
    for (i = 0; i < n; i++)
        b[p + 16 + ofs + i] = pat(i);
    p += 16 + ofs + n;
    b32(b + 4, p - 8);
    return p;
}


typedef struct buf { const uint8_t *b; uint32_t n; int fail_at; } buf;

static int mem_read(void *ctx, uint32_t off, void *out, uint32_t n)
{
    buf *m = ctx;

    if (m->fail_at >= 0 && off + n > (uint32_t)m->fail_at)
        return -1;
    if (off > m->n || n > m->n - off)
        return -1;
    memcpy(out, m->b + off, n);
    return (int)n;
}

static void expect(const char *what, const uint8_t *b, uint32_t n, int kind, int err, int ch,
                   uint32_t rate, uint32_t frames, int be)
{
    buf m = { b, n, -1 };
    aud_info a;

    aud_parse(mem_read, &m, n, &a);
    if (a.kind != kind || a.err != err || (err == AUD_OK && (a.channels != ch || a.rate != rate
        || a.frames != frames || a.big_endian != be))) {
        fails++;
        printf("FAIL %s: kind %d err %d (%s) ch %d rate %u frames %u be %d\n", what, a.kind, a.err,
               aud_err_text(a.err), a.channels, a.rate, a.frames, a.big_endian);
    }
}

int main(void)
{
    uint32_t n;
    aud_info a;
    buf m;

    n = make_wav(file_buf, 1, 2, 44100, 16, 4000, 1, 0);
    expect("wav stereo + LIST", file_buf, n, AUD_WAV, AUD_OK, 2, 44100, 1000, 0);
    {
        buf mm = { file_buf, n, -1 };
        aud_parse(mem_read, &mm, n, &a);
        CHECK(a.data_bytes == 4000 && !memcmp(file_buf + a.data_off, "\x00\x07\x0e", 3));
    }
    n = make_wav(file_buf, 1, 1, 44100, 16, 3000, 0, 0);
    expect("wav mono", file_buf, n, AUD_WAV, AUD_OK, 1, 44100, 1500, 0);
    n = make_wav(file_buf, 0xfffe, 2, 44100, 16, 8000, 0, 0);
    expect("wav extensible", file_buf, n, AUD_WAV, AUD_OK, 2, 44100, 2000, 0);
    n = make_wav(file_buf, 3, 2, 44100, 32, 800, 0, 0);
    expect("wav float", file_buf, n, AUD_WAV, AUD_ECODEC, 0, 0, 0, 0);
    n = make_wav(file_buf, 1, 2, 44100, 24, 6000, 0, 0);
    expect("wav 24 bit", file_buf, n, AUD_WAV, AUD_EBITS, 0, 0, 0, 0);
    n = make_wav(file_buf, 1, 2, 48000, 16, 6000, 0, 0);
    expect("wav 48k", file_buf, n, AUD_WAV, AUD_ERATE, 0, 0, 0, 0);
    n = make_wav(file_buf, 1, 6, 44100, 16, 6000, 0, 0);
    expect("wav 6 channels", file_buf, n, AUD_WAV, AUD_ECHANNELS, 0, 0, 0, 0);
    n = make_wav(file_buf, 1, 2, 44100, 16, 4000, 0, 900000);
    expect("wav data past the end", file_buf, n, AUD_WAV, AUD_ESHORT, 0, 0, 0, 0);
    n = make_wav(file_buf, 1, 2, 44100, 16, 0, 0, 0);
    expect("wav empty data", file_buf, n, AUD_WAV, AUD_ESHORT, 0, 0, 0, 0);
    n = make_wav(file_buf, 1, 2, 44100, 16, 4000, 0, 0);
    expect("wav cut in the header", file_buf, 30, AUD_WAV, AUD_ENOFMT, 0, 0, 0, 0);
    expect("wav no data chunk", file_buf, 44 - 8, AUD_WAV, AUD_ENODATA, 0, 0, 0, 0);

    n = make_aif(file_buf, 0, 1, 44100, 16, 20000, 0);
    expect("aiff mono", file_buf, n, AUD_AIFF, AUD_OK, 1, 44100, 20000, 1);
    n = make_aif(file_buf, 0, 2, 48000, 16, 1000, 0);
    expect("aiff 48k", file_buf, n, AUD_AIFF, AUD_ERATE, 0, 0, 0, 0);
    n = make_aif(file_buf, 0, 2, 22050, 16, 1000, 0);
    expect("aiff 22.05k", file_buf, n, AUD_AIFF, AUD_ERATE, 0, 0, 0, 0);
    n = make_aif(file_buf, "NONE", 2, 44100, 16, 3000, 0);
    expect("aifc NONE", file_buf, n, AUD_AIFF, AUD_OK, 2, 44100, 3000, 1);
    n = make_aif(file_buf, "twos", 2, 44100, 16, 3000, 0);
    expect("aifc twos", file_buf, n, AUD_AIFF, AUD_OK, 2, 44100, 3000, 1);
    n = make_aif(file_buf, "sowt", 2, 44100, 16, 5000, 4);
    expect("aifc sowt + SSND offset", file_buf, n, AUD_AIFF, AUD_OK, 2, 44100, 5000, 0);
    {
        buf mm = { file_buf, n, -1 };
        aud_parse(mem_read, &mm, n, &a);
        CHECK(a.data_bytes == 20000 && file_buf[a.data_off] == pat(0) && file_buf[a.data_off + 1] == pat(1));
    }
    n = make_aif(file_buf, "ima4", 2, 44100, 16, 100, 0);
    expect("aifc ima4", file_buf, n, AUD_AIFF, AUD_ECODEC, 0, 0, 0, 0);
    n = make_aif(file_buf, 0, 2, 44100, 16, 1000, 0);
    expect("aiff cut short", file_buf, n - 100, AUD_AIFF, AUD_ESHORT, 0, 0, 0, 0);

    expect("mp3", (const uint8_t *)"ID3\x04 not audio we read", 23, AUD_NONE, AUD_EFORMAT, 0, 0, 0, 0);
    expect("too short", (const uint8_t *)"RIFF", 4, AUD_NONE, AUD_EFORMAT, 0, 0, 0, 0);

    /* a chunk whose size runs past the end: no hang, no out-of-range read */
    n = make_wav(file_buf, 1, 2, 44100, 16, 4000, 1, 0);
    w32(file_buf + 16, 0xfffffff0u);
    expect("LIST size huge", file_buf, n, AUD_WAV, AUD_ENOFMT, 0, 0, 0, 0);

    /* a read error inside the header */
    n = make_wav(file_buf, 1, 2, 44100, 16, 4000, 0, 0);
    m.b = file_buf; m.n = n; m.fail_at = 20;
    CHECK(aud_parse(mem_read, &m, n, &a) == AUD_EREAD);

    CHECK(!strcmp(aud_err_text(AUD_OK), "OK") && !strcmp(aud_err_text(AUD_ERATE), "NOT 44.1 KHZ")
          && !strcmp(aud_err_text(99), "?"));
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
