/*
 * Implementation of the MBE-SQL interface on MySQL.
 *
 * MySQL uses a different prepared-statement model compared to PostgreSQL:
 *   - Parameters are bound via MYSQL_BIND arrays rather than passed as
 *     string arrays at execution time.
 *   - There is no server-side named statement registry: each MYSQL_STMT*
 *     handle IS the prepared statement and must be kept alive.
 *   - Result sets are fetched by binding output MYSQL_BIND arrays and
 *     calling mysql_stmt_fetch() row by row.
 *
 * The public interface (wmbe_sql_be_*) is identical to pq.c so that the
 * rest of the library (sql.c, mbe.c) is completely backend-agnostic.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>
#include <mysql/mysql.h>
#include <mysql/errmsg.h>

#include "wlib/usrlib.h"

#define L_FEATURE L_FEAT_DATA

#include "wlib-internal/log.h"
#include "wlib-mbe-sql/sql-be.h"

/* Maximum length of a serialised field value (mirrors pq.c assumption). */
#define MY_VALUE_BUFLEN WMBE_SQL_VALUE_BUFLEN

/* my_bool is gone from MySQL 8 (it is a bool there); MariaDB Connector/C still has it */
#if defined(MYSQL_VERSION_ID) && MYSQL_VERSION_ID >= 80000 && !defined(MARIADB_CLIENT_VERSION_STR)
typedef bool my_bool;
#endif

/* Client side timeouts, in seconds. Without them a peer that goes silent blocks the thread for
 * good: max_statement_time only helps while the server is alive. The read timeout must be above
 * the longest statement timeout that the library asks for. */
#ifndef MY_CONNECT_TIMEOUT
#define MY_CONNECT_TIMEOUT 5
#endif
#ifndef MY_RW_TIMEOUT
#define MY_RW_TIMEOUT 60
#endif

/* Character set of the connection. The values are bytes (Enscribe compares them byte by byte)
 * and the key columns are latin1_bin (see mbe-ini.md): with the same character set on both
 * sides the server has nothing to convert, and the index on a key can be used for sure. Any
 * other can be given in the environment, for tables that are not latin1. */
#ifndef MY_DEFAULT_CHARSET
#define MY_DEFAULT_CHARSET "latin1"
#endif
#define MY_CHARSET_ENV "WMBE_MYSQL_CHARSET"

/* Server error numbers that the client library has no name for */
#define MY_ER_LOCK_WAIT_TIMEOUT  1205
#define MY_ER_LOCK_DEADLOCK      1213
#define MY_ER_UNKNOWN_STMT       1243
#define MY_ER_SERVER_SHUTDOWN    1053
#define MY_ER_NEED_REPREPARE     1615
#define MY_ER_CONNECTION_KILLED  1927
#define MY_ER_STATEMENT_TIMEOUT  1969   /* MariaDB: max_statement_time */
#define MY_ER_QUERY_TIMEOUT      3024   /* MySQL: max_execution_time */

/* -------------------------------------------------------------------------
 * Prototypes
 * ---------------------------------------------------------------------- */
static void get_phold (char *buf, int bufsize, int pos);
static enum mbe_status open_connection (const char *url, struct wmbe_sql_conn **conn);
static enum mbe_status restore_connection (struct wmbe_sql_conn *conn);
static enum mbe_status prepare_stmt (struct wmbe_sql_conn *conn, 
    const char *name, const char *stmt, int params, enum wmbe_sql_be_param_type *types,
    struct wmbe_sql_stmt **pstmt);
static enum mbe_status prepare_now (struct wmbe_sql_stmt *pstmt);
static enum mbe_status exec_stmt (struct wmbe_sql_stmt *pstmt, long timeout, char **values, 
    struct wmbe_sql_cursor **cursor);
static int next (struct wmbe_sql_cursor *cursor);
static enum mbe_status get (struct wmbe_sql_cursor *cursor, int field, char *data, int data_len);

static enum mbe_status begin(struct wmbe_sql_conn *conn);
static enum mbe_status commit(struct wmbe_sql_conn *conn);

static int eof (struct wmbe_sql_cursor *cursor);
static int rows (struct wmbe_sql_cursor *cursor);
static int position (struct wmbe_sql_cursor *cursor);
static int affected_rows (struct wmbe_sql_cursor *cursor);
static void free_cursor (struct wmbe_sql_cursor *cursor);
static void free_statement (struct wmbe_sql_stmt *pstmt);
static void free_statement_data (struct wmbe_sql_stmt *pstmt);
static void free_connection (struct wmbe_sql_conn *conn);

/* -------------------------------------------------------------------------
 * Internal structures
 * ---------------------------------------------------------------------- */

struct wmbe_sql_conn {
    MYSQL *mysql;
    char          *url;          /* URL */
    int           x_level;       /* number of nested transactions */
    timestamp_t   reconn_time;   /* Circuit breaker */
    int           timeout;       /* Current statement timeout */
};

struct wmbe_sql_stmt {
    struct  wmbe_sql_conn *conn;  /* borrowed – do NOT close here */
    char        *sql;
    MYSQL_STMT  *stmt;          /* NULL until it is prepared, when it is first run */
    int          params;
    /* One bind slot per parameter; values are strings (MYSQL_TYPE_STRING). */
    MYSQL_BIND  *bind_in;
    unsigned long *lengths;
    /* Scratch for fetching the result, made when the statement first returns rows and kept: the
     * shape of the result of a statement does not change */
    int            res_fields;
    MYSQL_BIND    *res_bind;
    char          *res_bufs;
    unsigned long *res_lengths;
    my_bool       *res_nulls;
    my_bool       *res_errors;
    /* Size of the data of the last result: where the next one starts */
    size_t         data_hint;
    size_t         rows_hint;
};

struct wmbe_sql_be_ifc wmbe_be_mysql = {
    .column_escape = "`",
    .keyset_row_compare = 0,
    .get_phold = get_phold,
    .open_connection = open_connection,
    .restore_connection = restore_connection,
    .prepare_stmt = prepare_stmt,
    .prepare_now = prepare_now,
    .exec_stmt = exec_stmt,
    .next = next,
    .get = get,
    .eof = eof,
    .rows = rows,
    .position = position,
    .begin = begin,
    .commit = commit,
    .affected_rows = affected_rows,
    .free_cursor = free_cursor,
    .free_statement = free_statement,
    .free_connection = free_connection
};

/*
 * A cursor holds the complete result set after execution, copied out of the statement: the
 * statement can be run again, or prepared again on a new connection, while the cursor is in use.
 * This is what the PostgreSQL backend does too.
 */
struct wmbe_sql_cell {
    size_t        ofs;           /* of the value (NUL-terminated) in the cursor's data */
    unsigned long len;           /* real length: it may be more than we keep */
    int           is_null;
};

struct wmbe_sql_cursor {
    int           field_count;
    int           rows;
    int           affected_rows;
    int           current_row;   /* 0-based index of the current row */
    struct wmbe_sql_cell *cells; /* rows * field_count, by row */
    char         *data;
    size_t        data_used;
    size_t        data_size;
};

/* -------------------------------------------------------------------------
 * Connection
 * ---------------------------------------------------------------------- */

/* Errors that mean that the connection is gone, and has to be restored */
static int conn_lost(unsigned int e) {
    return e == CR_SERVER_GONE_ERROR || e == CR_SERVER_LOST || e == CR_CONNECTION_ERROR
        || e == CR_CONN_HOST_ERROR || e == MY_ER_CONNECTION_KILLED || e == MY_ER_SERVER_SHUTDOWN;
}

static pthread_once_t lib_once = PTHREAD_ONCE_INIT;

/* mysql_init() does this too, but not safely when several threads open their first connection */
static void lib_init(void) {
    mysql_library_init(0, NULL, NULL);
}

/*
 * Parse a libpq-style connection URL into individual mysql_real_connect()
 * parameters.  We support the subset actually used in cfg.c:
 *
 *   mysql://[user[:password]@]host[:port]/dbname
 */
static int parse_mysql_url(const char *url,
                            char *host,  size_t hlen,
                            char *user,  size_t ulen,
                            char *pass,  size_t plen,
                            char *db,    size_t dlen,
                            unsigned int *port) {
    const char *p;
    const char *at, *colon, *slash, *port_colon;

    if (strncmp(url, "mysql://", 8) != 0) {
        return -1;
    }
    p = url + 8;

    /* Default values */
    host[0] = user[0] = pass[0] = db[0] = '\0';
    *port = 3306;

    /* The credentials end at the last '@' that is before the first '/': the password can have
     * an '@' (there is no decoding, so it cannot have a '/') */
    slash = strchr(p, '/');
    if (slash == NULL) {
        return -1;
    }

    at = NULL;
    for (const char *q = p; q < slash; q++) {
        if (*q == '@') {
            at = q;
        }
    }

    if (at != NULL) {
        /* user[:pass] section */
        colon = memchr(p, ':', at - p);
        if (colon != NULL) {
            size_t ul = colon - p;
            size_t pl = at - colon - 1;
            strncpy(user, p, ul < ulen ? ul : ulen - 1);
            user[ul < ulen ? ul : ulen - 1] = '\0';
            strncpy(pass, colon + 1, pl < plen ? pl : plen - 1);
            pass[pl < plen ? pl : plen - 1] = '\0';
        } else {
            size_t ul = at - p;
            strncpy(user, p, ul < ulen ? ul : ulen - 1);
            user[ul < ulen ? ul : ulen - 1] = '\0';
        }
        p = at + 1;
    }

    /* host[:port]/db */
    strncpy(db, slash + 1, dlen - 1);
    db[dlen - 1] = '\0';

    port_colon = memchr(p, ':', slash - p);
    if (port_colon != NULL) {
        size_t hl = port_colon - p;
        strncpy(host, p, hl < hlen ? hl : hlen - 1);
        host[hl < hlen ? hl : hlen - 1] = '\0';
        *port = (unsigned int) atoi(port_colon + 1);
    } else {
        size_t hl = slash - p;
        strncpy(host, p, hl < hlen ? hl : hlen - 1);
        host[hl < hlen ? hl : hlen - 1] = '\0';
    }

    return 0;
}

static enum mbe_status my_connect(struct wmbe_sql_conn *conn) {
    char host[256], user[128], pass[128], db[128];
    unsigned int port;
    unsigned int timeout;
    const char *charset = getenv(MY_CHARSET_ENV);

    /* The URL has the password: it is never logged, only the host and the database */

    conn->mysql = mysql_init(NULL);
    if (conn->mysql == NULL) {
        l_error("mysql_init() failed – out of memory?");
        return MBE_S_OUT_OF_MEMORY;
    }

    if (parse_mysql_url(conn -> url, host, sizeof(host), user, sizeof(user),
                        pass, sizeof(pass), db, sizeof(db), &port) != 0) {
        l_error("Cannot parse the MySQL URL (expected mysql://[user[:password]@]host[:port]/db)");
        goto fail;
    }

    /* Like Enscribe, which has no snapshots: what we read is what the others have committed so far.
     * It is an init command so that it is also run when the connection is restored */
    if (mysql_options(conn->mysql, MYSQL_INIT_COMMAND,
                      "SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED")) {
        l_error("Failed to set the isolation level");
        goto fail;
    }

    if (charset == NULL || charset[0] == '\0') {
        charset = MY_DEFAULT_CHARSET;
    }
    if (mysql_options(conn->mysql, MYSQL_SET_CHARSET_NAME, charset)) {
        l_error("Failed to set the character set %s", charset);
        goto fail;
    }

    timeout = MY_CONNECT_TIMEOUT;
    mysql_options(conn->mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    timeout = MY_RW_TIMEOUT;
    mysql_options(conn->mysql, MYSQL_OPT_READ_TIMEOUT, &timeout);
    mysql_options(conn->mysql, MYSQL_OPT_WRITE_TIMEOUT, &timeout);

    if (!mysql_real_connect(conn->mysql,
                            host[0] ? host : NULL,
                            user[0] ? user : NULL,
                            pass[0] ? pass : NULL,
                            db[0]   ? db   : NULL,
                            port, NULL, CLIENT_FOUND_ROWS)) {
        l_error("Failed to connect to %s:%u/%s: %s", host, port, db, mysql_error(conn->mysql));
        goto fail;
    }

    return MBE_S_OK;

fail:
    /* An unconnected handle must not look like a live connection to the "fail fast" checks */
    mysql_close(conn->mysql);
    conn->mysql = NULL;
    return MBE_S_IO_ERROR;
}

enum mbe_status open_connection(const char *url, struct wmbe_sql_conn **connp) {
    struct wmbe_sql_conn *conn;
    enum mbe_status res;

    l_debug("Opening MySQL connection");

    pthread_once(&lib_once, lib_init);

    conn = calloc(1, sizeof(struct wmbe_sql_conn));
    if (conn == NULL) {
        l_error("Out of memory when creating the conn struct");
        return MBE_S_OUT_OF_MEMORY;
    }

    conn->url = strdup(url);
    if (conn->url == NULL) {
        l_error("Out of memory duplicating URL");
        free(conn);
        return MBE_S_OUT_OF_MEMORY;
    }

    res = my_connect(conn);
    if (res != MBE_S_OK) {
        free_connection(conn);
        return res;
    }

    /* Transaction state must be set here because pq_connect is
     * used for restoring connections where these variables must
     * be preserved */
    conn -> x_level = 0;
    conn -> timeout = 0;
    conn -> reconn_time = 0;

    l_debug("MySQL connection OK");
    *connp = conn;
    return MBE_S_OK;
}

static enum mbe_status restore_connection (struct wmbe_sql_conn *conn) {
    if (conn -> mysql != NULL) {
        mysql_close(conn -> mysql);
        conn -> mysql = NULL;
    }

    conn -> x_level = 0;
    conn -> timeout = 0;    // Reset default

    if (u_time() < conn -> reconn_time) {
        l_debug("Reconnection timeout not reached, not attempting to restore connection");
        return MBE_S_IO_ERROR;
    }

    conn -> reconn_time = wmbe_sql_be_next_retry();

    return my_connect(conn);
}

void free_connection(struct wmbe_sql_conn *conn) {
    if (conn->mysql != NULL) {
        mysql_close(conn->mysql);
    }
    if (conn -> url != NULL) {
        free(conn -> url);
    }
    free(conn);
}

/* -------------------------------------------------------------------------
 * Prepared statements
 * ---------------------------------------------------------------------- */

static void get_phold (char *buf, int bufsize, int pos) {
    strncpy(buf, "?", bufsize - 1);
    buf[bufsize - 1] = '\0';
}

enum mbe_status prepare_stmt(struct wmbe_sql_conn *conn,
    const char *name, const char *stmt_sql, int params,
    enum wmbe_sql_be_param_type *types,
    struct wmbe_sql_stmt **pstmtp) {

    struct wmbe_sql_stmt *pstmt = *pstmtp;

    /*
     * 'name' is used as an identifier in pq.c's server-side registry.
     * MySQL has no equivalent; we simply log it for debugging.
     */
    l_debug("Registering MySQL statement [%s]: %s", name, stmt_sql);

    if (pstmt == NULL) {
        pstmt = calloc(1, sizeof(struct wmbe_sql_stmt));
        if (pstmt == NULL) {
            return MBE_S_OUT_OF_MEMORY;
        }
        pstmt->sql = strdup(stmt_sql);
        if (pstmt->sql == NULL) {
            free(pstmt);
            return MBE_S_OUT_OF_MEMORY;
        }
        pstmt->conn = conn;
        pstmt->params = params;
    } else {
        /* A new connection: what the old one had is gone, and it is prepared again when needed */
        free_statement_data(pstmt);
    }

    *pstmtp = pstmt;

    return MBE_S_OK;
}

/* Prepares the statement on the server, unless it is already */
enum mbe_status prepare_now(struct wmbe_sql_stmt *pstmt) {
    unsigned int my_errno;
    int params = pstmt->params;

    if (pstmt->stmt != NULL) {
        return MBE_S_OK;
    }

    if (pstmt->conn->mysql == NULL) {
        return MBE_S_IO_ERROR;
    }

    pstmt->stmt = mysql_stmt_init(pstmt->conn->mysql);
    if (pstmt->stmt == NULL) {
        l_error("mysql_stmt_init() failed");
        return MBE_S_OUT_OF_MEMORY;
    }

    if (mysql_stmt_prepare(pstmt->stmt, pstmt->sql, strlen(pstmt->sql)) != 0) {
        my_errno = mysql_stmt_errno(pstmt->stmt);
        l_error("mysql_stmt_prepare(%s) failed: %s", pstmt->sql, mysql_stmt_error(pstmt->stmt));
        free_statement_data(pstmt);

        /* A broken connection is fixed by resetting it, a wrong statement is not */
        return conn_lost(my_errno) ? MBE_S_IO_ERROR : MBE_S_INVALID_KEY;
    }

    if (params > 0) {
        pstmt->bind_in  = calloc(params, sizeof(MYSQL_BIND));
        pstmt->lengths  = calloc(params, sizeof(unsigned long));
        if (!pstmt->bind_in || !pstmt->lengths) {
            free_statement_data(pstmt);
            return MBE_S_OUT_OF_MEMORY;
        }

        /*
         * Pre-configure each bind slot.  We always pass values as
         * MYSQL_TYPE_STRING and let MySQL do the implicit cast; this
         * mirrors how pq.c passes everything as text strings.
         */
        for (int i = 0; i < params; i++) {
            pstmt->bind_in[i].buffer_type   = MYSQL_TYPE_STRING;
            pstmt->bind_in[i].buffer        = NULL;   /* set at exec time */
            pstmt->bind_in[i].buffer_length = 0;
            pstmt->bind_in[i].length        = &pstmt->lengths[i];
            pstmt->bind_in[i].is_null       = 0;
        }
    }

    l_debug("MySQL statement prepared OK");

    return MBE_S_OK;
}

void free_statement_data(struct wmbe_sql_stmt *pstmt) {
    if (pstmt == NULL) {
        return;
    }
    if (pstmt->stmt) {
        mysql_stmt_close(pstmt->stmt);
        pstmt -> stmt = NULL;
    }
    if (pstmt->bind_in) {
        free(pstmt->bind_in);
        pstmt -> bind_in = NULL;
    }
    if (pstmt->lengths) {
        free(pstmt->lengths);
        pstmt -> lengths = NULL;
    }
    free(pstmt->res_bind);
    free(pstmt->res_bufs);
    free(pstmt->res_lengths);
    free(pstmt->res_nulls);
    free(pstmt->res_errors);
    pstmt->res_bind = NULL;
    pstmt->res_bufs = NULL;
    pstmt->res_lengths = NULL;
    pstmt->res_nulls = NULL;
    pstmt->res_errors = NULL;
    pstmt->res_fields = 0;
}

void free_statement(struct wmbe_sql_stmt *pstmt) {
    free_statement_data(pstmt);
    if (pstmt != NULL) {
        free(pstmt->sql);
        free(pstmt);
    }
}

/* -------------------------------------------------------------------------
 * Execution
 * ---------------------------------------------------------------------- */
static enum mbe_status decode_result(const char *op, MYSQL_STMT *stmt) {
    unsigned int my_errno = mysql_stmt_errno(stmt);
    enum mbe_status status;

    if (my_errno == MY_ER_STATEMENT_TIMEOUT || my_errno == MY_ER_QUERY_TIMEOUT) {
        l_warn("MySQL statement timed out");
        status = MBE_S_TIMEOUT;
    } else if (my_errno == MY_ER_LOCK_WAIT_TIMEOUT || my_errno == MY_ER_LOCK_DEADLOCK) {
        /* Not a problem with the statement: somebody else holds the record. A deadlock has
         * rolled the transaction back already */
        l_warn("%s: lock wait timeout or deadlock (errno %u): %s", op, my_errno,
               mysql_stmt_error(stmt));
        status = MBE_S_TIMEOUT;
    } else if (my_errno == 1062) {
        /* ER_DUP_ENTRY – unique constraint violation (SQLSTATE 23505) */
        l_debug("Statement failed due to duplicate-key violation");
        status = MBE_S_RECORD_EXISTS;
    } else if (my_errno == MY_ER_NEED_REPREPARE || my_errno == MY_ER_UNKNOWN_STMT) {
        /* The server dropped or invalidated the statement: the caller prepares it again */
        l_debug("%s: the statement is not valid on the server (errno %u)", op, my_errno);
        status = WMBE_S_STMT_LOST;
    } else if (conn_lost(my_errno)) {
        /* Connection dropped – signal IO error so the caller reconnects */
        l_error("MySQL connection lost (errno %u): %s", my_errno,
                mysql_stmt_error(stmt));
        status = MBE_S_IO_ERROR;
    } else {
        /* Logical error (bad query, other constraint, …) – do not reconnect */
        l_error("%s failed (errno %u): %s", op, my_errno, mysql_stmt_error(stmt));
        status = MBE_S_INVALID_KEY;
    }
    return status;
}

/* Room for n more bytes of data */
static int reserve(struct wmbe_sql_cursor *cursor, size_t n) {
    if (cursor->data_used + n > cursor->data_size) {
        size_t size = cursor->data_size ? 2 * cursor->data_size : 4096;
        char *grown;

        while (size < cursor->data_used + n) {
            size *= 2;
        }
        grown = realloc(cursor->data, size);
        if (grown == NULL) {
            return 0;
        }
        cursor->data = grown;
        cursor->data_size = size;
    }

    return 1;
}

/* Makes (once per statement) the arrays that the result is fetched into */
static enum mbe_status result_scratch(struct wmbe_sql_stmt *pstmt, int fields) {
    if (pstmt->res_fields == fields && pstmt->res_bind != NULL) {
        return MBE_S_OK;
    }

    free(pstmt->res_bind);
    free(pstmt->res_bufs);
    free(pstmt->res_lengths);
    free(pstmt->res_nulls);
    free(pstmt->res_errors);

    pstmt->res_bind    = calloc(fields, sizeof(MYSQL_BIND));
    pstmt->res_bufs    = malloc((size_t) fields * MY_VALUE_BUFLEN);
    pstmt->res_lengths = calloc(fields, sizeof(unsigned long));
    pstmt->res_nulls   = calloc(fields, sizeof(my_bool));
    pstmt->res_errors  = calloc(fields, sizeof(my_bool));
    pstmt->res_fields  = fields;

    if (!pstmt->res_bind || !pstmt->res_bufs || !pstmt->res_lengths || !pstmt->res_nulls
        || !pstmt->res_errors) {
        /* what was made is freed with the statement (or by the next call, which retries) */
        pstmt->res_fields = 0;
        return MBE_S_OUT_OF_MEMORY;
    }

    for (int i = 0; i < fields; i++) {
        pstmt->res_bind[i].buffer_type   = MYSQL_TYPE_STRING;
        pstmt->res_bind[i].buffer        = pstmt->res_bufs + (size_t) i * MY_VALUE_BUFLEN;
        pstmt->res_bind[i].buffer_length = MY_VALUE_BUFLEN;
        pstmt->res_bind[i].length        = &pstmt->res_lengths[i];
        pstmt->res_bind[i].is_null       = &pstmt->res_nulls[i];
        pstmt->res_bind[i].error         = &pstmt->res_errors[i];
    }

    return MBE_S_OK;
}

/* Copies the rows of the stored result into the cursor. Values longer than the buffer are kept
 * cut: get() refuses them, knowing the real length. */
static enum mbe_status copy_rows(struct wmbe_sql_stmt *pstmt, struct wmbe_sql_cursor *cursor) {
    MYSQL_STMT *stmt = pstmt->stmt;
    int fields = cursor->field_count;
    size_t cap = 0;
    enum mbe_status status = result_scratch(pstmt, fields);
    char *bufs = pstmt->res_bufs;
    unsigned long *lengths = pstmt->res_lengths;
    my_bool *nulls = pstmt->res_nulls;

    if (status != MBE_S_OK) {
        return status;
    }

    /* The rows are read as they come, without storing the result on the client first (that would
     * be a second copy of all of it): the row count is not known up front, so the cells start as
     * many as the last result of this statement had, and double when they are not enough */
    cap = pstmt->rows_hint > 0 ? pstmt->rows_hint : 16;
    cursor->cells = calloc(cap * fields, sizeof(struct wmbe_sql_cell));
    if (cursor->cells == NULL) {
        return MBE_S_OUT_OF_MEMORY;
    }

    /* Starts as big as the last result of this statement, so that it is not grown row by row */
    if (pstmt->data_hint > 0 && !reserve(cursor, pstmt->data_hint)) {
        return MBE_S_OUT_OF_MEMORY;
    }

    /* Done every time: it is cheap, and the binding has to be there after each execute */
    if (mysql_stmt_bind_result(stmt, pstmt->res_bind) != 0) {
        l_error("mysql_stmt_bind_result failed: %s", mysql_stmt_error(stmt));
        return MBE_S_IO_ERROR;
    }

    for (;;) {
        int rc = mysql_stmt_fetch(stmt);

        if (rc == MYSQL_NO_DATA) {
            break;
        }
        if (rc != 0 && rc != MYSQL_DATA_TRUNCATED) {
            /* Errors of the execution (a timeout, a deadlock) can come here, with no stored result */
            return decode_result("mysql_stmt_fetch", stmt);
        }

        if ((size_t) cursor->rows == cap) {
            struct wmbe_sql_cell *grown = realloc(cursor->cells,
                2 * cap * fields * sizeof(struct wmbe_sql_cell));

            if (grown == NULL) {
                return MBE_S_OUT_OF_MEMORY;
            }
            cursor->cells = grown;
            cap *= 2;
        }

        for (int i = 0; i < fields; i++) {
            struct wmbe_sql_cell *cell = &cursor->cells[(size_t) cursor->rows * fields + i];
            size_t kept = lengths[i] < MY_VALUE_BUFLEN ? lengths[i] : MY_VALUE_BUFLEN - 1;

            cell->is_null = nulls[i];
            cell->len = lengths[i];
            if (nulls[i]) {
                continue;
            }
            if (!reserve(cursor, kept + 1)) {
                return MBE_S_OUT_OF_MEMORY;
            }
            cell->ofs = cursor->data_used;
            memcpy(cursor->data + cursor->data_used, bufs + (size_t) i * MY_VALUE_BUFLEN, kept);
            cursor->data[cursor->data_used + kept] = '\0';
            cursor->data_used += kept + 1;
        }
        cursor->rows++;
    }

    pstmt->data_hint = cursor->data_used;
    pstmt->rows_hint = cursor->rows;

    return MBE_S_OK;
}

enum mbe_status exec_stmt(struct wmbe_sql_stmt *pstmt, long timeout,
    char **values, struct wmbe_sql_cursor **cursorp) {

    struct wmbe_sql_cursor *cursor;
    enum mbe_status status;

    l_debug("Executing MySQL statement");
    if (pstmt -> conn -> mysql == NULL) {
        l_error("Fail fast: connection to DB has not been recovered yet");
        return MBE_S_IO_ERROR;
    }

    if (pstmt->stmt == NULL) {
        status = prepare_now(pstmt);

        if (status != MBE_S_OK) {
            return status;
        }
    }

    if (timeout != pstmt -> conn -> timeout && pstmt -> conn -> mysql != NULL) {
        char tql[64];
    
        pstmt -> conn -> timeout = timeout;
        /* Integers only: %f follows the locale, and "1,500" is not a number to the server */
        long ms = timeout < 0 ? 0 : timeout;

        snprintf(tql, sizeof(tql), "SET max_statement_time=%ld.%03ld", ms / 1000, ms % 1000);
        if (mysql_query(pstmt->conn->mysql, tql) != 0)
            l_warn("SET max_statement_time failed: %s", mysql_error(pstmt->conn->mysql));

    }

    /* Bind input parameters */
    if (pstmt->params > 0) {
        for (int i = 0; i < pstmt->params; i++) {
            pstmt->lengths[i]           = strlen(values[i]);
            pstmt->bind_in[i].buffer    = values[i];
            pstmt->bind_in[i].buffer_length = pstmt->lengths[i] + 1;
        }
        if (mysql_stmt_bind_param(pstmt->stmt, pstmt->bind_in) != 0) {
            unsigned int my_errno = mysql_stmt_errno(pstmt->stmt);
            if (conn_lost(my_errno)) {
                l_error("MySQL connection lost during bind (errno %u): %s", my_errno,
                        mysql_stmt_error(pstmt->stmt));
                return MBE_S_IO_ERROR;
            }
            l_error("mysql_stmt_bind_param failed (errno %u): %s", my_errno,
                    mysql_stmt_error(pstmt->stmt));
            return MBE_S_INVALID_KEY;
        }
    }

    if (mysql_stmt_execute(pstmt->stmt) != 0) {
        return decode_result("mysql_stmt_execute", pstmt->stmt);
    }

    cursor = calloc(1, sizeof(struct wmbe_sql_cursor));
    if (cursor == NULL) return MBE_S_OUT_OF_MEMORY;

    cursor->field_count = mysql_stmt_field_count(pstmt->stmt);
    cursor->affected_rows = (long long) mysql_stmt_affected_rows(pstmt->stmt);

    if (cursor->field_count > 0) {
        /* The cursor gets the complete result set (copy_rows), so that the row count is known
         * and it can be walked freely – matches PQ behaviour. */
        status = copy_rows(pstmt, cursor);
        mysql_stmt_free_result(pstmt->stmt);
        if (status != MBE_S_OK) {
            free_cursor(cursor);
            return status;
        }
    }

    *cursorp = cursor;
    l_debug("MySQL statement executed OK, %d rows", cursor->rows);
    return MBE_S_OK;
}

/* -------------------------------------------------------------------------
 * Cursor navigation
 * ---------------------------------------------------------------------- */

/*
 * Advance to the next row.
 * Returns 1 if the new current row is valid, 0 at EOF.
 *
 * NOTE: pq.c's next() increments current_row *before* checking,
 * and get() uses current_row as the already-advanced index.
 * We mirror that contract.
 */
int next(struct wmbe_sql_cursor *cursor) {
    if (cursor->current_row < cursor->rows) {
        cursor->current_row++;
    }

    return cursor->current_row < cursor->rows;
}

int eof(struct wmbe_sql_cursor *cursor) {
    return cursor == NULL || cursor->current_row >= cursor->rows;
}

static int rows(struct wmbe_sql_cursor *cursor) {
    return cursor->rows;
}

static int position(struct wmbe_sql_cursor *cursor) {
    return cursor->current_row;
}

/*
 * Retrieve a single field value from the current row.
 * 'field' is 1-based (matching pq.c / PQgetvalue convention).
 */
enum mbe_status get(struct wmbe_sql_cursor *cursor,
    int field, char *data, int data_len) {

    int idx = field - 1;
    const struct wmbe_sql_cell *cell;

    if (idx < 0 || idx >= cursor->field_count || cursor->current_row >= cursor->rows) {
        return MBE_S_BAD_SPECS;
    }

    cell = &cursor->cells[(size_t) cursor->current_row * cursor->field_count + idx];
    if (cell->is_null) {
        data[0] = '\0';
        return MBE_S_OK;
    }

    /* cell->len is the real length, even when it exceeds what we kept */
    if (cell->len >= (unsigned long) data_len || cell->len >= MY_VALUE_BUFLEN) {
        l_error("Value of column %d is too long: %lu bytes, buffer holds %d",
                field, cell->len, data_len - 1);
        return MBE_S_BAD_SPECS;
    }

    memcpy(data, cursor->data + cell->ofs, cell->len + 1);  /* includes the terminator */

    return MBE_S_OK;
}

static int affected_rows (struct wmbe_sql_cursor *cursor) {
    return cursor->affected_rows;
}

void free_cursor(struct wmbe_sql_cursor *cursor) {
    if (cursor == NULL) return;

    free(cursor->cells);
    free(cursor->data);
    free(cursor);
}

/* -------------------------------------------------------------------------
 * Locking
 * ---------------------------------------------------------------------- */

/*
 * Transactions use START TRANSACTION / COMMIT with the connection left in autocommit: one round
 * trip to begin and one to commit. Toggling autocommit (SET autocommit=0 ... SET autocommit=1)
 * costs two more round trips, in every write.
 */
static enum mbe_status begin(struct wmbe_sql_conn *conn) {
    if (!conn -> x_level) {
        if (conn -> mysql == NULL) {
            l_error("Fail fast: connection to DB has not been recovered yet");
            return MBE_S_IO_ERROR;
        }

        if (mysql_query(conn -> mysql, "START TRANSACTION")) {
            l_error("Failed to start a transaction: %s", mysql_error(conn -> mysql));
            return MBE_S_IO_ERROR;
        }
    }

    /* Only now: if we failed there's no transaction, so there's nothing to commit later */
    conn -> x_level++;

    return MBE_S_OK;
}

static enum mbe_status commit(struct wmbe_sql_conn *conn) {
    enum mbe_status status = MBE_S_OK;

    if (!conn -> x_level) {
        return MBE_S_OK;
    }

    if (!--conn -> x_level) {
        if (conn -> mysql == NULL) {
            l_error("Fail fast: connection to DB has not been recovered yet");
            return MBE_S_IO_ERROR;
        }

        /* The connection is in autocommit again after the COMMIT or the ROLLBACK */
        if (mysql_commit(conn -> mysql)) {
            l_error("COMMIT failed: %s", mysql_error(conn -> mysql));
            status = MBE_S_IO_ERROR;
            /* Make sure that "failed" means "not committed" */
            mysql_rollback(conn -> mysql);
        }
    }
    return status;
}
