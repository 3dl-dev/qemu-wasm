#!/usr/bin/env python3
"""List the JS library functions an Emscripten glue (out.js) proxies to the
runtime ("main") thread, and optionally fail if any of them is file, socket
or tty I/O.

With the JS filesystem (MEMFS) every __syscall_* / fd_* the QEMU pthreads
make is proxied synchronously to the runtime thread (OpenVMX rd vms-e79).
A WASMFS build keeps that I/O in Wasm on the calling thread, so its glue
must not proxy any of it. Usage: proxied-calls.py out.js [--forbid-io]
"""
import re
import sys

IO = re.compile(r'^(syscall_|fd_|wasmfs_|environ_|emscripten_stdin|mmap_js|munmap_js|msync_js|xterm_pty_|PTY_)')


def proxied(js):
    names = []
    for m in re.finditer(r'proxyToMainThread\(', js):
        head = js[max(0, m.start() - 400):m.start()]
        defs = re.findall(r'(?:function\s+([A-Za-z0-9_$]+)\s*\(|'
                          r'var\s+([A-Za-z0-9_$]+)\s*=\s*(?:function\s*)?\()', head)
        if not defs:
            continue
        a, b = defs[-1]
        name = a or b
        if name and name != 'proxyToMainThread':
            names.append(name)
    return sorted(set(names))


def main():
    path = sys.argv[1]
    forbid = '--forbid-io' in sys.argv[2:]
    js = open(path, encoding='utf-8', errors='replace').read()
    names = proxied(js)
    io = [n for n in names if IO.match(n.lstrip('_'))]
    print('%s proxies %d function(s) to the runtime thread:' % (path, len(names)))
    for n in names:
        print('  %s%s' % (n, '   <- I/O' if n in io else ''))
    print('I/O proxied to the runtime thread: %d' % len(io))
    if forbid and io:
        print('FAIL: this glue proxies file/socket/tty I/O to the runtime thread')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
