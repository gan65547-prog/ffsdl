/*
 * ml_os — OS-level helpers.
 *
 * Provides page size query and a millisecond monotonic clock (ml_uptime)
 * used by the timer wheel and event loop.
 */
#ifndef __ML_OS_H__
#define __ML_OS_H__

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "core/ml_types.h"

typedef long ml_time_t;

/* Return the system page size. */
static inline ml_int_t ml_pagesize(void) {
    return (ml_int_t)sysconf(_SC_PAGESIZE);
}

/* Monotonic millisecond clock — immune to NTP / wall-clock adjustments. */
static inline ml_int64_t ml_uptime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ml_int64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* 本地时区当日日期，格式 YYYYMMDD（含 NUL 需 >=9 字节）。
 * 日切文件名 / 日期比对统一入口，替代各处 localtime_r+strftime 样板。 */
static inline void ml_date_yyyymmdd(char *buf, size_t n) {
    time_t t = time(NULL);
    struct tm tmbuf;
    if (!buf || n == 0) return;
    localtime_r(&t, &tmbuf);
    strftime(buf, n, "%Y%m%d", &tmbuf);
    buf[n - 1] = '\0';
}

/* UTC 当前时间，RFC 3339 毫秒精度：2026-09-13T12:34:56.789Z。
 * 协议时间戳统一入口；buf 需 >=25 字节。返回 0 成功，-1 缓冲不足。 */
static inline int ml_time_rfc3339_ms(char *buf, size_t n) {
    struct timespec ts;
    struct tm tmbuf;
    size_t len;
    if (!buf || n < 25) return -1;
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tmbuf);
    strftime(buf, n, "%Y-%m-%dT%H:%M:%S", &tmbuf);
    len = strlen(buf);
    snprintf(buf + len, n - len, ".%03ldZ", (long)(ts.tv_nsec / 1000000));
    return 0;
}

#endif
