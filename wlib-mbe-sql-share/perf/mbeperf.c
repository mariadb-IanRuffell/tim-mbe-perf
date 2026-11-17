/*
 * mbeperf: performance tool for the MBE (Enscribe) emulator.
 *
 * It runs the usage patterns of the Enscribe API that the EIR programs follow (p1 to p5 and
 * p7, see patterns.c) at a given rate and mix, with one or more clients, and reports the
 * throughput and the latency of each pattern. See README.md.
 *
 * Every client is a process (the emulator is not made for threads) that opens the file and
 * so makes its own connection. The parent prepares the table, starts the clients all at the
 * same time, shows the progress and collects the results, which the clients leave in a
 * memory area shared with it.
 */

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "wlib/usrlib.h"

#include "wlib-internal/cfg.h"

#include "mbeperf.h"

#define MAX_CLIENTS     256
#define LATE_NS         1000000     /* An operation that starts this late is counted as late */
#define START_DELAY_NS  200000000   /* Time between the moment all the clients are ready and the start */

static struct shared_area *shared;
static volatile sig_atomic_t stop_requested;

/* Options --------------------------------------------------------------- */

static void defaults(struct perf_cfg *cfg) {
    memset(cfg, 0, sizeof(*cfg));

#ifdef MBEPERF_SQL
#ifdef PSQL_ENABLED
    strcpy(cfg -> file, "mbeperf-pq.mbe.ini");
#else
    strcpy(cfg -> file, "mbeperf-my.mbe.ini");
#endif
#endif
    /* Without the SQL fixtures there is no default: it is the name of the other implementation's file */
    cfg -> rows = 10000;
    cfg -> rec_len = 80;
    cfg -> key_off = 8;
    cfg -> uid_off = 0;
    cfg -> altkey = 1;
    cfg -> altkey_spec = 1;
    cfg -> schema = SCHEMA_AUTO;
    cfg -> duration = 30;
    cfg -> warmup = 2;
    cfg -> clients = 1;
    cfg -> scan_len = 100;
    cfg -> p2_new_pct = 20;
    cfg -> p5_lock = P5_LOCK_NONE;
    cfg -> imsi_per_imei = 3;
    cfg -> arrival = ARRIVAL_FIXED;
    cfg -> seed = 1;
    cfg -> key_offset = -1;     /* Chosen after the options are read, if it is not set */
    cfg -> setup = 1;
    cfg -> teardown = 0;
    cfg -> progress = 1;
    /* The mix is all zeroes until it is set: normalize_mix() gives the default one */
}

static int parse_bool(const char *s, int *dst) {
    if (!strcmp(s, "1") || !strcasecmp(s, "yes") || !strcasecmp(s, "true") || !strcasecmp(s, "on")) {
        *dst = 1;
    } else if (!strcmp(s, "0") || !strcasecmp(s, "no") || !strcasecmp(s, "false") || !strcasecmp(s, "off")) {
        *dst = 0;
    } else {
        return -1;
    }

    return 0;
}

static int parse_number(const char *s, double *dst) {
    char *end;

    *dst = strtod(s, &end);

    return (end == s || *end != 0) ? -1 : 0;
}

/* "p1", "P1" or "1" */
static int pattern_by_name(const char *name) {
    if (*name == 'p' || *name == 'P') {
        name++;
    }
    for (int i = 0; i < PAT__COUNT; i++) {
        char id[8];

        snprintf(id, sizeof(id), "%d", pat_ids[i]);
        if (!strcmp(name, id)) {
            return i;
        }
    }

    return -1;
}

static int set_mix_entry(struct perf_cfg *cfg, const char *name, const char *value) {
    double pct;
    int p = pattern_by_name(name);

    if (p < 0 || parse_number(value, &pct) || pct < 0) {
        fprintf(stderr, "Invalid mix entry %s=%s\n", name, value);
        return -1;
    }
    cfg -> mix[p] = pct;

    return 0;
}

/* --mix p1=40,p2=20: replaces the whole mix */
static int parse_mix(struct perf_cfg *cfg, const char *text) {
    char *copy = strdup(text), *save = NULL;
    int ret = 0;

    memset(cfg -> mix, 0, sizeof(cfg -> mix));
    for (char *tok = strtok_r(copy, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(tok, '=');

        if (eq == NULL) {
            fprintf(stderr, "Invalid mix entry '%s': expected pN=percent\n", tok);
            ret = -1;
            break;
        }
        *eq = 0;
        if (set_mix_entry(cfg, tok, eq + 1)) {
            ret = -1;
            break;
        }
    }
    free(copy);

    return ret;
}

/* Sets an option, whether it comes from the command line or from the [run] section of
 * the config file. Returns -1 if the name is unknown or the value is not valid */
static int apply_option(struct perf_cfg *cfg, const char *name, const char *value) {
    double num;
    int flag;

#define STR(opt, field) \
    if (!strcmp(name, opt)) { snprintf(cfg -> field, sizeof(cfg -> field), "%s", value); return 0; }
#define NUM(opt, field, type, min) \
    if (!strcmp(name, opt)) { if (parse_number(value, &num) || num < (min)) { goto invalid; } cfg -> field = (type) num; return 0; }
#define FLAG(opt, field) \
    if (!strcmp(name, opt)) { if (parse_bool(value, &flag)) { goto invalid; } cfg -> field = flag; return 0; }

    STR("file", file)
    STR("url", url)
    STR("csv", csv)
    NUM("rows", rows, int, 1)
    NUM("rec-len", rec_len, int, 1)
    NUM("rec-key-off", key_off, int, 0)
    NUM("rec-uid-off", uid_off, int, 0)
    NUM("altkey-spec", altkey_spec, int, 1)
    FLAG("altkey", altkey)
    FLAG("purge", purge)
    NUM("duration", duration, int, 0)
    NUM("warmup", warmup, int, 0)
    NUM("ops", ops, int64_t, 0)
    NUM("clients", clients, int, 1)
    NUM("rate", rate, double, 0)
    NUM("scan-len", scan_len, int, 1)
    NUM("p2-new-pct", p2_new_pct, int, 0)
    NUM("imsi-per-imei", imsi_per_imei, int, 1)
    NUM("seed", seed, unsigned, 0)
    NUM("key-offset", key_offset, int64_t, 0)
    FLAG("setup", setup)
    FLAG("teardown", teardown)
    FLAG("progress", progress)

    if (!strcmp(name, "mix")) {
        return parse_mix(cfg, value);
    }
    if (!strcmp(name, "p5-lock")) {
        if (!strcmp(value, "none") || !strcmp(value, "0")) {
            cfg -> p5_lock = P5_LOCK_NONE;
        } else if (!strcmp(value, "direct") || !strcmp(value, "1")) {
            cfg -> p5_lock = P5_LOCK_DIRECT;
        } else if (!strcmp(value, "readfirst")) {
            cfg -> p5_lock = P5_LOCK_READFIRST;
        } else {
            goto invalid;
        }
        return 0;
    }
    if (!strcmp(name, "schema")) {
        if (!strcmp(value, "auto")) {
            cfg -> schema = SCHEMA_AUTO;
        } else if (!strcmp(value, "create")) {
            cfg -> schema = SCHEMA_CREATE;
        } else if (!strcmp(value, "existing")) {
            cfg -> schema = SCHEMA_EXISTING;
        } else {
            goto invalid;
        }
        return 0;
    }
    if (!strcmp(name, "arrival")) {
        if (!strcmp(value, "fixed")) {
            cfg -> arrival = ARRIVAL_FIXED;
        } else if (!strcmp(value, "poisson")) {
            cfg -> arrival = ARRIVAL_POISSON;
        } else {
            goto invalid;
        }
        return 0;
    }

    fprintf(stderr, "Unknown option '%s'\n", name);
    return -1;

invalid:
    fprintf(stderr, "Invalid value '%s' for option '%s'\n", value, name);
    return -1;
}

/* The [run] and [mix] sections of the config file */
static int load_config(struct perf_cfg *cfg, const char *fname) {
    static const char *names[] = {
        "file", "url", "csv", "rows", "duration", "warmup", "ops", "clients", "rate", "scan-len",
        "p2-new-pct", "imsi-per-imei", "seed", "key-offset", "setup", "teardown", "progress",
        "p5-lock", "arrival", "mix", "rec-len", "rec-key-off", "rec-uid-off", "altkey", "altkey-spec", "schema",
        "purge", NULL
    };
    struct wl_cfg_file *file;
    char value[256];
    int found;

    if (wl_cfg_open(fname, &file) != WL_STATUS_OK) {
        fprintf(stderr, "Cannot open the config file %s\n", fname);
        return -1;
    }

    for (const char **n = names; *n != NULL; n++) {
        if (wl_cfg_get_string(file, "run", *n, &found, value, sizeof(value)) == WL_STATUS_OK && found) {
            if (apply_option(cfg, *n, value)) {
                wl_cfg_close(file);
                return -1;
            }
        }
    }

    for (int i = 0; i < PAT__COUNT; i++) {
        char key[8];

        snprintf(key, sizeof(key), "p%d", pat_ids[i]);
        if (wl_cfg_get_string(file, "mix", key, &found, value, sizeof(value)) == WL_STATUS_OK && found) {
            if (set_mix_entry(cfg, key, value)) {
                wl_cfg_close(file);
                return -1;
            }
        }
    }

    wl_cfg_close(file);

    return 0;
}

static void usage(const char *prog) {
    printf(
"Usage: %s [options]\n"
"\n"
"Runs the Enscribe usage patterns of the EIR programs against the MBE emulator.\n"
"Options can also be set in a config file ([run] and [mix] sections, same names);\n"
"the command line has precedence.\n"
"\n"
"  -f, --file FILE        .mbe.ini of the table (mbeperf-pq.mbe.ini)\n"
"      --url URL          database URL, instead of the one in the file\n"
"  -c, --config FILE      config file\n"
"  -r, --rows N           records loaded by the setup (10000)\n"
"  -d, --duration S       seconds to measure (30). 0: stop after --ops operations\n"
"  -n, --ops N            total operations, if --duration is 0\n"
"  -w, --warmup S         seconds of warm-up, not measured (2)\n"
"  -j, --clients N        client processes (1)\n"
"  -R, --rate R           total operations per second, 0: as fast as possible (0)\n"
"  -m, --mix LIST         percentage of each pattern, e.g. p1=40,p2=20,p3=5,p4=5,p5=20,p7=10\n"
"                         (relative: they are scaled to add up to 100)\n"
"      --scan-len N       records read by p4 (100)\n"
"      --p2-new-pct N     %% of p2 operations on a record that does not exist (20)\n"
"      --p5-lock MODE     none, direct (READLOCKX, as the programs do) or readfirst (default none)\n"
"      --imsi-per-imei N  records that share the prefix p3 looks for (3)\n"
"      --arrival A        fixed or poisson spacing of the operations (fixed)\n"
"      --seed N           random seed (1)\n"
"      --key-offset N     numbering of the records created by p2 and p7 (default: from the clock, so\n"
"                         that they are new in every run, also with --no-setup)\n"
"      --csv FILE         write the results in CSV format\n"
"\n"
"Other databases (anything that opens through MBE_FILE_OPEN_):\n"
"      --schema MODE      auto (default), create or existing. The tool creates the table only for\n"
"                         the SQL fixtures (auto: if it can). With existing, the schema must be there:\n"
"                         setup only loads the records, through the MBE API\n"
"      --purge            setup: delete all the records of an existing schema before loading them.\n"
"                         teardown: delete them at the end. Never done unless asked for\n"
"      --rec-len N        record length (80), --rec-key-off N: offset of the 30 byte primary key (8),\n"
"      --rec-uid-off N    offset of the 8 byte alternate key (0), --altkey-spec N: its key specifier (1)\n"
"      --no-altkey        the file has no alternate key (p5 is not possible)\n"
"      --setup, --no-setup        create and load the table (default), or use the existing one\n"
"      --teardown, --no-teardown  drop the table at the end (default: leave it)\n"
"      --no-progress      do not show the progress line\n"
"  -h, --help\n",
        prog);
}

enum { OPT_URL = 256, OPT_SCAN_LEN, OPT_P2_NEW, OPT_P5_LOCK, OPT_IMSI, OPT_ARRIVAL, OPT_SEED, OPT_KEY_OFFSET,
    OPT_CSV, OPT_SETUP, OPT_NO_SETUP, OPT_TEARDOWN, OPT_NO_TEARDOWN, OPT_NO_PROGRESS,
    OPT_SCHEMA, OPT_PURGE, OPT_REC_LEN, OPT_KEY_OFF, OPT_UID_OFF, OPT_ALTKEY_SPEC, OPT_NO_ALTKEY };

static const struct option long_options[] = {
    { "file", required_argument, NULL, 'f' },
    { "url", required_argument, NULL, OPT_URL },
    { "config", required_argument, NULL, 'c' },
    { "rows", required_argument, NULL, 'r' },
    { "duration", required_argument, NULL, 'd' },
    { "ops", required_argument, NULL, 'n' },
    { "warmup", required_argument, NULL, 'w' },
    { "clients", required_argument, NULL, 'j' },
    { "rate", required_argument, NULL, 'R' },
    { "mix", required_argument, NULL, 'm' },
    { "scan-len", required_argument, NULL, OPT_SCAN_LEN },
    { "p2-new-pct", required_argument, NULL, OPT_P2_NEW },
    { "p5-lock", required_argument, NULL, OPT_P5_LOCK },
    { "imsi-per-imei", required_argument, NULL, OPT_IMSI },
    { "arrival", required_argument, NULL, OPT_ARRIVAL },
    { "seed", required_argument, NULL, OPT_SEED },
    { "key-offset", required_argument, NULL, OPT_KEY_OFFSET },
    { "csv", required_argument, NULL, OPT_CSV },
    { "setup", no_argument, NULL, OPT_SETUP },
    { "no-setup", no_argument, NULL, OPT_NO_SETUP },
    { "teardown", no_argument, NULL, OPT_TEARDOWN },
    { "no-teardown", no_argument, NULL, OPT_NO_TEARDOWN },
    { "no-progress", no_argument, NULL, OPT_NO_PROGRESS },
    { "schema", required_argument, NULL, OPT_SCHEMA },
    { "purge", no_argument, NULL, OPT_PURGE },
    { "rec-len", required_argument, NULL, OPT_REC_LEN },
    { "rec-key-off", required_argument, NULL, OPT_KEY_OFF },
    { "rec-uid-off", required_argument, NULL, OPT_UID_OFF },
    { "altkey-spec", required_argument, NULL, OPT_ALTKEY_SPEC },
    { "no-altkey", no_argument, NULL, OPT_NO_ALTKEY },
    { "help", no_argument, NULL, 'h' },
    { NULL, 0, NULL, 0 }
};

static const char *short_options = "f:c:r:d:n:w:j:R:m:h";

/* The name that apply_option() knows for the option that getopt returned */
static const char *option_name(int opt) {
    switch (opt) {
    case 'f': return "file";
    case 'r': return "rows";
    case 'd': return "duration";
    case 'n': return "ops";
    case 'w': return "warmup";
    case 'j': return "clients";
    case 'R': return "rate";
    case 'm': return "mix";
    case OPT_URL: return "url";
    case OPT_SCAN_LEN: return "scan-len";
    case OPT_P2_NEW: return "p2-new-pct";
    case OPT_P5_LOCK: return "p5-lock";
    case OPT_IMSI: return "imsi-per-imei";
    case OPT_ARRIVAL: return "arrival";
    case OPT_SEED: return "seed";
    case OPT_KEY_OFFSET: return "key-offset";
    case OPT_CSV: return "csv";
    case OPT_SCHEMA: return "schema";
    case OPT_REC_LEN: return "rec-len";
    case OPT_KEY_OFF: return "rec-key-off";
    case OPT_UID_OFF: return "rec-uid-off";
    case OPT_ALTKEY_SPEC: return "altkey-spec";
    }

    return NULL;
}

static int parse_args(struct perf_cfg *cfg, int argc, char **argv) {
    const char *config = NULL;
    int opt;

    /* The config file is loaded first, so that the command line wins */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config")) {
            if (i + 1 < argc) {
                config = argv[i + 1];
            }
        } else if (!strncmp(argv[i], "--config=", 9)) {
            config = argv[i] + 9;
        }
    }
    if (config != NULL) {
        snprintf(cfg -> config, sizeof(cfg -> config), "%s", config);
        if (load_config(cfg, config)) {
            return -1;
        }
    }

    optind = 1;
    while ((opt = getopt_long(argc, argv, short_options, long_options, NULL)) != -1) {
        switch (opt) {
        case 'h':
            usage(argv[0]);
            exit(0);
        case 'c':
            break;
        case OPT_SETUP:         cfg -> setup = 1; break;
        case OPT_NO_SETUP:      cfg -> setup = 0; break;
        case OPT_TEARDOWN:      cfg -> teardown = 1; break;
        case OPT_NO_TEARDOWN:   cfg -> teardown = 0; break;
        case OPT_NO_PROGRESS:   cfg -> progress = 0; break;
        case OPT_PURGE:         cfg -> purge = 1; break;
        case OPT_NO_ALTKEY:     cfg -> altkey = 0; break;
        case '?':
            return -1;
        default:
            if (apply_option(cfg, option_name(opt), optarg)) {
                return -1;
            }
        }
    }

    if (optind < argc) {
        fprintf(stderr, "Unexpected argument '%s'\n", argv[optind]);
        return -1;
    }

    if (cfg -> file[0] == 0) {
        fprintf(stderr, "--file is required\n");
        return -1;
    }
    if (cfg -> clients > MAX_CLIENTS) {
        fprintf(stderr, "At most %d clients\n", MAX_CLIENTS);
        return -1;
    }
    if (cfg -> duration == 0 && cfg -> ops == 0) {
        fprintf(stderr, "Either --duration or --ops must be set\n");
        return -1;
    }
    if (cfg -> p2_new_pct > 100) {
        fprintf(stderr, "--p2-new-pct is a percentage\n");
        return -1;
    }

    if (cfg -> key_offset < 0) {
        /* The records created by p2 and p7 must not be there from a previous run on the same table
         * (--no-setup): each second has its own range */
        cfg -> key_offset = (int64_t) (time(NULL) % 10000) * 100000;
    }

    normalize_mix(cfg);

    if (validate_layout(cfg)) {
        return -1;
    }
    if (!cfg -> altkey && cfg -> mix[PAT_ALTKEY] > 0) {
        fprintf(stderr, "p5 needs an alternate key: give it a percentage of 0 in the mix, or drop --no-altkey\n");
        return -1;
    }

    return 0;
}

/* Workers --------------------------------------------------------------- */

static void on_signal(int sig) {
    stop_requested = 1;
    if (shared != NULL) {
        __atomic_store_n(&shared -> stop, 1, __ATOMIC_RELAXED);
    }
}

static int stopping(void) {
    return stop_requested || __atomic_load_n(&shared -> stop, __ATOMIC_RELAXED);
}

/* The time to wait for the next operation of a client */
static int64_t next_interval(const struct perf_cfg *cfg, struct worker *w, int64_t interval_ns) {
    if (cfg -> arrival == ARRIVAL_POISSON) {
        return (int64_t) (-log(1.0 - rng_double(&w -> rng)) * interval_ns);
    }

    return interval_ns;
}

static void worker_main(const struct perf_cfg *cfg, int id) {
    struct worker w;
    struct worker_slot *slot = &shared -> slots[id];
    int64_t start, warm_end, end, next, interval_ns = 0, last = 0;
    int64_t ops_left = 0, measured_ops = 0;
    int st;

    stats_init(slot);
    memset(&w, 0, sizeof(w));
    w.cfg = cfg;
    w.slot = slot;
    w.id = id;
    w.rng = (cfg -> seed + 1) * 0x9E3779B97F4A7C15ULL + id * 0xBF58476D1CE4E5B9ULL + 1;
    for (int i = 0; i < 8; i++) {
        rng_next(&w.rng);
    }

    st = MBE_FILE_OPEN_(cfg -> file, strlen(cfg -> file), &w.fd);
    if (st != 0) {
        fprintf(stderr, "client %d: cannot open %s: error %d\n", id, cfg -> file, st);
        slot -> exit_status = 2;
        __atomic_store_n(&slot -> done, 1, __ATOMIC_RELEASE);
        _exit(2);
    }

    __atomic_store_n(&slot -> ready, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&shared -> start_ns, __ATOMIC_ACQUIRE) == 0 && !stopping()) {
        usleep(1000);
    }

    start = __atomic_load_n(&shared -> start_ns, __ATOMIC_ACQUIRE);
    warm_end = start + (int64_t) cfg -> warmup * 1000000000LL;
    end = cfg -> duration > 0 ? warm_end + (int64_t) cfg -> duration * 1000000000LL : INT64_MAX;
    if (cfg -> duration == 0) {
        ops_left = cfg -> ops / cfg -> clients + (id < cfg -> ops % cfg -> clients ? 1 : 0);
    }
    if (cfg -> rate > 0) {
        interval_ns = (int64_t) (cfg -> clients * 1e9 / cfg -> rate);
    }
    /* The clients are spread over the period, so that they don't all fire together */
    next = start + id * interval_ns / cfg -> clients;

    while (!stopping()) {
        int64_t due, begin, done;
        int pattern, outcome, late = 0;

        if (interval_ns > 0) {
            due = next;
            next += next_interval(cfg, &w, interval_ns);
            if (now_ns() < due) {
                sleep_until(due);
            }
        } else {
            due = now_ns();
        }
        if (stopping()) {
            break;
        }

        begin = now_ns();
        /* If the system can't keep up, the schedule is far ahead of the clock: the run ends on
         * time anyway, and what it didn't get to do shows in the rate that was achieved */
        if (due >= end || begin >= end) {
            break;
        }
        if (interval_ns > 0 && begin - due > LATE_NS) {
            late = 1;
        }

        pattern = pick_pattern(cfg, &w.rng);
        outcome = run_pattern(&w, pattern);
        done = now_ns();
        last = done;

        __atomic_fetch_add(&slot -> progress_ops, 1, __ATOMIC_RELAXED);
        if (outcome == OUT_ERR) {
            __atomic_fetch_add(&slot -> progress_err, 1, __ATOMIC_RELAXED);
        }

        /* Operations that were due during the warm-up are not measured */
        if (due >= warm_end) {
            stats_record(&w, pattern, outcome, done - begin, done - due, late);
            if (cfg -> duration == 0 && ++measured_ops >= ops_left) {
                break;
            }
        }
    }

    slot -> measured_ns = last > warm_end ? (uint64_t) (last - warm_end) : 0;
    MBE_FILE_CLOSE_(w.fd);
    __atomic_store_n(&slot -> done, 1, __ATOMIC_RELEASE);

    /* No exit(): the library has nothing to say now, and it could take its time */
    _exit(0);
}

/* The parent ------------------------------------------------------------ */

static void print_config(const struct perf_cfg *cfg) {
    printf("mbeperf: file=%s rows=%d clients=%d", cfg -> file, cfg -> rows, cfg -> clients);
    if (cfg -> rate > 0) {
        printf(" rate=%.1f/s (%s)", cfg -> rate, cfg -> arrival == ARRIVAL_POISSON ? "poisson" : "fixed");
    } else {
        printf(" rate=unthrottled");
    }
    if (cfg -> duration > 0) {
        printf(" duration=%ds", cfg -> duration);
    } else {
        printf(" ops=%lld", (long long) cfg -> ops);
    }
    printf(" warmup=%ds\nmix:", cfg -> warmup);
    for (int i = 0; i < PAT__COUNT; i++) {
        if (cfg -> mix[i] > 0) {
            printf(" p%d=%.1f%%", pat_ids[i], cfg -> mix[i]);
        }
    }
    printf("  (scan-len=%d p2-new-pct=%d p5-lock=%s imsi-per-imei=%d seed=%u key-offset=%lld)\n",
        cfg -> scan_len, cfg -> p2_new_pct,
        cfg -> p5_lock == P5_LOCK_NONE ? "none" : cfg -> p5_lock == P5_LOCK_DIRECT ? "direct" : "readfirst",
        cfg -> imsi_per_imei, cfg -> seed, (long long) cfg -> key_offset);
}

static uint64_t total_progress(int clients, int errors) {
    uint64_t sum = 0;

    for (int c = 0; c < clients; c++) {
        sum += __atomic_load_n(errors ? &shared -> slots[c].progress_err : &shared -> slots[c].progress_ops,
            __ATOMIC_RELAXED);
    }

    return sum;
}

int main(int argc, char **argv) {
    struct perf_cfg cfg;
    struct sigaction sa;
    pid_t *pids;
    size_t size;
    int64_t t0, started = 0;
    uint64_t prev_ops = 0;
    int running, failed = 0, result = 0, wstatus = 0;

    defaults(&cfg);
    if (parse_args(&cfg, argc, argv)) {
        usage(argv[0]);
        return 2;
    }

    /* The URL goes to the environment: the configuration reader looks there first, and
     * that way it applies both to the setup and to the emulator itself */
    if (cfg.url[0] != 0) {
        setenv("GENERAL__URL", cfg.url, 1);
    }

    print_config(&cfg);
    fflush(stdout);

    if (cfg.setup && setup_table(&cfg)) {
        return 1;
    }

    size = offsetof(struct shared_area, slots) + cfg.clients * sizeof(struct worker_slot);
    shared = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    memset(shared, 0, size);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);

    pids = calloc(cfg.clients, sizeof(*pids));
    for (int c = 0; c < cfg.clients; c++) {
        pids[c] = fork();
        if (pids[c] < 0) {
            perror("fork");
            __atomic_store_n(&shared -> stop, 1, __ATOMIC_RELAXED);
            failed = 1;
            break;
        }
        if (pids[c] == 0) {
            worker_main(&cfg, c);
        }
    }

    /* Wait until all of them have opened the file, and start them together */
    for (;;) {
        int ready = 0, gone = 0;

        for (int c = 0; c < cfg.clients; c++) {
            ready += __atomic_load_n(&shared -> slots[c].ready, __ATOMIC_ACQUIRE);
            gone += pids[c] > 0 && __atomic_load_n(&shared -> slots[c].done, __ATOMIC_ACQUIRE);
        }
        if (failed || gone > 0 || stop_requested) {
            __atomic_store_n(&shared -> stop, 1, __ATOMIC_RELAXED);
            failed = 1;
            break;
        }
        if (ready == cfg.clients) {
            break;
        }
        usleep(5000);
    }
    if (!failed) {
        started = now_ns() + START_DELAY_NS;
        __atomic_store_n(&shared -> start_ns, started, __ATOMIC_RELEASE);
        t0 = started + (int64_t) cfg.warmup * 1000000000LL;
        printf("\nRunning%s...\n", cfg.warmup ? " (warm-up first)" : "");
    } else {
        t0 = 0;
    }

    /* Progress, until the clients are done */
    do {
        int64_t now;

        running = 0;
        for (int c = 0; c < cfg.clients; c++) {
            if (pids[c] > 0 && waitpid(pids[c], &wstatus, WNOHANG) == pids[c]) {
                pids[c] = -1;
                if ((WIFEXITED(wstatus) && WEXITSTATUS(wstatus) != 0) || WIFSIGNALED(wstatus)) {
                    failed = 1;
                }
            }
            running += pids[c] > 0;
        }
        if (running == 0) {
            break;
        }

        if (started) {
            usleep(1000000);
            if (cfg.progress) {
                uint64_t ops = total_progress(cfg.clients, 0);

                now = now_ns();
                fprintf(stderr, "\r[%5.0fs%s] %llu ops, %llu ops/s, %llu errors   ",
                    (now - started) / 1e9, now < t0 ? " warm-up" : "", (unsigned long long) ops,
                    (unsigned long long) (ops - prev_ops), (unsigned long long) total_progress(cfg.clients, 1));
                prev_ops = ops;
            }
        } else {
            usleep(10000);
        }
    } while (running);
    if (cfg.progress && started) {
        fprintf(stderr, "\n");
    }

    if (failed) {
        fprintf(stderr, "Some clients failed or the run was interrupted: results are partial\n");
    }

    {
        uint64_t max_ns = 0;
        double wall;

        for (int c = 0; c < cfg.clients; c++) {
            if (shared -> slots[c].measured_ns > max_ns) {
                max_ns = shared -> slots[c].measured_ns;
            }
        }
        wall = max_ns > 0 ? max_ns / 1e9 : (cfg.duration > 0 ? cfg.duration : 1);

        if (started) {
            result = stats_report(&cfg, shared -> slots, cfg.clients, wall);
            stats_write_csv(&cfg, shared -> slots, cfg.clients, wall);
        }
    }

    if (cfg.teardown) {
        teardown_table(&cfg);
    }

    /* As the clients, leave without the library's exit handlers: they have nothing to clean here */
    fflush(stdout);
    _exit(failed || result ? 1 : 0);
}
