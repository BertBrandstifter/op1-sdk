/* Host tests for NOTE ROLL's punch-in effects (plain cc). */
#include <stdio.h>
#include "nr_punch.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define B(fx)   (1u << (fx))

int main(void)
{
    nr_px_seg s[3];
    nr_px_pulse o[16];
    int n, i, up = 0, dn = 0, five = 0, same = 0, ok;

    CHECK(nr_px_repeat(1000, 64, 5000, 2756, s) == 1 && s[0].src == 1000);
    n = nr_px_repeat(4980, 64, 5000, 2756, s);
    CHECK(n == 2 && s[0].n == 20 && s[1].src == 5000 && s[1].out == 20);
    n = nr_px_reverse(4980, 64, 5000, 1000, 4, s);
    CHECK(n == 2 && s[1].src == 8000 && s[1].n == 44);

    /* plain: the note itself */
    CHECK(nr_px_notes(0, 60, 100, 0, 1, 5512, 5512, 0, 256, o, 16) == 1 && o[0].key == 60 && o[0].vel == 100
          && o[0].delay == 0 && o[0].hold == 5512);
    CHECK(nr_px_notes(B(NR_PX_OCTUP), 60, 100, 0, 1, 5512, 5512, 0, 256, o, 16) == 1 && o[0].key == 72);
    CHECK(nr_px_notes(B(NR_PX_OCTUP) | B(NR_PX_OCTDN), 60, 100, 0, 1, 5512, 5512, 0, 256, o, 16) == 1 && o[0].key == 60);
    CHECK(nr_px_notes(B(NR_PX_OCTUP), 120, 100, 0, 1, 5512, 5512, 0, 256, o, 16) == 1 && o[0].key == 120);
    /* SOLOs */
    CHECK(nr_px_notes(B(NR_PX_SCHORDS), 60, 100, 0, 1, 5512, 5512, 0, 256, o, 16) == 0
          && nr_px_notes(B(NR_PX_SCHORDS), 60, 100, 0, 3, 5512, 5512, 0, 256, o, 16) == 1);
    CHECK(nr_px_notes(B(NR_PX_SLEAD), 67, 100, 2, 3, 5512, 5512, 0, 256, o, 16) == 1
          && nr_px_notes(B(NR_PX_SLEAD), 60, 100, 0, 3, 5512, 5512, 0, 256, o, 16) == 0);
    CHECK(nr_px_notes(B(NR_PX_SBASS) | B(NR_PX_SLEAD), 64, 100, 1, 3, 5512, 5512, 0, 256, o, 16) == 0);
    CHECK(nr_px_notes(B(NR_PX_SMELODY), 60, 100, 0, 1, 5512, 5512, 0, 256, o, 16) == 1);
    /* FADE */
    CHECK(nr_px_notes(B(NR_PX_FADE), 60, 100, 0, 1, 5512, 5512, 0, 128, o, 16) == 1 && o[0].vel == 50);
    CHECK(nr_px_notes(B(NR_PX_FADE), 60, 100, 0, 1, 5512, 5512, 0, 0, o, 16) == 0);
    /* RISING ARP, STACCATO, BLINDS */
    CHECK(nr_px_notes(B(NR_PX_ARP), 64, 100, 2, 3, 5511, 11022, 0, 256, o, 16) == 1 && o[0].delay == 3674 && o[0].hold == 1837);
    CHECK(nr_px_notes(B(NR_PX_STAC), 64, 100, 0, 1, 5512, 22048, 0, 256, o, 16) == 1 && o[0].hold == 1378);
    CHECK(nr_px_notes(B(NR_PX_BLINDS), 60, 100, 0, 1, 5512, 5512, 0, 256, o, 16) == 4 && o[1].delay == 1378 && o[1].hold == 689);
    /* CHIP ARP: a chord's notes in turn every 1/32 while the note lasts; one note: with its octave */
    n = nr_px_notes(B(NR_PX_CHIPUP), 64, 100, 1, 3, 5512, 4 * 5512, 0, 256, o, 16);
    for (i = 0, ok = n == 3; i < n; i++)
        ok &= o[i].key == 64 && o[i].delay == (uint32_t)(1 + 3 * i) * 2756;
    CHECK(ok);
    n = nr_px_notes(B(NR_PX_CHIPDN), 60, 100, 0, 3, 5512, 4 * 5512, 0, 256, o, 16);
    CHECK(n == 2 && o[0].delay == 2 * 2756 && o[1].delay == 5 * 2756);  /* down: the lowest note last */
    n = nr_px_notes(B(NR_PX_CHIPUP), 60, 100, 0, 1, 5512, 5512, 0, 256, o, 16);
    CHECK(n == 2 && o[0].key == 60 && o[1].key == 72 && o[1].delay == 2756);
    /* TRILL: the note and a whole tone up in turn */
    n = nr_px_notes(B(NR_PX_TRILL), 60, 100, 0, 1, 5512, 2 * 5512, 0, 256, o, 16);
    CHECK(n == 4 && o[1].key == 62 && o[2].key == 60 && o[3].delay == 3 * 2756);
    /* COIN, OCT ARP, ECHO after the note */
    n = nr_px_notes(B(NR_PX_COIN) | B(NR_PX_OCTARP) | B(NR_PX_ECHO), 60, 100, 0, 1, 5512, 5512, 0, 256, o, 16);
    CHECK(n == 5 && o[1].key == 72 && o[1].delay == 1378 && o[2].key == 72 && o[2].delay == 2756
          && o[3].key == 60 && o[3].delay == 3 * 5512 && o[3].vel == 60 && o[4].delay == 6 * 5512 && o[4].vel == 30);
    CHECK(nr_px_notes(B(NR_PX_ECHO) | B(NR_PX_COIN), 60, 100, 0, 1, 5512, 5512, 0, 256, o, 2) == 2);  /* max */
    /* RANDOM */
    for (i = 0; i < 100; i++) {
        nr_px_notes(B(NR_PX_RAND), 60, 100, 0, 1, 5512, 5512, (uint32_t)i, 256, o, 16);
        up += o[0].key == 72;
        dn += o[0].key == 48;
        five += o[0].key == 67;
        same += o[0].key == 60;
    }
    CHECK(up == 30 && dn == 20 && five == 20 && same == 30);

    printf(fails ? "nr_punch: %d FAILED\n" : "nr_punch: all passed (%d)\n", fails ? fails : checks);
    return fails != 0;
}
