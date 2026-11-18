/*
 * The few C library functions that the tests need on WebAssembly, so that they run without a WASI sysroot.
 * Zig's compiler runtime already has the memory functions and strlen().
 * Printing and exiting use two WASI calls, which both wasmer and wasmtime support.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct wasi_iovec {
    const void *buf;
    size_t      len;
} wasi_iovec;

__attribute__((import_module("wasi_snapshot_preview1"), import_name("fd_write"))) int32_t
wasi_fd_write(int32_t fd, const wasi_iovec *iovs, size_t iovs_len, size_t *written);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("proc_exit"))) _Noreturn void
wasi_proc_exit(int32_t code);

int
strcmp(const char *a, const char *b)
{
    while (*a != 0 && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char) *a - (unsigned char) *b;
}

static void
write_out(const char *s, size_t len)
{
    const wasi_iovec iov = { s, len };
    size_t           written;

    wasi_fd_write(1, &iov, 1, &written);
}

/* Only %s, and %d for numbers that aren't negative. */
int
printf(const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    for (const char *p = format; *p != 0; p++) {
        if (p[0] != '%' || (p[1] != 's' && p[1] != 'd')) {
            write_out(p, 1);
            continue;
        }
        p++;
        if (*p == 's') {
            const char *s = va_arg(ap, const char *);
            write_out(s, strlen(s));
        } else {
            unsigned n = (unsigned) va_arg(ap, int);
            char     digits[10];
            size_t   i = sizeof digits;

            do {
                digits[--i] = (char) ('0' + n % 10);
                n /= 10;
            } while (n != 0);
            write_out(digits + i, sizeof digits - i);
        }
    }
    va_end(ap);
    return 0;
}

extern unsigned char __heap_base;

/* Memory is never freed, since the tests don't allocate much. */
void *
malloc(size_t size)
{
    static uintptr_t next  = (uintptr_t) &__heap_base;
    const uintptr_t  start = (next + 15) & ~(uintptr_t) 15;
    size_t           pages;

    if (size > SIZE_MAX - start) {
        return NULL;
    }
    next  = start + size;
    pages = (next + 65535) / 65536;
    if (pages > __builtin_wasm_memory_size(0) &&
        __builtin_wasm_memory_grow(0, pages - __builtin_wasm_memory_size(0)) == SIZE_MAX) {
        return NULL;
    }
    return (void *) start;
}

void
free(void *p)
{
    (void) p;
}

_Noreturn void
abort(void)
{
    __builtin_trap();
}

int main(void);

void
_start(void)
{
    wasi_proc_exit(main());
}
