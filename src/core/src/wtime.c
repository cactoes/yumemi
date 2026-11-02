#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
// #define NOUSER
#include <windows.h>
#include <timeapi.h>
#include "wtime.h"

u64 time_now_ns() {
    static LARGE_INTEGER freq;
    LARGE_INTEGER counter;

    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);

    QueryPerformanceCounter(&counter);

    u64 secs = (u64)(counter.QuadPart / freq.QuadPart);
    u64 rem  = (u64)(counter.QuadPart % freq.QuadPart);
    return secs * 1000000000ull + rem * 1000000000ull / (u64)freq.QuadPart;
}

void sleep_until_ns(u64 deadline) {
    static bool timer_init = false;
    if (!timer_init) {
        timeBeginPeriod(1);
        timer_init = true;
    }

    const u64 spin_margin = 1500000ull;

    u64 now = time_now_ns();
    if (now >= deadline)
        return;

    if (deadline - now > spin_margin) {
        u64 sleep_ms = (deadline - now - spin_margin) / 1000000ull;
        if (sleep_ms > 0)
            Sleep((DWORD)sleep_ms);
    }

    while (time_now_ns() < deadline)
        YieldProcessor();
}