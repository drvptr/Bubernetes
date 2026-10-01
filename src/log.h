#ifndef LOG_H
#define LOG_H

#include <errno.h>

/*
 * Logging. Same four names the original bubelet used (INFO/WARN/ERR/PERR),
 * but printf-style so call sites can say what actually happened. PERR appends
 * strerror(errno); the others do not touch errno.
 *
 * Deliberately not a clever macro: it just forwards to one plain function.
 */
void bube_log(const char *level, const char *func, int err, const char *fmt, ...);

#define INFO(...) bube_log("INFO", __func__, 0,     __VA_ARGS__)
#define WARN(...) bube_log("WARN", __func__, 0,     __VA_ARGS__)
#define ERR(...)  bube_log("ERR ", __func__, 0,     __VA_ARGS__)
#define PERR(...) bube_log("ERR ", __func__, errno, __VA_ARGS__)

#endif
