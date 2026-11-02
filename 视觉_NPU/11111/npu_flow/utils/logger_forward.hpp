#pragma once
#include "fmt/core.h"

struct __logger_initliazer {
    int __log_level_int; // 0: verbose 1: debug, 2: info, 3: warn, 4: error, 5: critical, 6: fatal
    static const char *__log_level_str[7];

    __logger_initliazer();
    void set_log_level(int log_level_int);
    static std::string get_time();
};
extern __logger_initliazer __logger_initliazer_instance;

static inline void set_log_level(int log_level_int) {
    __logger_initliazer_instance.set_log_level(log_level_int);
}

static inline int get_log_level() {
    return __logger_initliazer_instance.__log_level_int;
}

#define __FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)
#define __LOG_FMT(lvl, ...)                                                        \
    if (__logger_initliazer_instance.__log_level_int <= lvl) {                     \
        fmt::print("[{}][{}] [{}:{}] ", __logger_initliazer::__log_level_str[lvl], \
                   __logger_initliazer::get_time(), __FILENAME__, __LINE__);       \
        fmt::print(__VA_ARGS__);                                                   \
        fmt::print("\r\n");                                                        \
    }

#define LOG_VERBOSE(...) __LOG_FMT(0, __VA_ARGS__)
#define LOG_TRACE LOG_VERBOSE
#define LOG_DEBUG(...) __LOG_FMT(1, __VA_ARGS__)
#define LOG_INFO(...) __LOG_FMT(2, __VA_ARGS__)
#define LOG_WARN(...) __LOG_FMT(3, __VA_ARGS__)
#define LOG_ERROR(...) __LOG_FMT(4, __VA_ARGS__)
#define LOG_CRITICAL(...) __LOG_FMT(5, __VA_ARGS__)
#define LOG_FATAL(...) __LOG_FMT(6, __VA_ARGS__)
