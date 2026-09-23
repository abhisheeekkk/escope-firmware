/* Minimal newlib syscall stubs for bare-metal.
 * These satisfy the linker when using -specs=nosys.specs is insufficient,
 * and provide a _write hook for semihosting-free printf (not used here). */
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

__attribute__((weak)) int _close(int fd)              { (void)fd; return -1; }
__attribute__((weak)) int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
__attribute__((weak)) int _isatty(int fd)             { (void)fd; return 1; }
__attribute__((weak)) int _lseek(int fd, int ptr, int dir) { (void)fd;(void)ptr;(void)dir; return 0; }
__attribute__((weak)) int _read(int fd, char *ptr, int len) { (void)fd;(void)ptr;(void)len; return 0; }
__attribute__((weak)) int _write(int fd, char *ptr, int len)
{
    /* Redirect to CDC if needed in future — for now discard */
    (void)fd; (void)ptr;
    return len;
}
