/*
 * fcntl(F_SETFL, O_NONBLOCK) for Emscripten's WASMFS.
 *
 * WASMFS (3.1.50) never blocks: a pipe read returns what the pipe holds, a
 * write always completes, and files live in memory. But its F_SETFL accepts
 * only O_APPEND and fails O_NONBLOCK with EINVAL, so QEMU's event notifiers
 * (glib g_unix_set_fd_nonblocking on a pipe) abort at startup. The final
 * link wraps fcntl (meson.build, -Wl,--wrap=fcntl): when F_SETFL fails with
 * EINVAL only because O_NONBLOCK was asked for, set the remaining flags; the
 * descriptor is already non-blocking. The JS filesystem (MEMFS) accepts
 * O_NONBLOCK, so there the first call succeeds and nothing changes.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>

int __real_fcntl(int fd, int cmd, ...);
int __wrap_fcntl(int fd, int cmd, ...);

int __wrap_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    unsigned long arg;
    int ret;

    /* As musl's fcntl: every command's argument fits an unsigned long. */
    va_start(ap, cmd);
    arg = va_arg(ap, unsigned long);
    va_end(ap);

    ret = __real_fcntl(fd, cmd, arg);
    if (ret < 0 && errno == EINVAL && cmd == F_SETFL && (arg & O_NONBLOCK)) {
        ret = __real_fcntl(fd, F_SETFL, arg & ~(unsigned long)O_NONBLOCK);
    }
    return ret;
}
