/*
 * The usage patterns of the Enscribe API that the EIR programs follow. Each function does one
 * operation of its pattern, and tells how it ended. The numbers are those of the analysis of
 * the programs (there is no p6):
 *
 *  p1  Keyed lookup without a lock: SETKEY (exact) + READX
 *  p2  Read-lock/update/unlock, or insert if not found: SETKEY + READLOCKX, then
 *      WRITEUPDATEUNLOCKX (UNLOCKREC if it fails) or WRITEX
 *  p3  Generic positioning on a prefix of the key: SETKEY (generic) + READX
 *  p4  Cursor scan: SETKEY (approximate) + READX until the end or the limit
 *  p5  Lookup on the alternate key: SETKEY (key_spec 1, exact) + READX
 *      (optionally the lock/update/unlock cycle as well)
 *  p7  Plain insert of a new record: WRITEX
 *
 * The keys that the operations use are numbered 0 .. rows - 1 for the records loaded by the
 * setup, and the records created by p2 and p7 use numbers above that (a different range for
 * each worker, so they don't collide).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wlib/usrlib.h"

#include "mbeperf.h"

const int pat_ids[PAT__COUNT] = { 1, 2, 3, 4, 5, 7 };
const char *pat_names[PAT__COUNT] = {
    "p1 peek", "p2 upsert", "p3 generic", "p4 scan", "p5 altkey", "p7 append"
};

#define UID_BASE        1000000ULL
#define P2_NEW_BASE     2000000000LL
#define P7_BASE         1000000000LL
#define WORKER_SPAN     10000000LL   /* new records that each worker can create in a range */

/* "IMEI" (14 digits) + "IMSI" (16 digits). Records with consecutive numbers share the IMEI in
 * groups of imsi_per_imei, so that a generic search by IMEI finds more than one record */
void gen_key(char *key, int64_t n, int imsi_per_imei) {
    char buf[REC_KEY_LEN + 1];

    /* long is 32 bits wide on some of the platforms */
    snprintf(buf, sizeof(buf), "%014lld%016lld", 10000000000000LL + n / imsi_per_imei,
        2220100000000000LL + n);
    memcpy(key, buf, REC_KEY_LEN);
}

uint64_t gen_uid(int64_t n) {
    return UID_BASE + n;
}

void build_record(const struct perf_cfg *cfg, char *rec, int64_t n) {
    memset(rec, 0, cfg -> rec_len);
    gen_key(rec + cfg -> key_off, n, cfg -> imsi_per_imei);
    if (cfg -> altkey) {
        uint64_t uid = gen_uid(n);

        memcpy(rec + cfg -> uid_off, &uid, sizeof(uid));
    }
}

/* The counter and the time of the last change go to the end of the record */
static void touch_data(const struct perf_cfg *cfg, char *rec) {
    char *where = rec + cfg -> rec_len - REC_TOUCH_LEN;
    uint32_t counter;
    uint64_t when = (uint64_t) time(NULL);

    memcpy(&counter, where, sizeof(counter));
    counter++;
    memcpy(where, &counter, sizeof(counter));
    memcpy(where + sizeof(counter), &when, sizeof(when));
}

/* The layout has to leave the key, the alternate key and the counter apart, inside the record */
int validate_layout(const struct perf_cfg *cfg) {
    struct { int off, len; const char *name; } part[3];
    int n = 0;

    if (cfg -> rec_len < REC_KEY_LEN + REC_TOUCH_LEN || cfg -> rec_len > MAX_REC_LEN) {
        fprintf(stderr, "--rec-len must be between %d and %d\n", REC_KEY_LEN + REC_TOUCH_LEN, MAX_REC_LEN);
        return -1;
    }

    part[n].off = cfg -> key_off, part[n].len = REC_KEY_LEN, part[n++].name = "key";
    part[n].off = cfg -> rec_len - REC_TOUCH_LEN, part[n].len = REC_TOUCH_LEN, part[n++].name = "counter (end of the record)";
    if (cfg -> altkey) {
        part[n].off = cfg -> uid_off, part[n].len = REC_UID_LEN, part[n++].name = "alternate key";
    }

    for (int i = 0; i < n; i++) {
        if (part[i].off < 0 || part[i].off + part[i].len > cfg -> rec_len) {
            fprintf(stderr, "The %s does not fit in a record of %d bytes\n", part[i].name, cfg -> rec_len);
            return -1;
        }
        for (int j = i + 1; j < n; j++) {
            if (part[i].off < part[j].off + part[j].len && part[j].off < part[i].off + part[i].len) {
                fprintf(stderr, "The %s and the %s overlap in the record layout\n", part[i].name, part[j].name);
                return -1;
            }
        }
    }

    return 0;
}

static int64_t random_row(struct worker *w) {
    return (int64_t) (rng_next(&w -> rng) % (uint64_t) w -> cfg -> rows);
}

static int is_miss(int status) {
    /* End of file (1) is what a positioning that finds nothing leads to, 11 is "not found" */
    return status == 1 || status == 11;
}

static int fail(struct worker *w, int status) {
    w -> last_status = status;

    return OUT_ERR;
}

/* p1 ----------------------------------------------------------------- */

static int pat_peek(struct worker *w) {
    char rec[MAX_REC_LEN];
    unsigned short count;
    char key[REC_KEY_LEN];
    int st;

    gen_key(key, random_row(w), w -> cfg -> imsi_per_imei);

    if ((st = MBE_FILE_SETKEY_(w -> fd, key, REC_KEY_LEN, 0, MBE_POS_EXACT, 0)) != 0) {
        return fail(w, st);
    }

    st = MBE_READX(w -> fd, rec, w -> cfg -> rec_len, &count, -1);
    if (st == 0) {
        w -> rows = 1;
        return OUT_OK;
    }

    return is_miss(st) ? OUT_MISS : fail(w, st);
}

/* p2 ----------------------------------------------------------------- */

static int pat_upsert(struct worker *w) {
    char rec[MAX_REC_LEN];
    unsigned short count;
    char key[REC_KEY_LEN];
    int64_t n;
    int st;

    if ((int) (rng_next(&w -> rng) % 100) < w -> cfg -> p2_new_pct) {
        /* A record that is not in the table: it goes to the insert branch */
        n = P2_NEW_BASE + w -> cfg -> key_offset + w -> id * WORKER_SPAN + w -> p2_seq++;
    } else {
        n = random_row(w);
    }
    gen_key(key, n, w -> cfg -> imsi_per_imei);

    if ((st = MBE_FILE_SETKEY_(w -> fd, key, REC_KEY_LEN, 0, MBE_POS_EXACT, 0)) != 0) {
        return fail(w, st);
    }

    st = MBE_READLOCKX(w -> fd, rec, w -> cfg -> rec_len, &count, -1);
    if (st == 0) {
        w -> rows = 1;
        touch_data(w -> cfg, rec);
        st = MBE_WRITEUPDATEUNLOCKX(w -> fd, rec, w -> cfg -> rec_len, &count, -1);
        if (st != 0) {
            MBE_UNLOCKREC(w -> fd);
            return fail(w, st);
        }
        return OUT_OK;
    }

    if (!is_miss(st)) {
        return fail(w, st);
    }

    /* Not found: insert it */
    build_record(w -> cfg, rec, n);
    touch_data(w -> cfg, rec);
    st = MBE_WRITEX(w -> fd, rec, w -> cfg -> rec_len, &count, -1);
    if (st == 0) {
        return OUT_OK;
    }

    /* Another instance inserted it between the read and the write */
    return st == MBE_S_RECORD_EXISTS ? OUT_RACE : fail(w, st);
}

/* p3 ----------------------------------------------------------------- */

static int pat_generic(struct worker *w) {
    char rec[MAX_REC_LEN];
    unsigned short count;
    char key[REC_KEY_LEN];
    int st;

    gen_key(key, random_row(w), w -> cfg -> imsi_per_imei);

    /* Only the IMEI part of the key goes in */
    if ((st = MBE_FILE_SETKEY_(w -> fd, key, REC_PREFIX_LEN, 0, MBE_POS_GENERIC, 0)) != 0) {
        return fail(w, st);
    }

    st = MBE_READX(w -> fd, rec, w -> cfg -> rec_len, &count, -1);
    if (st == 0) {
        w -> rows = 1;
        return OUT_OK;
    }

    return is_miss(st) ? OUT_MISS : fail(w, st);
}

/* p4 ----------------------------------------------------------------- */

static int pat_scan(struct worker *w) {
    char rec[MAX_REC_LEN];
    unsigned short count;
    char key[REC_KEY_LEN];
    int scan_len = w -> cfg -> scan_len;
    int64_t first;
    int st;

    first = w -> cfg -> rows > scan_len ? (int64_t) (rng_next(&w -> rng) % (uint64_t) (w -> cfg -> rows - scan_len)) : 0;
    gen_key(key, first, w -> cfg -> imsi_per_imei);

    if ((st = MBE_FILE_SETKEY_(w -> fd, key, REC_KEY_LEN, 0, MBE_POS_APPROX, 0)) != 0) {
        return fail(w, st);
    }

    for (int i = 0; i < scan_len; i++) {
        st = MBE_READX(w -> fd, rec, w -> cfg -> rec_len, &count, -1);
        if (st == 0) {
            w -> rows++;
        } else if (is_miss(st)) {
            /* End of the file */
            return OUT_OK;
        } else {
            return fail(w, st);
        }
    }

    return OUT_OK;
}

/* p5 ----------------------------------------------------------------- */

static int pat_altkey(struct worker *w) {
    char rec[MAX_REC_LEN];
    unsigned short count;
    uint64_t uid = gen_uid(random_row(w));
    int mode = w -> cfg -> p5_lock;
    int st;

    if ((st = MBE_FILE_SETKEY_(w -> fd, (char *) &uid, sizeof(uid), w -> cfg -> altkey_spec, MBE_POS_EXACT, 0)) != 0) {
        return fail(w, st);
    }

    if (mode == P5_LOCK_DIRECT) {
        /* As the programs do: the lock is requested with the first read */
        st = MBE_READLOCKX(w -> fd, rec, w -> cfg -> rec_len, &count, -1);
    } else {
        st = MBE_READX(w -> fd, rec, w -> cfg -> rec_len, &count, -1);
    }
    if (st != 0) {
        return is_miss(st) ? OUT_MISS : fail(w, st);
    }
    w -> rows = 1;

    if (mode == P5_LOCK_NONE) {
        return OUT_OK;
    }

    if (mode == P5_LOCK_READFIRST) {
        /* A plain read has been done: now the record is read again, locked */
        st = MBE_READUPDATELOCKX(w -> fd, rec, w -> cfg -> rec_len, &count, -1);
        if (st != 0) {
            return fail(w, st);
        }
    }

    touch_data(w -> cfg, rec);
    st = MBE_WRITEUPDATEUNLOCKX(w -> fd, rec, w -> cfg -> rec_len, &count, -1);
    if (st != 0) {
        MBE_UNLOCKREC(w -> fd);
        return fail(w, st);
    }

    return OUT_OK;
}

/* p7 ----------------------------------------------------------------- */

static int pat_append(struct worker *w) {
    char rec[MAX_REC_LEN];
    unsigned short count;
    int st;

    build_record(w -> cfg, rec, P7_BASE + w -> cfg -> key_offset + w -> id * WORKER_SPAN + w -> p7_seq++);

    st = MBE_WRITEX(w -> fd, rec, w -> cfg -> rec_len, &count, -1);
    if (st == 0) {
        return OUT_OK;
    }

    return st == MBE_S_RECORD_EXISTS ? OUT_RACE : fail(w, st);
}

int run_pattern(struct worker *w, int pattern) {
    w -> rows = 0;
    w -> last_status = 0;

    switch (pattern) {
    case PAT_PEEK:      return pat_peek(w);
    case PAT_UPSERT:    return pat_upsert(w);
    case PAT_GENERIC:   return pat_generic(w);
    case PAT_SCAN:      return pat_scan(w);
    case PAT_ALTKEY:    return pat_altkey(w);
    case PAT_APPEND:    return pat_append(w);
    }

    return fail(w, -1);
}
