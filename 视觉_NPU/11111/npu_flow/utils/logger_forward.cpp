#include "logger_forward.hpp"
// 0: verbose 1: debug, 2: info, 3: warn, 4: error, 5: critical, 6: fatal
// 默认为info

__logger_initliazer __logger_initliazer_instance;
__logger_initliazer::__logger_initliazer()
    : __log_level_int(2) {
    const char *log_level = getenv("LOG_LEVEL");
    if (log_level != nullptr) {
        int log_level_int = std::stoi(log_level);
        set_log_level(log_level_int);
    }
}

void __logger_initliazer::set_log_level(int log_level_int) {
    if (log_level_int < 0 || log_level_int > 6) {
    } else {
        __log_level_int = log_level_int;
    }
}

// const char *__logger_initliazer::__log_level_str[7] = {
//     "VERBOSE",
//     "DEBUG",
//     "INFO",
//     "WARN",
//     "ERROR",
//     "CRITICAL",
//     "FATAL"};

const char *__logger_initliazer::__log_level_str[7] = {
    "V",
    "D",
    "\033[32mI\033[0m",
    "\033[33mW\033[0m",
    "\033[31mE\033[0m",
    "\033[31mC\033[0m",
    "\033[31mF\033[0m"};

std::string __logger_initliazer::get_time() {
    time_t timep;
    struct tm *p;

    time(&timep);          // 获取从1970至今过了多少秒，存入time_t类型的timep
    p = localtime(&timep); // 用localtime将秒数转化为struct tm结构体
    // return fmt::format("{:04d}/{:02d}/{:02d} {:02d}:{:02d}:{:02d}",
    //                    1900 + p->tm_year, 1 + p->tm_mon, p->tm_mday,
    //                    p->tm_hour, p->tm_min, p->tm_sec);

    return fmt::format("{:02d}/{:02d} {:02d}:{:02d}:{:02d}",
                       1 + p->tm_mon, p->tm_mday,
                       p->tm_hour, p->tm_min, p->tm_sec);
}
