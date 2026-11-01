/* libgcc for m68k-amigaos has no __atomic_* helpers.  The unit tests are
 * single-threaded, so plain read-modify-write is enough here. */
#include <stdint.h>
uint32_t __atomic_fetch_add_4(volatile void *p, uint32_t v, int order)
{
    volatile uint32_t *q = (volatile uint32_t *)p;
    uint32_t old = *q;
    (void)order;
    *q = old + v;
    return old;
}
