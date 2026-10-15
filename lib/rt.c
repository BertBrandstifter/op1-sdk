/* op1-sdk runtime: the four functions GCC may emit calls to even in freestanding code (struct
 * copies, initializers). Always linked by tools/build.py; --gc-sections drops unused ones.
 * Loop-to-call pattern detection is off so these never call themselves. */
#include <stddef.h>

#define RT __attribute__((optimize("no-tree-loop-distribute-patterns")))

RT void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    while (n--)
        *d++ = *s++;
    return dst;
}

RT void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (d < s)
        while (n--)
            *d++ = *s++;
    else
        while (n--)
            d[n] = s[n];
    return dst;
}

RT void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;

    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

RT int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;

    for (; n--; x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}
