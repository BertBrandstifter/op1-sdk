/* host tests for dr_store.c: cc -std=c99 test_dr_store.c dr_store.c && ./a.out
 * (dr_core.c is included below: the store clamps step parameters with dr_param_clamp) */
#include <stdio.h>
#include <string.h>
#include "dr_store.h"
#include "dr_core.c"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static char text[DR_ROW_TEXT_MAX];

static void b64_tests(void)
{
    static const char *const in[] = { "", "f", "fo", "foo", "foob", "fooba", "foobar" };
    static const char *const want[] = { "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy" };
    uint8_t out[16];
    int i;

    for (i = 0; i < 7; i++) {
        int n = (int)strlen(in[i]);

        CHECK(dr_b64_enc((const uint8_t *)in[i], n, text) == (int)strlen(want[i]));
        CHECK(!strcmp(text, want[i]));
        CHECK(dr_b64_dec(want[i], out, sizeof(out)) == n && !memcmp(out, in[i], (size_t)n));
    }
    CHECK(dr_b64_dec("Zm9", out, 16) == -1);            /* not a multiple of 4 */
    CHECK(dr_b64_dec("Zm'v", out, 16) == -1);           /* a quote never decodes */
    CHECK(dr_b64_dec("Zg==Zg==", out, 16) == -1);       /* padding in the middle */
    CHECK(dr_b64_dec("Z===", out, 16) == -1);
    CHECK(dr_b64_dec("Zm9vYmFy", out, 5) == -1);        /* too long for out */
}

static void pattern_tests(void)
{
    static dr_pattern p, q, r;
    int n, i, bad;

    dr_pattern_init(&p);
    p.lane[2].key = 17;
    p.lane[2].len = 12;
    p.lane[2].rate = 5;
    p.lane[2].level = 40;
    p.lane[2].mute = 1;
    p.lane[7].s[63].on = 1;
    p.lane[7].s[63].p[DR_TONE] = -24;
    p.lane[7].s[63].p[DR_NUDGE] = 50;
    p.lane[0].s[0].on = 1;
    p.lane[0].s[0].p[DR_RATCH] = 4;

    n = dr_store_pattern(2, &p, text);
    CHECK(n == (int)DR_B64_LEN(DR_HDR + sizeof(dr_pattern)) && n == (int)strlen(text));
    CHECK(n + 80 < 8000);                               /* the INSERT fits TE's 8 KB SQL buffer */
    for (i = bad = 0; i < n; i++)
        bad += text[i] == '\'' || text[i] == '%' || text[i] == '"' || text[i] == '\\';
    CHECK(!bad);

    memset(&q, 0x55, sizeof(q));
    CHECK(dr_load_pattern(2, text, &q) == 0 && !memcmp(&p, &q, sizeof(p)));
    memset(&r, 0x55, sizeof(r));
    CHECK(dr_load_pattern(1, text, &r) == -1);          /* another row's data */
    CHECK(dr_load_pattern(4, text, &r) == -1);
    CHECK(dr_load_pattern(2, "", &r) == -1);            /* empty row */
    CHECK(dr_load_pattern(2, 0, &r) == -1);
    CHECK(dr_load_pattern(2, "garbage!", &r) == -1);
    text[10] = '\'';
    CHECK(dr_load_pattern(2, text, &r) == -1);
    for (i = 0; i < (int)sizeof(r); i++)
        bad += ((uint8_t *)&r)[i] != 0x55;
    CHECK(!bad);                                        /* failed loads leave out untouched */

    /* out-of-range fields come back in range */
    {
        uint8_t raw[DR_HDR + sizeof(dr_pattern)];

        raw[0] = 'D'; raw[1] = 'R'; raw[2] = DR_STORE_VERSION; raw[3] = 0;
        memset(raw + DR_HDR, 0xff, sizeof(dr_pattern));
        dr_b64_enc(raw, (int)sizeof(raw), text);
        CHECK(dr_load_pattern(0, text, &q) == 0);
        CHECK(q.lane[0].key < DR_KEYS && q.lane[0].len == DR_STEPS && q.lane[0].rate == DR_RATE_16
              && q.lane[0].level == 100 && q.lane[0].mute == 1 && q.lane[0].s[5].on == 1);
        CHECK(q.lane[3].s[9].p[DR_VEL] == 1 && q.lane[3].s[9].p[DR_RATCH] == 1);    /* -1 -> min */
        raw[2] = DR_STORE_VERSION + 1;
        dr_b64_enc(raw, (int)sizeof(raw), text);
        CHECK(dr_load_pattern(0, text, &q) == -1);      /* unknown version: defaults stay */
    }
}

static void settings_tests(void)
{
    dr_settings s, t;
    int i;

    memset(&s, 0, sizeof(s));
    s.cur = 3;
    s.swing = 55;
    s.zoom = 2;
    for (i = 0; i < DR_KEYS; i++) {
        s.filt[i].type = (uint8_t)(i % 4);
        s.filt[i].cut = (uint8_t)(i * 5);
        s.filt[i].res = (uint8_t)(127 - i);
    }
    dr_store_settings(&s, text);
    CHECK(dr_load_settings(text, 4, &t) == 0 && !memcmp(&s, &t, sizeof(s)));
    CHECK(dr_load_pattern(DR_ROW_SETTINGS, text, (dr_pattern *)0) == -1);
    s.cur = 9;
    s.swing = 200;
    s.zoom = 7;
    s.filt[3].type = 9;
    s.filt[4].cut = 200;
    dr_store_settings(&s, text);
    CHECK(dr_load_settings(text, 4, &t) == 0);
    CHECK(t.cur == 1 && t.swing == 100 && t.zoom == 2 && t.filt[3].type == 0 && t.filt[4].cut == 127);
}

int main(void)
{
    b64_tests();
    pattern_tests();
    settings_tests();
    CHECK(dr_hash("abc", 3) != dr_hash("abd", 3));
    printf(fails ? "%d FAILED\n" : "dr_store: all passed\n", fails);
    return fails != 0;
}
