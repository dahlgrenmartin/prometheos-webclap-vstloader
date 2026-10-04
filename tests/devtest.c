// Phase 0, task 2: can a Windows program under Wine reach a Boxedwine device?
//
// 1. Through Wine: CreateFileA("Z:\\dev\\..."), ReadFile, WriteFile and
//    DeviceIoControl, which Wine turns into open/read/write/ioctl on the unix
//    side (Boxedwine's kernel).
// 2. Directly: Linux i386 system calls (int 0x80) from Windows code. Boxedwine
//    emulates the Linux kernel, and Wine runs Windows code as an ordinary Linux
//    process, so the same instruction reaches Boxedwine's syscall table without
//    going through Wine's I/O layer.
//
// Prints one line per probe and writes them to C:\vstpoc-out\devtest.txt.
#include <windows.h>
#include <stdio.h>
#include <string.h>

static FILE *out;

static void say(const char *fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    printf("devtest: %s\n", line);
    fflush(stdout);
    if (out) {
        fprintf(out, "%s\n", line);
        fflush(out);
    }
}

static int linux3(int nr, int a, int b, int c) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}

enum { SYS_read = 3, SYS_write = 4, SYS_open = 5, SYS_close = 6, SYS_ioctl = 54, O_RDWR = 2 };

static double nowUs(void) {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return 1e6 * (double)t.QuadPart / (double)f.QuadPart;
}

static void throughWine(const char *path, int doRead, int doWrite) {
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        say("wine %s: CreateFileA failed, error %lu", path, GetLastError());
        return;
    }
    say("wine %s: CreateFileA ok, type %lu", path, GetFileType(h));
    unsigned char buf[64];
    DWORD n = 0;
    if (doRead) {
        BOOL ok = ReadFile(h, buf, sizeof buf, &n, NULL);
        say("wine %s: ReadFile ok=%d bytes=%lu first=%02x%02x error=%lu", path, ok, n, buf[0], buf[1],
            ok ? 0 : GetLastError());
        double t0 = nowUs();
        for (int i = 0; i < 1000; ++i) ReadFile(h, buf, 16, &n, NULL);
        say("wine %s: 1000 x ReadFile(16) = %.1f us each", path, (nowUs() - t0) / 1000);
    }
    if (doWrite) {
        BOOL ok = WriteFile(h, "hello", 5, &n, NULL);
        say("wine %s: WriteFile ok=%d bytes=%lu error=%lu", path, ok, n, ok ? 0 : GetLastError());
        double t0 = nowUs();
        for (int i = 0; i < 1000; ++i) WriteFile(h, buf, 16, &n, NULL);
        say("wine %s: 1000 x WriteFile(16) = %.1f us each", path, (nowUs() - t0) / 1000);
    }
    DWORD ret = 0;
    unsigned in = 0x1234, outv = 0;
    BOOL ok = DeviceIoControl(h, CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS), &in,
                              sizeof in, &outv, sizeof outv, &ret, NULL);
    say("wine %s: DeviceIoControl ok=%d returned=%lu error=%lu", path, ok, ret, ok ? 0 : GetLastError());
    CloseHandle(h);
}

static void direct(const char *path, int doRead, int doWrite) {
    int fd = linux3(SYS_open, (int)path, O_RDWR, 0);
    say("int80 %s: open -> %d", path, fd);
    if (fd < 0) return;
    unsigned char buf[64];
    if (doRead) {
        int n = linux3(SYS_read, fd, (int)buf, sizeof buf);
        say("int80 %s: read -> %d first=%02x%02x", path, n, buf[0], buf[1]);
        double t0 = nowUs();
        for (int i = 0; i < 1000; ++i) linux3(SYS_read, fd, (int)buf, 16);
        say("int80 %s: 1000 x read(16) = %.1f us each", path, (nowUs() - t0) / 1000);
    }
    if (doWrite) {
        int n = linux3(SYS_write, fd, (int)"hello", 5);
        say("int80 %s: write -> %d", path, n);
        double t0 = nowUs();
        for (int i = 0; i < 1000; ++i) linux3(SYS_write, fd, (int)buf, 16);
        say("int80 %s: 1000 x write(16) = %.1f us each", path, (nowUs() - t0) / 1000);
    }
    int r = linux3(SYS_ioctl, fd, 0x5600, 0);
    say("int80 %s: ioctl(0x5600) -> %d", path, r);
    linux3(SYS_close, fd, 0, 0);
}

int main(void) {
    CreateDirectoryA("C:\\vstpoc-out", NULL);
    out = fopen("C:\\vstpoc-out\\devtest.txt", "w");
    throughWine("Z:\\dev\\null", 1, 1);
    throughWine("Z:\\dev\\urandom", 1, 0);
    throughWine("Z:\\dev\\vstbridge", 1, 1);
    direct("/dev/null", 1, 1);
    direct("/dev/urandom", 1, 0);
    direct("/dev/vstbridge", 0, 0);
    say("done");
    if (out) fclose(out);
    return 0;
}
