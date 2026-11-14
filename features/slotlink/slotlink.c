/* slotlink: the default slot files that are a copy of a factory sound are left out of the update
 * (content_links in feature.json, checked byte for byte by tools/build.py); this puts them back
 * on the OP-1 when TE would have copied them (spec: README.md).
 *
 * TE fills user/audio/{synth,drum}/ from content/audio/... only when user/op1.db was missing
 * (first boot, after a factory reset): db2.cpp's init (0x0196f6a0) calls the directory copy
 * 0x0196eb7e(dir, root) for audio/synth, audio/preset_synth, audio/drum, audio/preset_drum. Our
 * hook runs after the synth and the drum copy and creates each linked slot file that is still
 * missing from its factory file. A slot file that exists (TE copied it: the build kept it) is
 * never touched. Main thread, before the UI. */
#include <op1/te246.h>

typedef struct {
    const char *dir;                            /* TE's directory argument */
    const char *slot;                           /* user/<slot> */
    const char *from;                           /* content/<from> */
} sl_link;

/* the same pairs as "content_links" in feature.json */
static const sl_link links[] = {
    { "audio/drum", "audio/drum/drum_0.raw", "audio/factory_drum/peek.raw" },
    { "audio/drum", "audio/drum/drum_2.raw", "audio/factory_drum/awa beef.raw" },
    { "audio/drum", "audio/drum/drum_4.raw", "audio/factory_drum/neoprene.raw" },
    { "audio/drum", "audio/drum/drum_5.raw", "audio/factory_drum/acetate.raw" },
    { "audio/drum", "audio/drum/drum_6.raw", "audio/factory_drum/red maple.raw" },
    { "audio/synth", "audio/synth/synth_7.raw", "audio/factory_synth/suitcase.raw" },
};
#define LINKS ((int)(sizeof(links) / sizeof(links[0])))

static char buf[2048];
static char src[160], dst[160];
volatile int sl_made, sl_failed;                /* bench reads */

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* out = a + b + c, 0 if it does not fit */
static int join(char *out, int cap, const char *a, const char *b, const char *c)
{
    const char *parts[3] = { a, b, c };
    int i, n = 0;

    for (i = 0; i < 3; i++)
        for (const char *p = parts[i]; *p; p++) {
            if (n >= cap - 1)
                return 0;
            out[n++] = *p;
        }
    out[n] = 0;
    return 1;
}

/* copy content/<from> to user/<slot> unless user/<slot> exists; 1 = made */
static int make(const char *root, const sl_link *l)
{
    int in, out, n, ok = 1;

    if (!join(dst, sizeof(dst), root, "user/", l->slot) || !join(src, sizeof(src), root, "content/", l->from))
        return -1;
    out = TE_FILE_OPEN(dst, TE_FILE_RDONLY, TE_FILE_MODE);
    if (out != -1) {                            /* there: TE copied it */
        TE_FILE_CLOSE(out);
        return 0;
    }
    in = TE_FILE_OPEN(src, TE_FILE_RDONLY, TE_FILE_MODE);
    if (in == -1)
        return -1;
    out = TE_FILE_OPEN(dst, TE_FILE_CREATE, TE_FILE_CREATE_MODE);
    if (out == -1) {
        TE_FILE_CLOSE(in);
        return -1;
    }
    while ((n = TE_FILE_READ(in, buf, sizeof(buf))) > 0)
        if (TE_FILE_WRITE(out, buf, (uint32_t)n) != n) {
            ok = 0;
            break;
        }
    if (n < 0)
        ok = 0;
    TE_FILE_CLOSE(in);
    TE_FILE_CLOSE(out);
    return ok ? 1 : -1;
}

/* after TE's directory copy 0x0196eb7e(dir, root) */
void sl_after_copy(const char *dir, const char *root)
{
    int i, r;

    for (i = 0; i < LINKS; i++) {
        if (!same(dir, links[i].dir))
            continue;
        r = make(root, &links[i]);
        if (r > 0)
            sl_made++;
        else if (r < 0)
            sl_failed++;
    }
}
