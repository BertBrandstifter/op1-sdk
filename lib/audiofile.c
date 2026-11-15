/* op1-sdk library "audiofile": WAV / AIFF header parser, see include/op1/audiofile.h. */
#include <op1/audiofile.h>

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }
static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return be16(p) << 16 | be16(p + 2); }

static int id_is(const uint8_t *p, const char *id)
{
    return p[0] == (uint8_t)id[0] && p[1] == (uint8_t)id[1] && p[2] == (uint8_t)id[2]
        && p[3] == (uint8_t)id[3];
}

#define AUD_SCAN 65536u                          /* chunks must start in the first 64 KB */

static uint32_t ext80(const uint8_t *b)         /* integer part of an 80-bit IEEE extended */
{
    int e = (int)(((b[0] & 0x7f) << 8) | b[1]), shift = 16383 + 31 - e;

    return shift < 0 || shift > 31 ? 0 : be32(b + 2) >> shift;
}

struct src { aud_read_fn read; void *ctx; };

static int rdf(const struct src *s, uint32_t off, void *buf, uint32_t n)
{
    return s->read(s->ctx, off, buf, n) == (int)n ? 0 : -1;
}

static int finish(aud_info *a, int have_fmt, int have_data, uint32_t codec_ok)
{
    uint32_t frame = (uint32_t)a->channels * 2;

    if (!have_fmt)
        return a->err = AUD_ENOFMT;
    if (!have_data)
        return a->err = AUD_ENODATA;
    if (!codec_ok)
        return a->err = AUD_ECODEC;
    if (a->channels != 1 && a->channels != 2)
        return a->err = AUD_ECHANNELS;
    if (a->bits != 16)
        return a->err = AUD_EBITS;
    if (a->rate != 44100)
        return a->err = AUD_ERATE;
    if (a->kind == AUD_WAV)
        a->frames = a->data_bytes / frame;
    if (!a->frames || a->data_bytes / frame < a->frames)
        return a->err = AUD_ESHORT;
    return a->err = AUD_OK;
}

int aud_parse(aud_read_fn read, void *ctx, uint32_t size, aud_info *a)
{
    const struct src s = { read, ctx }, *v = &s;
    uint8_t h[40];
    uint32_t pos = 12, codec_ok = 0, n;
    int have_fmt = 0, have_data = 0, wav, chunks;

    a->kind = AUD_NONE;
    a->channels = a->bits = a->big_endian = 0;
    a->rate = a->frames = a->data_off = a->data_bytes = 0;
    if (size < 12 || rdf(v, 0, h, 12))
        return a->err = size < 12 ? AUD_EFORMAT : AUD_EREAD;
    if (id_is(h, "RIFF") && id_is(h + 8, "WAVE")) {
        wav = 1;
        a->kind = AUD_WAV;
    } else if (id_is(h, "FORM") && (id_is(h + 8, "AIFF") || id_is(h + 8, "AIFC"))) {
        wav = 0;
        a->kind = AUD_AIFF;
        a->big_endian = 1;
        codec_ok = id_is(h + 8, "AIFF");        /* AIFC: from COMM's compression type */
    } else {
        return a->err = AUD_EFORMAT;
    }
    for (chunks = 0; chunks < 64 && pos + 8 <= size && pos < AUD_SCAN; chunks++) {
        uint32_t len, body = pos + 8;

        if (rdf(v, pos, h, 8))
            return a->err = AUD_EREAD;
        len = wav ? le32(h + 4) : be32(h + 4);
        if (wav && id_is(h, "fmt ")) {
            n = len < 40 ? len : 40;
            if (n < 16 || n > size - body)              /* too small or cut off by the end */
                return a->err = AUD_ENOFMT;
            if (rdf(v, body, h, n))
                return a->err = AUD_EREAD;
            codec_ok = le16(h) == 1 || (le16(h) == 0xfffe && n >= 26 && le16(h + 24) == 1);
            a->channels = (uint8_t)(le16(h + 2) > 255 ? 255 : le16(h + 2));
            a->rate = le32(h + 4);
            a->bits = (uint8_t)(le16(h + 14) > 255 ? 255 : le16(h + 14));
            have_fmt = 1;
        } else if (wav && id_is(h, "data")) {
            a->data_off = body;
            a->data_bytes = len;
            have_data = 1;
        } else if (!wav && id_is(h, "COMM")) {
            n = len < 22 ? len : 22;
            if (n < 18 || n > size - body)
                return a->err = AUD_ENOFMT;
            if (rdf(v, body, h, n))
                return a->err = AUD_EREAD;
            a->channels = (uint8_t)(be16(h) > 255 ? 255 : be16(h));
            a->frames = be32(h + 2);
            a->bits = (uint8_t)(be16(h + 6) > 255 ? 255 : be16(h + 6));
            a->rate = ext80(h + 8);
            if (!codec_ok) {                    /* AIFC */
                if (n >= 22 && (id_is(h + 18, "NONE") || id_is(h + 18, "twos")))
                    codec_ok = 1;               /* big-endian linear PCM (afconvert writes twos) */
                else if (n >= 22 && id_is(h + 18, "sowt"))
                    codec_ok = 1, a->big_endian = 0;
            }
            have_fmt = 1;
        } else if (!wav && id_is(h, "SSND")) {
            if (len < 8 || size - body < 8)
                return a->err = AUD_ENODATA;
            if (rdf(v, body, h, 8))
                return a->err = AUD_EREAD;
            if (be32(h) > len - 8)
                return a->err = AUD_ESHORT;
            a->data_off = body + 8 + be32(h);
            a->data_bytes = len - 8 - be32(h);
            have_data = 1;
        }
        if (have_fmt && have_data)
            break;
        if (len > size - body)                  /* the chunk runs past the end of the file */
            break;
        pos = body + len + (len & 1);
    }
    if (have_data && (a->data_off > size || a->data_bytes > size - a->data_off))
        return a->err = AUD_ESHORT;              /* truncated file */
    return finish(a, have_fmt, have_data, codec_ok);
}

const char *aud_err_text(int err)
{
    static const char *const t[] = {
        "OK", "READ ERROR", "NOT WAV/AIF", "NO FORMAT", "NO DATA", "COMPRESSED",
        "NOT 16 BIT", "NOT 44.1 KHZ", "NOT MONO/STEREO", "SHORT FILE",
    };

    return err >= 0 && err <= AUD_ESHORT ? t[err] : "?";
}
