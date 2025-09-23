#include "timeutil.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

double now_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int initialized = 0;
    if (!initialized) { QueryPerformanceFrequency(&freq); initialized = 1; }
    LARGE_INTEGER counter; QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#endif
}


