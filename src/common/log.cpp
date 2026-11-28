#include "pw/log.hpp"

#include <pthread.h>
#include <sys/time.h>

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace pw {
namespace {

pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
FILE* g_file = nullptr;
bool g_console = false;

const char* level_name(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warn: return "warn";
        case LogLevel::Error: return "error";
    }
    return "info";
}

const char* or_dash(const char* s) { return (s != nullptr && s[0] != '\0') ? s : "-"; }

}  // namespace

void log_open(const std::string& path) {
    std::error_code ec;
    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    pthread_mutex_lock(&g_mu);
    if (g_file != nullptr) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_file = std::fopen(path.c_str(), "a");
    pthread_mutex_unlock(&g_mu);
}

void log_set_console(bool on) { g_console = on; }

void log_close() {
    pthread_mutex_lock(&g_mu);
    if (g_file != nullptr) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    pthread_mutex_unlock(&g_mu);
}

void log_line(LogLevel level, const char* module, const char* session,
              const char* rule, const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    struct timeval tv {};
    gettimeofday(&tv, nullptr);
    struct tm tm {};
    gmtime_r(&tv.tv_sec, &tm);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm);
    const int ms = static_cast<int>(tv.tv_usec / 1000);

    pthread_mutex_lock(&g_mu);
    if (g_file != nullptr) {
        std::fprintf(g_file, "%s.%03dZ %s %s %s %s %s\n", ts, ms, level_name(level),
                     or_dash(module), or_dash(session), or_dash(rule), msg);
        std::fflush(g_file);
    }
    if (g_console) {
        std::fprintf(stderr, "%s.%03dZ %s %s %s %s %s\n", ts, ms, level_name(level),
                     or_dash(module), or_dash(session), or_dash(rule), msg);
        std::fflush(stderr);
    }
    pthread_mutex_unlock(&g_mu);
}

}  // namespace pw
