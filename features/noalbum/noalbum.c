/* noalbum (spec: README.md): the album gives its 121 MB in the disk image back.
 *
 * The 8 patches (feature.json) make the album sides 1 s long and the disk image (user/tape_c.raw,
 * the FAT volume of disk mode) 256 MiB instead of 384; TE rebuilds the image once (size mismatch).
 * The album recorder sits in TE's encrypted L1 code, so its own length limit cannot be changed:
 * on the album screen REC and PLAY are taken here, so nothing records past (or plays beyond) the
 * 1 s files. */
#include <op1/te246.h>
#include <op1/gfx.h>

static volatile uint8_t held;                   /* REC / PLAY taken while down (bit 0 / 1) */

/* on_key filter (KeyHandler thread) */
int na_key(int key, int down)
{
    int bit = key == TE_KEY_REC ? 1 : key == TE_KEY_PLAY ? 2 : 0;

    if (!bit)
        return 0;
    if (down) {
        if (*TE_EDIT_MODE != TE_EDIT_MODE_ALBUM)
            return 0;
        held |= (uint8_t)bit;
        return 1;
    }
    if (held & bit) {                           /* the matching key up */
        held &= (uint8_t)~bit;
        return 1;
    }
    return 0;
}

/* frame (GUI thread): a note on the album screen */
void na_frame(uint16_t *buf)
{
    if (*TE_EDIT_MODE != TE_EDIT_MODE_ALBUM)
        return;
    gfx_begin(buf);
    gfx_rect(40, 60, 240, 36, GFX_BLACK);
    gfx_text(52, 66, "ALBUM IS OFF", GFX_WHITE, 2);
    gfx_text(52, 82, "ITS SPACE IS FREE FOR SAMPLES", GFX_RGB(0x5a, 0x58, 0x66), 1);
}
