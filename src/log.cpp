#include "log.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace tr {

static FILE* g_log = nullptr;
static CRITICAL_SECTION g_lock;
static bool g_lock_init = false;

void log_init(const wchar_t* path) {
    if (!g_lock_init) {
        InitializeCriticalSection(&g_lock);
        g_lock_init = true;
    }
    if (g_log) return;
    g_log = _wfopen(path, L"a");
}

void logf(const char* fmt, ...) {
    if (!g_lock_init) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g_lock);
    if (g_log) {
        fprintf(g_log, "[%10llu] %s\n", (unsigned long long)GetTickCount64(), buf);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_lock);
    OutputDebugStringA(buf);
}

}  // namespace tr
