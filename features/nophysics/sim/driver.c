/* nophysics bench driver: the RTS the redirected pointers lead to, reached the ways TE reaches it.
 *   - like TE's boot loop over the ctor table: indirect CALL through a table of pointers,
 *   - like a vtable call: this pointer in R0, an argument in R1,
 *   - with callee-saved registers and the stack holding sentinels that must survive.
 * Exit 0 (HLT) = pass, else ABORT with R7 = failed check bits. */
#include <stdint.h>

typedef void (*fn0)(void);
typedef void (*fn2)(void *self, int arg);

extern char stub_addr[];         /* --defsym: 0x0191fe8c, first RTS of the blanked Box2D range */
volatile int fails;
volatile int calls;

#define CHECK(bit, c) do { if (!(c)) fails |= 1 << (bit); } while (0)

static fn0 table[30];

/* one asm block: sentinels in the callee-saved R4-R7 / P3-P4, CALL (P2), compare; 0 = all kept
 * (P5 is GCC's PIC register here) */
static uint32_t keeps_regs(void *fn)
{
    uint32_t bad;

    __asm__ volatile(
        "P2 = %1;\n\t"
        "R4 = 0x1234 (Z); R4.H = 0x4444;\n\t"
        "R5 = 0x2345 (Z); R5.H = 0x5555;\n\t"
        "R6 = 0x3456 (Z); R6.H = 0x6666;\n\t"
        "R7 = 0x4567 (Z); R7.H = 0x7777;\n\t"
        "P3 = R4; P4 = R5;\n\t"
        "SP += -12;\n\t"
        "CALL (P2);\n\t"
        "SP += 12;\n\t"
        "%0 = 1 (X);\n\t"
        "R0 = 0x1234 (Z); R0.H = 0x4444; CC = R4 == R0; IF !CC JUMP 1f;\n\t"
        "R0 = P3; CC = R4 == R0; IF !CC JUMP 1f;\n\t"
        "R0 = 0x2345 (Z); R0.H = 0x5555; CC = R5 == R0; IF !CC JUMP 1f;\n\t"
        "R0 = P4; CC = R5 == R0; IF !CC JUMP 1f;\n\t"
        "R0 = 0x3456 (Z); R0.H = 0x6666; CC = R6 == R0; IF !CC JUMP 1f;\n\t"
                "R0 = 0x4567 (Z); R0.H = 0x7777; CC = R7 == R0; IF !CC JUMP 1f;\n\t"
        "%0 = 0 (X);\n"
        "1:"
        : "=&d"(bad) : "a"(fn)
        : "R0", "R4", "R5", "R6", "R7", "P2", "P3", "P4", "CC", "RETS", "memory");
    return bad;
}

int main(void)
{
    uint32_t sp0, sp1;
    int i;
    volatile int guard = 0x5a5aa5a5;
    fn2 slot = (fn2)(void *)stub_addr;

    for (i = 0; i < 30; i++)
        table[i] = (fn0)(void *)stub_addr;
    __asm__ volatile("%0 = SP;" : "=d"(sp0));
    for (i = 0; i < 30; i++) {          /* the boot loop over the ctor table */
        table[i]();
        calls++;
    }
    slot((void *)0x01dd566c, 0x24);     /* vtable call: this in R0, an argument in R1 */
    calls++;
    __asm__ volatile("%0 = SP;" : "=d"(sp1));
    CHECK(0, calls == 31);
    CHECK(1, sp0 == sp1);
    CHECK(2, guard == 0x5a5aa5a5);
    CHECK(3, *(volatile uint16_t *)stub_addr == 0x0010);    /* it is RTS */
    CHECK(4, keeps_regs(stub_addr) == 0);
    return fails;
}
