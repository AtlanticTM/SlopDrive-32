// In-memory log buffer implementation - SlopDrive-32
#include "applog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Ring buffer of recent log lines. Kept small to bound RAM use.
static const int   LOG_LINES = 60;
static const int   LOG_LINE_LEN = 96;
static char        s_lines[LOG_LINES][LOG_LINE_LEN];
static int         s_head = 0;     // next write slot
static int         s_count = 0;    // number of valid lines
static SemaphoreHandle_t s_mtx = nullptr;

void applogBegin() {
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
}

void applog(const char* line) {
    if (!s_mtx) {  // allow logging before begin() (best effort, no lock)
        strncpy(s_lines[s_head], line, LOG_LINE_LEN - 1);
        s_lines[s_head][LOG_LINE_LEN - 1] = '\0';
        s_head = (s_head + 1) % LOG_LINES;
        if (s_count < LOG_LINES) s_count++;
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        strncpy(s_lines[s_head], line, LOG_LINE_LEN - 1);
        s_lines[s_head][LOG_LINE_LEN - 1] = '\0';
        s_head = (s_head + 1) % LOG_LINES;
        if (s_count < LOG_LINES) s_count++;
        xSemaphoreGive(s_mtx);
    }
}

void applogf(const char* fmt, ...) {
    char buf[LOG_LINE_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    applog(buf);
}

void applogDump(String& out) {
    if (s_mtx) xSemaphoreTake(s_mtx, pdMS_TO_TICKS(20));
    // Walk from oldest to newest.
    int start = (s_count < LOG_LINES) ? 0 : s_head;
    for (int i = 0; i < s_count; i++) {
        int idx = (start + i) % LOG_LINES;
        out += s_lines[idx];
        out += '\n';
    }
    if (s_mtx) xSemaphoreGive(s_mtx);
}
