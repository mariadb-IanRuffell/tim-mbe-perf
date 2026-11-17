/*
 * mbeperf: performance tool for the MBE (Enscribe) emulator. Shared declarations.
 */

#ifndef __mbeperf_h

#define __mbeperf_h

#include <stdint.h>
#include <time.h>

/* The patterns, in the order they are stored in the stats. The numbers are the pattern
 * identifiers that the user sees (p1 ... p7): pattern 6 is not implemented */
enum perf_pattern {
    PAT_PEEK,       /* p1: exact lookup on the primary key */
    PAT_UPSERT,     /* p2: read-lock, update and unlock, or insert if not found */
    PAT_GENERIC,    /* p3: generic (prefix) positioning */
    PAT_SCAN,       /* p4: approximate positioning and sequential reads */
    PAT_ALTKEY,     /* p5: exact lookup on the alternate key */
    PAT_APPEND,     /* p7: insert of a new record */
    PAT__COUNT
};

extern const int pat_ids[PAT__COUNT];
extern const char *pat_names[PAT__COUNT];

/* The record. It is an opaque array of bytes of rec_len bytes (80 by default), in which the
 * primary key, the alternate key and the counter that p2 and p5 update go to the offsets that
 * the schema has. The default layout is that of the fixtures:
 *
 *      0 ..  7   uid, 8 bytes: alternate key (key_spec 1)
 *      8 .. 37   key, 30 bytes: primary key (key_spec 0), 14 digit "IMEI" + 16 digit "IMSI"
 *     38 .. 79   data: the last 12 bytes have a counter and a time, updated by p2 and p5
 *
 * The integer is the first so that there is no padding whatever the alignment rules */
#define REC_KEY_LEN     30
#define REC_PREFIX_LEN  14  /* The "IMEI" part of the key, used for generic positioning */
#define REC_UID_LEN     8
#define REC_TOUCH_LEN   12  /* counter (4 bytes) + time (8 bytes), at the end of the record */
#define MAX_REC_LEN     4096

enum arrival { ARRIVAL_FIXED, ARRIVAL_POISSON };

enum p5_lock { P5_LOCK_NONE, P5_LOCK_DIRECT, P5_LOCK_READFIRST };

/* What setup does about the table */
enum schema_mode {
    SCHEMA_AUTO,        /* create it if the file is a SQL fixture that can be created, else it exists */
    SCHEMA_CREATE,      /* drop and create it (SQL backends only) */
    SCHEMA_EXISTING     /* the schema is there already: only the MBE API is used */
};

/* Run parameters */
struct perf_cfg {
    char    file[256];
    char    url[256];       /* Overrides the one in the .mbe.ini, if not empty */
    char    config[256];
    char    csv[256];
    int     rows;
    int     rec_len;        /* record layout: see above */
    int     key_off;
    int     uid_off;
    int     altkey;         /* 0: the file has no alternate key (p5 can't run) */
    int     altkey_spec;
    int     schema;
    int     purge;          /* setup: delete all the records first. teardown (existing schema): delete them */
    int     duration;       /* seconds, 0 = use ops */
    int     warmup;         /* seconds */
    int64_t ops;            /* total, over all clients (used when duration is 0) */
    int     clients;
    double  rate;           /* total ops/s, 0 = unthrottled */
    double  mix[PAT__COUNT];/* percentages. Normalized to 100 after parsing */
    int     scan_len;
    int     p2_new_pct;
    int     p5_lock;
    int     imsi_per_imei;
    int     arrival;
    unsigned seed;
    int64_t key_offset;     /* added to the numbers of the records created by p2 and p7 */
    int     setup;
    int     teardown;
    int     progress;
};

/* Results of an operation */
enum outcome {
    OUT_OK,         /* done */
    OUT_MISS,       /* not found/end of file: expected, not an error */
    OUT_RACE,       /* duplicate key on insert: someone else inserted it first */
    OUT_ERR         /* anything else; the status is in worker.last_status */
};

/* Log-linear histogram of nanoseconds: 8 sub-buckets per power of two */
#define HIST_BUCKETS 496

struct pat_stats {
    uint64_t ops, ok, miss, race, err, late, rows;
    uint64_t svc_sum_ns, svc_min_ns, svc_max_ns;
    uint64_t sched_sum_ns, sched_max_ns;
    uint64_t svc[HIST_BUCKETS];     /* service time: from the start of the operation */
    uint64_t sched[HIST_BUCKETS];   /* from the time the operation should have started */
};

#define MAX_ERRORS 16

struct err_entry {
    int code;
    int pattern;
    uint64_t count;
};

/* What a worker leaves in the shared memory for the parent */
struct worker_slot {
    /* Written by the child, read by the parent for the progress line */
    uint64_t progress_ops;
    uint64_t progress_err;
    int ready;
    int done;
    int exit_status;
    uint64_t measured_ns;       /* length of the measured period */
    struct pat_stats stats[PAT__COUNT];
    struct err_entry errors[MAX_ERRORS];
    int nerrors;
};

struct shared_area {
    int stop;                   /* set by the parent (Ctrl-C), checked by the workers */
    int64_t start_ns;           /* when the run starts, in CLOCK_MONOTONIC; 0 = not yet decided */
    struct worker_slot slots[1];/* really cfg.clients of them */
};

/* Per worker state */
struct worker {
    const struct perf_cfg *cfg;
    struct worker_slot *slot;
    int id;
    short fd;
    uint64_t rng;
    long p2_seq;
    long p7_seq;
    int last_status;
    long rows;                  /* records read by the last operation */
    int measuring;
};

/* pace.c */
int64_t now_ns(void);
uint64_t rng_next(uint64_t *state);
double rng_double(uint64_t *state);
void normalize_mix(struct perf_cfg *cfg);
int pick_pattern(const struct perf_cfg *cfg, uint64_t *rng);
void sleep_until(int64_t when_ns);

/* patterns.c */
void gen_key(char *key, int64_t n, int imsi_per_imei);
uint64_t gen_uid(int64_t n);
/* Fills the buffer (cfg->rec_len bytes) with the record number n */
void build_record(const struct perf_cfg *cfg, char *rec, int64_t n);
int validate_layout(const struct perf_cfg *cfg);
int run_pattern(struct worker *w, int pattern);

/* stats.c */
void stats_init(struct worker_slot *slot);
void stats_record(struct worker *w, int pattern, int outcome, int64_t svc_ns, int64_t sched_ns, int late);
uint64_t hist_percentile(const uint64_t *hist, double pct, uint64_t max_ns);
int stats_report(const struct perf_cfg *cfg, struct worker_slot *slots, int clients, double wall_s);
int stats_write_csv(const struct perf_cfg *cfg, struct worker_slot *slots, int clients, double wall_s);

/* setup.c */
int setup_table(const struct perf_cfg *cfg);
int teardown_table(const struct perf_cfg *cfg);

#endif
