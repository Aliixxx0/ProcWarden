#pragma once

#include <string>

namespace pw {

enum class LogLevel { Debug, Info, Warn, Error };

// Opens ~/.procwarden/warden.log (appends). Safe to call once per process.
void log_open(const std::string& path);
void log_set_console(bool on);
void log_close();

// One line per event: ISO-time level module session rule message
void log_line(LogLevel level, const char* module, const char* session,
              const char* rule, const char* fmt, ...)
    __attribute__((format(printf, 5, 6)));

}  // namespace pw

#define PW_LOG(lvl, mod, sess, rule, ...) \
    ::pw::log_line(lvl, mod, sess, rule, __VA_ARGS__)
