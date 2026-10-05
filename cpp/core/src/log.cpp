#include "uti120/log.hpp"

#include <atomic>

#ifdef __ANDROID__
#include <android/log.h>
#else
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#endif

namespace uti120 {

static std::atomic<int> g_level{static_cast<int>(Level::Warning)};

void set_log_level(Level level) { g_level = static_cast<int>(level); }

bool log_enabled(Level level) { return static_cast<int>(level) >= g_level; }

#ifdef __ANDROID__

// An app's stderr goes nowhere: write to logcat, which adds its own time stamp.
void log(Level level, const char* component, const char* fmt, ...) {
  static const android_LogPriority prio[] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO,
                                             ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
  va_list ap;
  va_start(ap, fmt);
  __android_log_vprint(prio[static_cast<int>(level) / 10 - 1], component, fmt, ap);
  va_end(ap);
}

#else

static std::mutex g_mutex;

static const char* level_name(Level level) {
  switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warning: return "WARNING";
    case Level::Error: return "ERROR";
  }
  return "?";
}

void log(Level level, const char* component, const char* fmt, ...) {
  using namespace std::chrono;
  auto now = system_clock::now();
  std::time_t t = system_clock::to_time_t(now);
  int ms = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
  std::tm tm{};
  localtime_r(&t, &tm);
  char stamp[32];
  std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);

  std::lock_guard lock(g_mutex);
  std::fprintf(stderr, "%s,%03d %s %s: ", stamp, ms, component, level_name(level));
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fputc('\n', stderr);
}

#endif

}  // namespace uti120
