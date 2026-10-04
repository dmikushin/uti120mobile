#pragma once

#include <cstdarg>

namespace uti120 {

enum class Level { Debug = 10, Info = 20, Warning = 30, Error = 40 };

// Messages below this level are dropped (default: Warning; -v Info, -vv Debug).
void set_log_level(Level level);
bool log_enabled(Level level);
void log(Level level, const char* component, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

}  // namespace uti120

#define UTI120_LOG(level, component, ...)                                   \
  do {                                                                      \
    if (::uti120::log_enabled(level)) ::uti120::log(level, component, __VA_ARGS__); \
  } while (0)
#define LOG_DEBUG(c, ...) UTI120_LOG(::uti120::Level::Debug, c, __VA_ARGS__)
#define LOG_INFO(c, ...) UTI120_LOG(::uti120::Level::Info, c, __VA_ARGS__)
#define LOG_WARNING(c, ...) UTI120_LOG(::uti120::Level::Warning, c, __VA_ARGS__)
