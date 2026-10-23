// Pulled into every C translation unit of the build (-include): the console's
// libc has no C11 timespec_get, which the core's nanotime.h wants for its clock.
#pragma once

#ifndef __cplusplus
#include <time.h>

#ifndef TIME_UTC
#define TIME_UTC 1
static inline int timespec_get(struct timespec* ts, int base) {
    // monotonic: the core only measures frame intervals with it
    return base == TIME_UTC && clock_gettime(CLOCK_MONOTONIC, ts) == 0 ? base : 0;
}
#endif
#endif
