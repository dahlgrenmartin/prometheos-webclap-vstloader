/* Diagnostic: does creating a (hidden) window work in this Boxedwine/Wine setup?
 * JUCE plugins create a hidden message window inside VSTPluginMain. */
#include <windows.h>
#include <stdio.h>

static void mark(const char *what) {
    FILE *f = fopen("wintest.log", "ab");
    if (f) { fprintf(f, "%lu %s\n", (unsigned long)GetTickCount(), what); fclose(f); }
}

int main(void) {
    DeleteFileA("wintest.log");
    mark("start");
    HWND msg = CreateWindowExA(0, "STATIC", "msg", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
    mark(msg ? "message-only window ok" : "message-only window failed");
    HWND hidden = CreateWindowExA(0, "STATIC", "hidden", WS_POPUP, 0, 0, 10, 10, NULL, NULL, NULL, NULL);
    mark(hidden ? "hidden top-level window ok" : "hidden top-level window failed");
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&m);
    mark("done");
    return 0;
}
