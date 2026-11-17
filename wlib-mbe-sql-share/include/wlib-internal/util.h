/*
 * A small collection of utils to solve recurring problems.
 */

#ifndef __util_h

#define __util_h

#include <stdint.h>
#include <time.h>

#include "status.h"

/* An ever growing storage area. The idea is that it will
 * saturate to the correct value */
struct u_area {
    void *ptr;
    size_t size;
};

typedef uint64_t timestamp_t;

/* Copy ASCIIZ string into a buffer, taking care of truncating, if 
 * necessary, and returning the real length. */
void u_copyz(char *dest, int maxlen, short *len, const char *src);

/* Get unix timestamp in milliseconds */
timestamp_t u_time(void);

/* Unix (ms) to Julian (us) */
timestamp_t u_unix_to_julian(timestamp_t timestamp);

/* Julian (us) to Unix (ms) */
timestamp_t u_julian_to_unix(timestamp_t timestamp);

/* Timespec to Julian (us) */
timestamp_t u_timespect_to_julian(struct timespec *ts);
/* Generate a random string printable of given length. Not very fast but
 * of rather good quality */
void u_random_string(char *buf, int len);

/* Enlarge the area to make sure that it is at least size bytes.
 * The previous contents may be destroyed */
enum wl_status u_ensure_size(struct u_area *area, size_t size);

/* Enlarge the area to make sure that it is at least size bytes.
 * The previous contesnts are always preserved */
enum wl_status u_ensure_size_keep(struct u_area *area, size_t size);

#endif
