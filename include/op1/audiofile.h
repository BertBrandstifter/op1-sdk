/* op1-sdk library "audiofile" (libs: ["audiofile"]): WAV / AIFF / AIFC header parser.
 *
 * Reads the header of an audio file through a caller-supplied "read n bytes at offset" function
 * (a yaffs pread, a file inside the disk-mode FAT image via op1/fat.h, a memory buffer in host
 * tests) and checks it against what the SDK plays: PCM 16-bit, 44.1 kHz, mono or stereo.
 * Accepted containers: RIFF/WAVE (PCM, WAVE_FORMAT_EXTENSIBLE with PCM), AIFF, AIFC with
 * NONE / twos (big-endian) or sowt (little-endian). Chunks must start in the first 64 KB.
 * No allocation, bounded loops. Thread context (file reads), never the audio thread. */
#ifndef OP1_AUDIOFILE_H
#define OP1_AUDIOFILE_H

#include <stdint.h>

/* read n bytes at byte offset off into buf; returns n on success, anything else = error */
typedef int (*aud_read_fn)(void *ctx, uint32_t off, void *buf, uint32_t n);

enum { AUD_NONE, AUD_WAV, AUD_AIFF };
enum {                                  /* aud_info.err: why a file is not accepted (0 = accepted) */
    AUD_OK, AUD_EREAD, AUD_EFORMAT,     /* not RIFF/WAVE, not FORM/AIFF/AIFC */
    AUD_ENOFMT, AUD_ENODATA,            /* no fmt / COMM, no data / SSND chunk */
    AUD_ECODEC,                         /* compressed (not PCM / NONE / twos / sowt) */
    AUD_EBITS, AUD_ERATE, AUD_ECHANNELS,/* not 16-bit, not 44.1 kHz, not 1 or 2 channels */
    AUD_ESHORT,                         /* data chunk shorter than the header says / empty */
};

typedef struct aud_info {
    uint8_t kind;                       /* AUD_WAV / AUD_AIFF */
    uint8_t err;                        /* AUD_* */
    uint8_t channels, bits;
    uint8_t big_endian;                 /* sample byte order (AIFF NONE / twos = 1, WAV / sowt = 0) */
    uint32_t rate;                      /* Hz (AIFF: integer part of the 80-bit float) */
    uint32_t frames;
    uint32_t data_off, data_bytes;      /* PCM data inside the file */
} aud_info;

/* parse the header of a file of `size` bytes; fills *a, returns a->err */
int aud_parse(aud_read_fn read, void *ctx, uint32_t size, aud_info *a);

const char *aud_err_text(int err);      /* "OK", "NOT 44.1 KHZ", ... (upper case, for the 3x5 font) */

#endif
