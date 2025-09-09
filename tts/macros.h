#pragma once
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <chrono>

#define timelog(fmt, ...) do { \
    time_t rawtime; \
    struct tm *timeinfo; \
    char buffer[20]; \
    time(&rawtime); \
    timeinfo = localtime(&rawtime); \
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo); \
    printf("[%s] " fmt "\n", buffer, ##__VA_ARGS__); \
} while(0)
#define delay_exit(ms) do { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); exit(0); } while(0)

const int default_exit_value = 25000;