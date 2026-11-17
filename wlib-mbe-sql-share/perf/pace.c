/*
 * Clock, random numbers, the mix of patterns and waiting for the next operation.
 */

#include <string.h>
#include <time.h>

#include "mbeperf.h"

int64_t now_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (int64_t) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* xorshift64*: good enough, and every worker has its own state */
uint64_t rng_next(uint64_t *state) {
    uint64_t x = *state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;

    return x * 0x2545F4914F6CDD1DULL;
}

/* In [0, 1) */
double rng_double(uint64_t *state) {
    return (rng_next(state) >> 11) * (1.0 / 9007199254740992.0);
}

/* The percentages are relative: they are scaled so that they add up to 100. A mix of zeroes
 * gets the defaults (without p5 if there is no alternate key) */
void normalize_mix(struct perf_cfg *cfg) {
    static const double defaults[PAT__COUNT] = { 40, 20, 5, 5, 20, 10 };
    double sum = 0;

    for (int i = 0; i < PAT__COUNT; i++) {
        if (cfg -> mix[i] < 0) {
            cfg -> mix[i] = 0;
        }
        sum += cfg -> mix[i];
    }

    if (sum <= 0) {
        memcpy(cfg -> mix, defaults, sizeof(defaults));
        if (!cfg -> altkey) {
            cfg -> mix[PAT_ALTKEY] = 0;
        }
        sum = 0;
        for (int i = 0; i < PAT__COUNT; i++) {
            sum += cfg -> mix[i];
        }
    }

    for (int i = 0; i < PAT__COUNT; i++) {
        cfg -> mix[i] = cfg -> mix[i] * 100.0 / sum;
    }
}

/* Chooses the pattern of the next operation according to the percentages */
int pick_pattern(const struct perf_cfg *cfg, uint64_t *rng) {
    double r = rng_double(rng) * 100.0;
    double acc = 0;
    int last = 0;

    for (int i = 0; i < PAT__COUNT; i++) {
        if (cfg -> mix[i] <= 0) {
            continue;
        }
        last = i;
        acc += cfg -> mix[i];
        if (r < acc) {
            return i;
        }
    }

    /* Rounding: r can be a hair over the last accumulated value */
    return last;
}

/* Sleeps until the (CLOCK_MONOTONIC) time. Returns at once if it has passed */
void sleep_until(int64_t when_ns) {
    struct timespec ts;

    ts.tv_sec = when_ns / 1000000000LL;
    ts.tv_nsec = when_ns % 1000000000LL;

    /* If a signal (Ctrl-C) interrupts the wait, the caller checks the stop flag */
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
}
