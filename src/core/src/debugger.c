#include "debugger.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <direct.h>

#define LOG_LINES     512
#define LOG_LINE_LEN  256

static char crash_log_buffer[256];
const char* crash_log_path = NULL;

static char log_ring[LOG_LINES][LOG_LINE_LEN];
static u32 log_next = 0;

void debugger_init() {
    time_t now = time(NULL);
    struct tm t;
    localtime_s(&t, &now);
    snprintf(crash_log_buffer, sizeof(crash_log_buffer), "logs/crash_log_%04d-%02d-%02d_%02d-%02d-%02d.txt", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    crash_log_path = crash_log_buffer;
}

void debug_log(const char* text, ...) {
    u32 idx = log_next++;
    char* line = log_ring[idx % LOG_LINES];

    va_list args;
    va_start(args, text);
    vsnprintf(line, LOG_LINE_LEN, text, args);
    va_end(args);
}

void dump_crash_log() {
    if (!crash_log_path)
        return;

    _mkdir("logs");

    FILE* fp = fopen(crash_log_path, "w");
    if (!fp)
        return;

    u32 total = log_next;
    u32 count = total < LOG_LINES ? total : LOG_LINES;
    u32 start = total - count;

    for (u32 i = 0; i < count; i++)
        fprintf(fp, "%s", log_ring[(start + i) % LOG_LINES]);

    fclose(fp);
}