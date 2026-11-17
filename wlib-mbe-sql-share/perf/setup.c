/*
 * Preparation of the file that the patterns work on, and cleanup afterwards.
 *
 * There are two cases:
 *
 *  - The file is a fixture of one of the SQL backends of wlib-mbe-sql (PostgreSQL, MySQL) and
 *    the tool is allowed to create it: the table is dropped and created with DDL straight to
 *    the server (the emulator has no way to create tables, and the index on the alternate
 *    key is something the real databases have as well).
 *
 *  - Any other file or database: the schema is there already, and the only thing known about
 *    it is the MBE API. Nothing is created or dropped.
 *
 * In both the records are loaded through the MBE API, as the programs would do. This is done
 * by a child process, so that the parent never opens a file: whatever the implementation does
 * with its connections, the workers that are forked afterwards start from scratch.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "wlib/usrlib.h"

#include "mbeperf.h"

#ifdef MBEPERF_SQL

#include "wlib-internal/cfg.h"

#include "wlib-mbe-sql/mbe-sql.h"
#include "wlib-mbe-sql/sql-be.h"

struct target {
    struct wmbe_sql_be_ifc *ifc;
    int postgres;
    char url[256];
    char table[128];
};

/* Finds out whether the file is a SQL fixture that can be created. It says why not if verbose */
static int get_target(const struct perf_cfg *cfg, struct target *t, int verbose) {
    struct wl_cfg_file *file;
    int found;

    memset(t, 0, sizeof(*t));

    if (wl_cfg_open(cfg -> file, &file) != WL_STATUS_OK) {
        if (verbose) {
            fprintf(stderr, "Cannot read %s as a SQL fixture\n", cfg -> file);
        }
        return 1;
    }

    /* An environment variable (GENERAL__URL, set by --url) takes precedence over the file */
    if (wl_cfg_get_string(file, "general", "url", &found, t -> url, sizeof(t -> url)) != WL_STATUS_OK || !found
        || wl_cfg_get_string(file, "general", "table", &found, t -> table, sizeof(t -> table)) != WL_STATUS_OK || !found) {
        if (verbose) {
            fprintf(stderr, "%s: general/url or general/table is missing\n", cfg -> file);
        }
        wl_cfg_close(file);
        return 1;
    }
    wl_cfg_close(file);

    if (!strncmp(t -> url, "postgres", 8)) {
#ifdef PSQL_ENABLED
        t -> ifc = &wmbe_be_postgres;
        t -> postgres = 1;
#endif
    } else if (!strncmp(t -> url, "mysql", 5) || !strncmp(t -> url, "mariadb", 7)) {
#ifdef MYSQL_ENABLED
        t -> ifc = &wmbe_be_mysql;
#endif
    }

    if (t -> ifc == NULL) {
        if (verbose) {
            fprintf(stderr, "%s: no SQL backend for the url (or it is not built in)\n", cfg -> file);
        }
        return 1;
    }

    return 0;
}

static int exec_sql(struct target *t, struct wmbe_sql_conn *conn, const char *sql) {
    struct wmbe_sql_stmt *stmt = NULL;  /* NULL: the statement is new */
    struct wmbe_sql_cursor *cursor;

    if (t -> ifc -> prepare_stmt(conn, "mbeperf", sql, 0, NULL, &stmt) != MBE_S_OK
        || t -> ifc -> exec_stmt(stmt, -1, NULL, &cursor) != MBE_S_OK) {
        fprintf(stderr, "Failed: %s\n", sql);
        return 1;
    }

    t -> ifc -> free_cursor(cursor);
    t -> ifc -> free_statement(stmt);

    return 0;
}

static int create_table(const struct perf_cfg *cfg, struct target *t) {
    struct wmbe_sql_conn *conn;
    char sql[512];
    int ret = 0;

    if (t -> ifc -> open_connection(t -> url, &conn) != MBE_S_OK) {
        fprintf(stderr, "Cannot connect to the database\n");
        return 1;
    }

    snprintf(sql, sizeof(sql), "DROP TABLE IF EXISTS %s", t -> table);
    ret |= exec_sql(t, conn, sql);

    /* The key is compared as bytes: by range and by prefix (LIKE), and both have to be able to
     * use the index, which is not the case with the default collations */
    snprintf(sql, sizeof(sql), "CREATE TABLE %s ("
        "uid BIGINT NOT NULL, "
        "rkey VARCHAR(%d) %s NOT NULL PRIMARY KEY, "
        "data VARCHAR(%d) NOT NULL DEFAULT '')",
        t -> table, 2 * REC_KEY_LEN,
        t -> postgres ? "COLLATE \"C\"" : "CHARACTER SET latin1 COLLATE latin1_bin",
        2 * (cfg -> rec_len - REC_KEY_LEN - REC_UID_LEN));
    ret |= exec_sql(t, conn, sql);

    snprintf(sql, sizeof(sql), "CREATE UNIQUE INDEX %s_uid ON %s (uid)", t -> table, t -> table);
    ret |= exec_sql(t, conn, sql);

    t -> ifc -> free_connection(conn);

    return ret;
}

static int analyze_table(struct target *t) {
    struct wmbe_sql_conn *conn;
    char sql[256];
    int ret;

    if (t -> ifc -> open_connection(t -> url, &conn) != MBE_S_OK) {
        fprintf(stderr, "Cannot connect to the database\n");
        return 1;
    }

    snprintf(sql, sizeof(sql), t -> postgres ? "ANALYZE %s" : "ANALYZE TABLE %s", t -> table);
    ret = exec_sql(t, conn, sql);
    t -> ifc -> free_connection(conn);

    return ret;
}

static int drop_table(struct target *t) {
    struct wmbe_sql_conn *conn;
    char sql[256];
    int ret;

    if (t -> ifc -> open_connection(t -> url, &conn) != MBE_S_OK) {
        fprintf(stderr, "Cannot connect to the database\n");
        return 1;
    }

    snprintf(sql, sizeof(sql), "DROP TABLE IF EXISTS %s", t -> table);
    ret = exec_sql(t, conn, sql);
    t -> ifc -> free_connection(conn);

    return ret;
}

#endif /* MBEPERF_SQL */

/* Whether the schema is created by this tool. Only the SQL fixtures can be */
static int schema_is_created(const struct perf_cfg *cfg) {
#ifdef MBEPERF_SQL
    struct target t;

    if (cfg -> schema == SCHEMA_EXISTING) {
        return 0;
    }
    if (get_target(cfg, &t, cfg -> schema == SCHEMA_CREATE) == 0) {
        return 1;
    }
#endif

    if (cfg -> schema == SCHEMA_CREATE) {
        fprintf(stderr, "--schema create needs a SQL fixture (and the SQL backends built in)\n");
        exit(1);
    }

    return 0;
}

/* Work that needs the MBE API ----------------------------------------- */

/* Deletes all the records, with a scan: the same that the cleaners do (read, and write-update with
 * length 0 on the record just read) */
static int purge_records(const struct perf_cfg *cfg, short fd) {
    char rec[MAX_REC_LEN], key[REC_KEY_LEN];
    unsigned short count;
    long deleted = 0;
    int st;

    /* A key that sorts first, to start from the beginning of the file */
    memset(key, 0, sizeof(key));
    st = MBE_FILE_SETKEY_(fd, key, REC_KEY_LEN, 0, MBE_POS_APPROX, 0);
    if (st != 0) {
        fprintf(stderr, "Cannot position at the start of the file: error %d\n", st);
        return 1;
    }

    for (;;) {
        st = MBE_READX(fd, rec, cfg -> rec_len, &count, -1);
        if (st == 1) {
            break;      /* end of file */
        }
        if (st != 0) {
            fprintf(stderr, "Failed to read while deleting records: error %d\n", st);
            return 1;
        }

        st = MBE_WRITEUPDATEX(fd, NULL, 0, &count, -1);
        if (st != 0) {
            fprintf(stderr, "Failed to delete a record: error %d\n", st);
            return 1;
        }
        if (++deleted % 20000 == 0) {
            fprintf(stderr, "  deleted %ld records\n", deleted);
        }
    }

    printf("Deleted %ld existing records\n", deleted);

    return 0;
}

static int load_records(const struct perf_cfg *cfg, short fd) {
    char rec[MAX_REC_LEN];
    unsigned short count;
    int present = 0, st;

    for (int i = 0; i < cfg -> rows; i++) {
        build_record(cfg, rec, i);

        st = MBE_WRITEX(fd, rec, cfg -> rec_len, &count, -1);
        if (st == MBE_S_RECORD_EXISTS) {
            /* Only possible when the file was not empty: that record is there already */
            present++;
        } else if (st != 0) {
            fprintf(stderr, "Failed to load record %d: error %d\n", i, st);
            return 1;
        }
        if ((i + 1) % 20000 == 0) {
            fprintf(stderr, "  loaded %d of %d records\n", i + 1, cfg -> rows);
        }
    }

    if (present > 0) {
        printf("%d of the %d records were in the file already\n", present, cfg -> rows);
    }

    return 0;
}

/* Runs in a child: opens the file and purges and/or loads it */
static int load_main(const struct perf_cfg *cfg, int purge, int load) {
    short fd;
    int st, ret = 0;

    st = MBE_FILE_OPEN_(cfg -> file, strlen(cfg -> file), &fd);
    if (st != 0) {
        fprintf(stderr, "Cannot open %s: error %d\n", cfg -> file, st);
        return 1;
    }

    if (purge) {
        ret = purge_records(cfg, fd);
    }
    if (!ret && load) {
        ret = load_records(cfg, fd);
    }

    MBE_FILE_CLOSE_(fd);

    return ret;
}

/* Runs load_main in another process. See the top of the file */
static int run_load(const struct perf_cfg *cfg, int purge, int load) {
    pid_t pid;
    int wstatus;

    fflush(stdout);
    fflush(stderr);

    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        int ret = load_main(cfg, purge, load);

        fflush(stdout);
        _exit(ret);
    }

    if (waitpid(pid, &wstatus, 0) != pid || !WIFEXITED(wstatus)) {
        return 1;
    }

    return WEXITSTATUS(wstatus);
}

/* ---------------------------------------------------------------------- */

int setup_table(const struct perf_cfg *cfg) {
    int ret = 0;

    if (schema_is_created(cfg)) {
#ifdef MBEPERF_SQL
        struct target t;

        get_target(cfg, &t, 1);
        if (cfg -> rec_len != 80 || cfg -> key_off != 8 || cfg -> uid_off != 0 || cfg -> altkey_spec != 1
            || !cfg -> altkey) {
            fprintf(stderr, "The table that mbeperf creates has the default record layout: "
                "use --schema existing for a different one\n");
            return 1;
        }
        printf("Creating table %s and loading %d records...\n", t.table, cfg -> rows);
        ret = create_table(cfg, &t);
        if (!ret) {
            ret = run_load(cfg, 0, 1);
        }
        if (!ret) {
            ret = analyze_table(&t);
        }
#endif
    } else {
        printf("The schema of %s exists already: %s %d records through the MBE API...\n", cfg -> file,
            cfg -> purge ? "deleting the records that it has and loading" : "loading", cfg -> rows);
        ret = run_load(cfg, cfg -> purge, 1);
    }

    return ret;
}

int teardown_table(const struct perf_cfg *cfg) {
    if (schema_is_created(cfg)) {
#ifdef MBEPERF_SQL
        struct target t;

        if (get_target(cfg, &t, 1)) {
            return 1;
        }
        return drop_table(&t);
#endif
    }

    /* There is nothing to drop: the records go, if it was asked for */
    if (cfg -> purge) {
        printf("Deleting the records...\n");
        return run_load(cfg, 1, 0);
    }

    printf("The schema of %s is left as it is (use --purge to delete the records)\n", cfg -> file);

    return 0;
}
