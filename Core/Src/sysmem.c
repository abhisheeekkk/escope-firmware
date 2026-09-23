#include <errno.h>
#include <stdint.h>

extern char _end;      /* Defined in linker script — end of BSS */
static char *heap_ptr;

void *_sbrk(ptrdiff_t incr)
{
    // heap grows up from _end
    char *prev;

    if (heap_ptr == 0) heap_ptr = &_end;

    prev = heap_ptr;
    heap_ptr += incr;
    return (void *)prev;
}
