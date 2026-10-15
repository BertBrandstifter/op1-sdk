/* First C example: the trampoline in entry.S calls count_lookup() on every
 * SeqIds name lookup the sequencer manager makes. Records how often it ran and
 * the last name it saw, using the te_string layout from te246.h. Both globals
 * land in .bss, which the boot loader zeroes (sdk.ld folds it into the image). */
#include <op1/te246.h>

unsigned long lookup_count;
char last_name[32];

void count_lookup(const te_string *name)
{
    const char *s = te_str_data(name);
    unsigned int n = name->size;
    unsigned int i;

    lookup_count++;
    if (n > sizeof last_name - 1)
        n = sizeof last_name - 1;
    for (i = 0; i < n; i++)
        last_name[i] = s[i];
    last_name[n] = '\0';
}
