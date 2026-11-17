/*
 * Statistics: per pattern counters and latency histograms, and the final report.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbeperf.h"

/* Bucket of a value: below 8 each value has its own bucket, above it there are 8 buckets for
 * each power of two (an error of 12.5% at most) */
static int hist_index(uint64_t ns) {
    int lg, sub;

    if (ns < 8) {
        return (int) ns;
    }

    lg = 63 - __builtin_clzll(ns);
    sub = (int) ((ns >> (lg - 3)) & 7);

    return (lg - 2) * 8 + sub;
}

/* The upper limit of a bucket */
static uint64_t hist_upper(int idx) {
    int lg, sub;

    if (idx < 8) {
        return (uint64_t) idx;
    }

    lg = idx / 8 + 2;
    sub = idx % 8;

    return (((uint64_t) (8 + sub + 1)) << (lg - 3)) - 1;
}

void stats_init(struct worker_slot *slot) {
    memset(slot, 0, sizeof(*slot));
    for (int i = 0; i < PAT__COUNT; i++) {
        slot -> stats[i].svc_min_ns = UINT64_MAX;
    }
}

void stats_record(struct worker *w, int pattern, int outcome, int64_t svc_ns, int64_t sched_ns, int late) {
    struct pat_stats *ps = &w -> slot -> stats[pattern];

    if (svc_ns < 0) {
        svc_ns = 0;
    }
    if (sched_ns < svc_ns) {
        sched_ns = svc_ns;
    }

    ps -> ops++;
    ps -> rows += w -> rows;
    ps -> late += late;
    switch (outcome) {
    case OUT_OK:    ps -> ok++; break;
    case OUT_MISS:  ps -> miss++; break;
    case OUT_RACE:  ps -> race++; break;
    default:
        ps -> err++;
        for (int i = 0; ; i++) {
            struct worker_slot *slot = w -> slot;

            if (i == slot -> nerrors) {
                if (i == MAX_ERRORS) {
                    /* Too many different ones: the last entry takes all the rest */
                    slot -> errors[MAX_ERRORS - 1].count++;
                    break;
                }
                slot -> errors[i].code = w -> last_status;
                slot -> errors[i].pattern = pattern;
                slot -> errors[i].count = 1;
                slot -> nerrors++;
                break;
            }
            if (slot -> errors[i].code == w -> last_status && slot -> errors[i].pattern == pattern) {
                slot -> errors[i].count++;
                break;
            }
        }
    }

    ps -> svc_sum_ns += svc_ns;
    if ((uint64_t) svc_ns < ps -> svc_min_ns) {
        ps -> svc_min_ns = svc_ns;
    }
    if ((uint64_t) svc_ns > ps -> svc_max_ns) {
        ps -> svc_max_ns = svc_ns;
    }
    ps -> sched_sum_ns += sched_ns;
    if ((uint64_t) sched_ns > ps -> sched_max_ns) {
        ps -> sched_max_ns = sched_ns;
    }
    ps -> svc[hist_index(svc_ns)]++;
    ps -> sched[hist_index(sched_ns)]++;
}

/* Value (ns) below which pct % of the samples are (to the precision of the buckets) */
uint64_t hist_percentile(const uint64_t *hist, double pct, uint64_t max_ns) {
    uint64_t total = 0, acc = 0, target;

    for (int i = 0; i < HIST_BUCKETS; i++) {
        total += hist[i];
    }
    if (total == 0) {
        return 0;
    }

    target = (uint64_t) (total * pct / 100.0 + 0.999999);
    if (target < 1) {
        target = 1;
    }

    for (int i = 0; i < HIST_BUCKETS; i++) {
        acc += hist[i];
        if (acc >= target) {
            /* The bucket is a range: the real value can't be above the maximum */
            return hist_upper(i) < max_ns ? hist_upper(i) : max_ns;
        }
    }

    return max_ns;
}

/* Adds up the stats of all the workers */
static void merge(struct pat_stats *dst, const struct pat_stats *src) {
    dst -> ops += src -> ops;
    dst -> ok += src -> ok;
    dst -> miss += src -> miss;
    dst -> race += src -> race;
    dst -> err += src -> err;
    dst -> late += src -> late;
    dst -> rows += src -> rows;
    dst -> svc_sum_ns += src -> svc_sum_ns;
    dst -> sched_sum_ns += src -> sched_sum_ns;
    if (src -> svc_min_ns < dst -> svc_min_ns) {
        dst -> svc_min_ns = src -> svc_min_ns;
    }
    if (src -> svc_max_ns > dst -> svc_max_ns) {
        dst -> svc_max_ns = src -> svc_max_ns;
    }
    if (src -> sched_max_ns > dst -> sched_max_ns) {
        dst -> sched_max_ns = src -> sched_max_ns;
    }
    for (int i = 0; i < HIST_BUCKETS; i++) {
        dst -> svc[i] += src -> svc[i];
        dst -> sched[i] += src -> sched[i];
    }
}

static void total_stats(struct worker_slot *slots, int clients, struct pat_stats *per_pattern, struct pat_stats *all) {
    memset(per_pattern, 0, PAT__COUNT * sizeof(*per_pattern));
    memset(all, 0, sizeof(*all));
    for (int p = 0; p < PAT__COUNT; p++) {
        per_pattern[p].svc_min_ns = UINT64_MAX;
    }
    all -> svc_min_ns = UINT64_MAX;

    for (int c = 0; c < clients; c++) {
        for (int p = 0; p < PAT__COUNT; p++) {
            merge(&per_pattern[p], &slots[c].stats[p]);
            merge(all, &slots[c].stats[p]);
        }
    }
}

static double us(uint64_t ns) {
    return ns / 1000.0;
}

int stats_report(const struct perf_cfg *cfg, struct worker_slot *slots, int clients, double wall_s) {
    struct pat_stats per_pattern[PAT__COUNT], all;
    uint64_t errors = 0;

    total_stats(slots, clients, per_pattern, &all);

    printf("\nResults (%d client%s, %.1f s measured)\n\n", clients, clients == 1 ? "" : "s", wall_s);

    printf("%-11s %9s %8s %7s %9s %9s %6s %6s %7s %9s\n",
        "pattern", "ops", "ops/s", "share%", "ok", "miss", "race", "err", "late", "rows/op");
    for (int p = 0; p <= PAT__COUNT; p++) {
        struct pat_stats *ps = p < PAT__COUNT ? &per_pattern[p] : &all;

        if (ps -> ops == 0 && p < PAT__COUNT) {
            continue;
        }
        printf("%-11s %9llu %8.1f %7.1f %9llu %9llu %6llu %6llu %7llu %9.2f\n",
            p < PAT__COUNT ? pat_names[p] : "total",
            (unsigned long long) ps -> ops, ps -> ops / wall_s,
            all.ops ? 100.0 * ps -> ops / all.ops : 0.0,
            (unsigned long long) ps -> ok, (unsigned long long) ps -> miss,
            (unsigned long long) ps -> race, (unsigned long long) ps -> err,
            (unsigned long long) ps -> late,
            ps -> ops ? (double) ps -> rows / ps -> ops : 0.0);
        errors += p < PAT__COUNT ? ps -> err : 0;
    }

    printf("\nLatency, microseconds. Service: from the start of the operation. "
           "Sched: from the time it was due to start\n\n");
    printf("%-11s | %8s %8s %8s %8s %8s %9s | %8s %8s %9s %9s\n",
        "pattern", "svc mean", "p50", "p90", "p99", "p99.9", "max",
        "sch mean", "p50", "p99", "max");
    for (int p = 0; p <= PAT__COUNT; p++) {
        struct pat_stats *ps = p < PAT__COUNT ? &per_pattern[p] : &all;

        if (ps -> ops == 0) {
            continue;
        }
        printf("%-11s | %8.1f %8.1f %8.1f %8.1f %8.1f %9.1f | %8.1f %8.1f %9.1f %9.1f\n",
            p < PAT__COUNT ? pat_names[p] : "total",
            us(ps -> svc_sum_ns / ps -> ops),
            us(hist_percentile(ps -> svc, 50, ps -> svc_max_ns)), us(hist_percentile(ps -> svc, 90, ps -> svc_max_ns)),
            us(hist_percentile(ps -> svc, 99, ps -> svc_max_ns)), us(hist_percentile(ps -> svc, 99.9, ps -> svc_max_ns)),
            us(ps -> svc_max_ns),
            us(ps -> sched_sum_ns / ps -> ops),
            us(hist_percentile(ps -> sched, 50, ps -> sched_max_ns)), us(hist_percentile(ps -> sched, 99, ps -> sched_max_ns)),
            us(ps -> sched_max_ns));
    }

    if (errors) {
        printf("\nErrors (MBE status code):\n");
        for (int c = 0; c < clients; c++) {
            for (int i = 0; i < slots[c].nerrors; i++) {
                printf("  client %d, %s: status %d x %llu\n", c, pat_names[slots[c].errors[i].pattern],
                    slots[c].errors[i].code, (unsigned long long) slots[c].errors[i].count);
            }
        }
    }

    if (cfg -> rate > 0) {
        printf("\nRate: target %.1f ops/s, achieved %.1f ops/s (%.1f%%)\n", cfg -> rate, all.ops / wall_s,
            100.0 * all.ops / wall_s / cfg -> rate);
    } else {
        printf("\nRate: unthrottled, achieved %.1f ops/s\n", all.ops / wall_s);
    }

    return errors != 0;
}

int stats_write_csv(const struct perf_cfg *cfg, struct worker_slot *slots, int clients, double wall_s) {
    struct pat_stats per_pattern[PAT__COUNT], all;
    FILE *f;

    if (cfg -> csv[0] == 0) {
        return 0;
    }

    f = fopen(cfg -> csv, "w");
    if (f == NULL) {
        perror(cfg -> csv);
        return 1;
    }

    total_stats(slots, clients, per_pattern, &all);

    fprintf(f, "pattern,clients,rate_target,wall_s,ops,ops_per_s,ok,miss,race,err,late,rows,"
               "svc_mean_us,svc_p50_us,svc_p90_us,svc_p99_us,svc_p999_us,svc_max_us,"
               "sched_mean_us,sched_p50_us,sched_p99_us,sched_max_us\n");
    for (int p = 0; p <= PAT__COUNT; p++) {
        struct pat_stats *ps = p < PAT__COUNT ? &per_pattern[p] : &all;

        if (ps -> ops == 0) {
            continue;
        }
        fprintf(f, "%s,%d,%.1f,%.3f,%llu,%.2f,%llu,%llu,%llu,%llu,%llu,%llu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
            p < PAT__COUNT ? pat_names[p] : "total", clients, cfg -> rate, wall_s,
            (unsigned long long) ps -> ops, ps -> ops / wall_s,
            (unsigned long long) ps -> ok, (unsigned long long) ps -> miss,
            (unsigned long long) ps -> race, (unsigned long long) ps -> err,
            (unsigned long long) ps -> late, (unsigned long long) ps -> rows,
            us(ps -> svc_sum_ns / ps -> ops),
            us(hist_percentile(ps -> svc, 50, ps -> svc_max_ns)), us(hist_percentile(ps -> svc, 90, ps -> svc_max_ns)),
            us(hist_percentile(ps -> svc, 99, ps -> svc_max_ns)), us(hist_percentile(ps -> svc, 99.9, ps -> svc_max_ns)),
            us(ps -> svc_max_ns),
            us(ps -> sched_sum_ns / ps -> ops),
            us(hist_percentile(ps -> sched, 50, ps -> sched_max_ns)), us(hist_percentile(ps -> sched, 99, ps -> sched_max_ns)),
            us(ps -> sched_max_ns));
    }

    fclose(f);

    return 0;
}
