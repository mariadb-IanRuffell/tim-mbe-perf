/*
 * MBE File descriptor and connection management.
 * SQL connections are both shared and cached:
 *  - Sharing a connection means that many active mbe_file's can use the same connection
 *      to access the DB. This means that each one will compile their own prepared
 *      statements because they will manipulate different tables.
 *  - Caching a file means that, when a file is closed, its resources are not released,
 *      but the descriptor is made available for the next OPEN. This is done because
 *      some applications open and close the same file frequently.
 */

#include <stdlib.h>
#include <string.h>

#define L_FEATURE L_FEAT_DATA

#include "wlib-internal/log.h"
#include "wlib-internal/cfg.h"
#include "wlib-internal/status.h"

#include "wlib/usrlib.h"

#include "wlib-mbe-sql/mbe-sql.h"
#include "wlib-mbe-sql/sql-be.h"

#define CONN_CACHE_INITIAL_SIZE 8
#define MAX_DESCRIPTORS 256

static void close_file(struct wmbe_sql_file *file, int is_conn_shared);

/* The connection cache: one entry per distinct URL, it grows as needed */
static struct conn_cache_entry {

    char *url;
    struct wmbe_sql_conn *conn;

} *conn_cache;
static int conn_cache_count;
static int conn_cache_size;

/* The descriptor array */
static int mfdt_watermark;
static struct wmbe_sql_file mfdt[MAX_DESCRIPTORS];

static struct wmbe_sql_conn *find_cached_connection(const char *url) {
    for (int i = 0; i < conn_cache_count; i++) {
        if (!strcmp(url, conn_cache[i].url)) {
            return conn_cache[i].conn;
        }
    }

    return NULL;
}

static enum wl_status add_cached_connection(const char *url, struct wmbe_sql_conn *conn) {
    char *new_url;

    if (conn_cache_count == conn_cache_size) {
        int new_size = conn_cache_size ? 2 * conn_cache_size : CONN_CACHE_INITIAL_SIZE;
        struct conn_cache_entry *new_cache = realloc(conn_cache, new_size * sizeof(*new_cache));

        if (new_cache == NULL) {
            return WL_STATUS_OUT_OF_MEMORY;
        }
        conn_cache = new_cache;
        conn_cache_size = new_size;
    }

    new_url = strdup(url);
    if (new_url == NULL) {
        return WL_STATUS_OUT_OF_MEMORY;
    }

    conn_cache[conn_cache_count].url = new_url;
    conn_cache[conn_cache_count].conn = conn;
    conn_cache_count++;

    return WL_STATUS_OK;
}

static enum wl_status open_connection(struct wmbe_sql_file *fdes, int *from_cache) {
    struct wmbe_sql_conn *conn;

    conn = find_cached_connection(fdes -> url);
    if (conn == NULL) {
        enum mbe_status res;

        res = fdes -> be -> open_connection(fdes -> url, &conn);
        if (res != MBE_S_OK) {
            return WL_STATUS_TRANSPORT_FAILED;
        }

        *from_cache = 0;
    } else {
        *from_cache = 1;
    }
    
    fdes -> conn = conn;

    return WL_STATUS_OK;
}

static enum wl_status prepare_statements(struct wmbe_sql_file *fdes) {
    enum mbe_status status;

    status = wlib_mbe_sql_prepare_file_stmts(fdes);
    if (status != MBE_S_OK) {
        return WL_STATUS_TRANSPORT_FAILED;
    }

    for (int i = 0; i < fdes -> field_number; i++) {
        if (fdes -> keys[i].field != NULL) {
            status = wlib_mbe_sql_prepare_key_stmts(fdes, &fdes -> keys[i]);
            if (status != MBE_S_OK) {
                return WL_STATUS_TRANSPORT_FAILED;
            }
        }
    }

    return WL_STATUS_OK;
}

/* Prepares the statements of a file again, clearing the stale flag on success */
static enum wl_status reprepare(struct wmbe_sql_file *fdes) {
    enum wl_status status = prepare_statements(fdes);

    if (status == WL_STATUS_OK) {
        fdes -> stmts_stale = 0;
    }

    return status;
}

static struct wmbe_sql_file *get_cached_descriptor(const char *fname) {
    l_debug("Looking for cached fdescriptor");
    for (int fdes = 0; fdes < mfdt_watermark; fdes++) {
        if (mfdt[fdes].state == WMBE_F_VALID
             && !strcmp(fname, mfdt[fdes].fname)) {
            return &mfdt[fdes];
        }
    }

    return NULL;
}

static struct wmbe_sql_file *get_free_descriptor(void) {
    l_debug("Looking for free fdescriptor");
    for (int fdes = 0; fdes < MAX_DESCRIPTORS; fdes++) {
        if (mfdt[fdes].state == WMBE_F_FREE) {
            if (fdes >= mfdt_watermark) {
                mfdt_watermark = fdes + 1;
            }
            return &mfdt[fdes];
        }
    }

    return NULL;
}

static struct wmbe_sql_be_ifc *find_ifc(const char *url) {
#ifdef PSQL_ENABLED
    if (!strncasecmp(url, "postgresql:", strlen("postgresql:"))) {
        return &wmbe_be_postgres;
    }
#endif
#ifdef MYSQL_ENABLED
    if (!strncasecmp(url, "mysql:", strlen("mysql:"))) {
        return &wmbe_be_mysql;
    }
#endif

    return NULL;
}

enum wl_status wlib_mbe_sql_fdes_open(const char *fname,
        short access, int exclusion, struct wmbe_sql_file **fdesp) {
    struct wmbe_sql_file *fdes;
    struct wmbe_sql_be_ifc *be;
    struct wl_cfg_file *cfg;
    enum wl_status res;
    int from_cache = 0;

    l_debug("Opening file descriptor");

    if (strlen(fname) >= sizeof(fdes -> fname)) {
        /* It is copied into the descriptor, right before the table name */
        l_error("Failed to open <%s>: name too long (max %d characters)", fname, (int) sizeof(fdes -> fname) - 1);
        return WL_STATUS_TOO_LARGE;
    }

    fdes = get_cached_descriptor(fname);                                
    if (fdes != NULL) {
        if (fdes -> stmts_stale) {
            /* Best effort: if it fails, it stays stale and is fixed when it's used */
            reprepare(fdes);
        }
        fdes -> state = WMBE_F_USED;
        *fdesp = fdes;
        return WL_STATUS_OK;
    }

    fdes = get_free_descriptor();
    if (fdes == NULL) {
        l_error("Failed to open <%s>: no fdescriptors available", fname);
        return WL_STATUS_OUT_OF_MEMORY;
    }

    res = wl_cfg_open(fname, &cfg);
    if (res != WL_STATUS_OK) {
        l_error("Failed to open <%s>, code: %d", fname, res);
        return res;
    }

    memset(fdes, 0, sizeof(struct wmbe_sql_file));
    res = wlib_mbe_cfg_parse_cfg_file(fdes, cfg);
    wl_cfg_close(cfg);
    if (res != WL_STATUS_OK) {
        l_error("Failed to parse <%s>, bail out", fname);
        close_file(fdes, 0);
        return res;
    } 

    if (fdes -> keys[0].field == NULL) {
        l_error("No primary key specified. You must set key_spec=0 for one field");
        close_file(fdes, 0);
        return WL_STATUS_PARSE_ERROR;
    }

    be = find_ifc(fdes -> url);
    if (be == NULL) {
        /* Only the scheme: the rest of the URL may hold a password */
        const char *sep = strstr(fdes -> url, "://");

        l_error("Unsupported database driver for url (scheme <%.*s>)",
            sep != NULL ? (int) (sep - fdes -> url) : 0, fdes -> url);
        close_file(fdes, 0);
        return WL_STATUS_PARSE_ERROR;
    }

    fdes -> be = be;
    strcpy(fdes -> fname, fname);
    fdes -> state = WMBE_F_USED;
    fdes -> access = access;
    fdes -> exclusion = exclusion;

    l_debug("Configuration parsed successfully. Now connecting to the DB");

    res = open_connection(fdes, &from_cache);
    if (res != WL_STATUS_OK) {
        l_error("Failed to open the database");
        close_file(fdes, from_cache);
        return res;
    }

    l_debug("Preparing all the statements");

    res = prepare_statements(fdes);
    if (res != WL_STATUS_OK) {
        l_error("Failed to prepare statements");
        close_file(fdes, from_cache);
        return res;
    }

    /* The statements are prepared when they are used, but a table or a column that does not
     * exist must fail the open, as the file does in Enscribe. The insert has all the columns, and the
     * rest of the statements only use those */
    if (fdes -> be -> prepare_now != NULL
        && fdes -> be -> prepare_now(fdes -> stmts.insert_stmt) != MBE_S_OK) {

        l_error("The statements of %s are not valid", fdes -> table);
        close_file(fdes, from_cache);
        return WL_STATUS_TRANSPORT_FAILED;
    }

    if (!from_cache) {
        res = add_cached_connection(fdes -> url, fdes -> conn);
        if (res != WL_STATUS_OK) {
            /* Nobody else knows this connection: if we don't cache it, nobody will ever close it */
            l_error("Failed to cache the connection");
            close_file(fdes, 0);
            return res;
        }
    }

    *fdesp = fdes;

    return WL_STATUS_OK;
}

enum mbe_status wlib_mbe_sql_fdes_lock(struct wmbe_sql_file *fdes) {
    enum mbe_status res;

    if (fdes -> lock_state != WMBE_L_NONE) {
        l_warn("Locking a file that is already locked");
    }

    /* 
     * Avoid double locking. Note that when the file lock
     * is WMBE_L_LOST, the lock is free at backend level so it's ok to call begin().
     * A WMBE_L_BROKEN one is released first, which gets us there.
     */
    if (fdes -> lock_state == WMBE_L_BROKEN) {
        /* The transaction is aborted, but it is still open and we are counted in it: end it,
         * or it would never be over */
        wlib_mbe_sql_fdes_unlock(fdes);
    }

    if (fdes -> lock_state != WMBE_L_LOCKED) {
        res = fdes -> be -> begin(fdes -> conn);
        if (res != MBE_S_OK) {
            return res;
        }

        fdes -> lock_state = WMBE_L_LOCKED;
    }   
    return MBE_S_OK;
}

enum mbe_status wlib_mbe_sql_fdes_unlock(struct wmbe_sql_file *fdes) {
    enum mbe_status res;

    if (fdes -> lock_state == WMBE_L_NONE) {
        l_warn("Unlocking a file that is not locked");
    }

    if (fdes -> lock_state == WMBE_L_LOST) {
        /* The transaction went with the connection: there is nothing to commit, and the
         * connection may be in the middle of the transaction of another file */
        res = MBE_S_OK;
    } else {
        res = fdes -> be -> commit(fdes -> conn);
    }
    fdes -> lock_state = WMBE_L_NONE;
    if (fdes -> stmts_stale) {
        /* The transaction is over, so now we can prepare the statements again.
         * Best effort: if it fails, it stays stale */
        reprepare(fdes);
    }

    return res;
}

enum mbe_status wlib_mbe_sql_fdes_exec(struct wmbe_sql_file *fdes, struct wmbe_sql_stmt *pstmt, char **values, 
        struct wmbe_sql_cursor **cursor, int transactional, long timeout) {

    enum mbe_status res;
    enum wl_status status;
    
    if (fdes -> lock_state == WMBE_L_BROKEN || fdes -> lock_state == WMBE_L_LOST) {
        l_warn("Transaction broken by connection reset");
        return MBE_S_INVALID_TRANSACTION;
    }

    res = fdes -> be -> exec_stmt(pstmt, timeout, values, cursor);

    if (res == WMBE_S_STMT_LOST) {
        l_warn("The server lost the prepared statements of %s", fdes -> table);

        if (transactional) {
            /* The error aborted the transaction, and the lock with it. We can't prepare
             * anything until it ends: wlib_mbe_sql_fdes_unlock() will do it */
            fdes -> stmts_stale = 1;
            if (fdes -> lock_state == WMBE_L_LOCKED) {
                fdes -> lock_state = WMBE_L_BROKEN;
            }
            return MBE_S_IO_ERROR;
        }

        res = reprepare(fdes) == WL_STATUS_OK
            ? fdes -> be -> exec_stmt(pstmt, timeout, values, cursor)
            : MBE_S_IO_ERROR;
        if (res == WMBE_S_STMT_LOST) {
            /* Didn't help: let the connection be reset */
            res = MBE_S_IO_ERROR;
        }
    }

    if (res != MBE_S_IO_ERROR) {
        return res;
    }

    /* The connection disappeared. This can be fixed by the backend */
    res = fdes -> be -> restore_connection(fdes -> conn);
    if (res == MBE_S_OK) {
        /* Recompile the prepared statements, that most likely went lost */
        for (int i = 0; i < mfdt_watermark; i++) {
            /* Closed (VALID) entries must be refreshed too: wlib_mbe_sql_fdes_open() hands them
             * back from the cache without preparing their statements again */
            if (mfdt[i].state != WMBE_F_FREE && mfdt[i].conn == fdes -> conn) {
                if (mfdt[i].lock_state == WMBE_L_LOCKED || mfdt[i].lock_state == WMBE_L_BROKEN) {
                    mfdt[i].lock_state = WMBE_L_LOST;
                }
                
                status = reprepare(&mfdt[i]);
                if (status != WL_STATUS_OK) {
                    /* Hopefully this won't happen because it implies that
                     * some tables won't be reachable any more */
                    return MBE_S_IO_ERROR;
                }
            }
        }
    }

    /* If we were in a transaction, don't retry this statement because the lock disappeared */
    if (res == MBE_S_OK && !transactional) {
        res = fdes -> be -> exec_stmt(pstmt, timeout, values, cursor);
    }

    return transactional ? MBE_S_IO_ERROR : res;
}

static void reset_state(struct wmbe_sql_file *fdes) {
    wmbe_mbe_sql_cursor_reset(fdes);

    fdes -> lock_state = WMBE_L_NONE;
}

static void close_file(struct wmbe_sql_file *file, int is_conn_shared) {
    file -> state = WMBE_F_FREE;
    reset_state(file);
    if (file -> fields != NULL) {
        free(file -> fields);
    }
    wlib_mbe_sql_free_file_stmts(file);
    memset(&file -> stmts, 0, sizeof(file -> stmts));
    if (file -> keys != NULL) {
        for (int i = 0; i < file -> field_number; i++) {
            wlib_mbe_sql_free_key_stmts(file, &file -> keys[i]);
        }
        free(file -> keys);
    }
    if (file -> conn != NULL && !is_conn_shared) {
        file -> be -> free_connection(file -> conn);
    }
}

void wlib_mbe_sql_fdes_close(struct wmbe_sql_file *fdes) {
    fdes -> state = WMBE_F_VALID;
    reset_state(fdes);

    l_debug("Descriptor closed and made available for reuse");
}

void wlib_mbe_sql_shutdown(void) {
    /* First of all, let's close all the file descriptors */
    for (int i = 0; i < mfdt_watermark; i++) {
        close_file(&mfdt[i], 1);
    }

    /* Then close the (possibly shared) connections as well */
    for (int i = 0; i < conn_cache_count; i++) {
        find_ifc(conn_cache[i].url) -> free_connection(conn_cache[i].conn);

        free(conn_cache[i].url);
    }

    free(conn_cache);
    conn_cache = NULL;
    conn_cache_count = 0;
    conn_cache_size = 0;

    mfdt_watermark = 0;
}
