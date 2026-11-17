/*
 * Functions implemented by the SQL backend.
 */

#ifndef __wlib_sql_be_h

#define __wlib_sql_be_h

#include "wlib-internal/util.h"

/* Size of the buffers used to fetch column values (terminator included). Values that
 * don't fit are not truncated: be->get() fails. Field definitions are checked against
 * this limit when the file is opened (see cfg.c). */
#define WMBE_SQL_VALUE_BUFLEN 1024

/* Returned by exec_stmt() when the connection is fine but the server doesn't know the prepared
 * statement any more. Internal: wlib_mbe_sql_fdes_exec() deals with it and never lets it out. */
#define WMBE_S_STMT_LOST ((enum mbe_status) 1000)

struct wmbe_sql_conn;
struct wmbe_sql_stmt;
struct wmbe_sql_cursor;

enum wmbe_sql_be_param_type {

    WMBE_SQL_BE_PT_VARCHAR,
    WMBE_SQL_BE_PT_INTEGER,
    WMBE_SQL_BE_PT_FLOAT,

};

struct wmbe_sql_be_ifc {

    const char *column_escape;

    /* The selects that get the page after a given record compare (key, primary key) as a row
     * if this is set, and with two ranges if not (see sql.c). Depends on what the server
     * is good at */
    int keyset_row_compare;

    void (*get_phold) (char *buf, int bufsize, int pos);
    enum mbe_status (*open_connection) (const char *url, struct wmbe_sql_conn **conn);
    enum mbe_status (*restore_connection) (struct wmbe_sql_conn *conn);
    /* Backends may leave the actual preparation for the first time that the statement is run, or
     * for prepare_now(): opening a file prepares a lot of statements, and most are never used.
     * If *pstmt is not NULL the statement is being prepared again on a new session. */
    enum mbe_status (*prepare_stmt) (struct wmbe_sql_conn *conn, 
        const char *name, const char *stmt, int params, enum wmbe_sql_be_param_type *types,
        struct wmbe_sql_stmt **pstmt);
    /* Optional. Makes sure that the statement is prepared on the server, which tells if it is
     * valid. Meant for a statement that stands for the rest (their tables and columns are the same) */
    enum mbe_status (*prepare_now) (struct wmbe_sql_stmt *pstmt);
    enum mbe_status (*exec_stmt) (struct wmbe_sql_stmt *pstmt, long timeout, char **values,
        struct wmbe_sql_cursor **cursor);
    int (*next) (struct wmbe_sql_cursor *cursor);
    enum mbe_status (*get) (struct wmbe_sql_cursor *cursor, int field, char *data, int data_len);

    enum mbe_status (*begin) (struct wmbe_sql_conn *conn);
    enum mbe_status (*commit) (struct wmbe_sql_conn *conn);
    /* Optional. Tells the backend that the next statement is the last one of the transaction, so
     * that it can save a round trip by committing along with it. commit() must be called anyway:
     * it reports the outcome */
    void (*commit_next) (struct wmbe_sql_conn *conn);

    int (*eof) (struct wmbe_sql_cursor *cursor);
    /* Number of records of the cursor (a page), and the one it is on: 0 is the first */
    int (*rows) (struct wmbe_sql_cursor *cursor);
    int (*position) (struct wmbe_sql_cursor *cursor);
    int (*affected_rows) (struct wmbe_sql_cursor *cursor);
    void (*free_cursor) (struct wmbe_sql_cursor *cursor);
    void (*free_statement) (struct wmbe_sql_stmt *pstmt);
    void (*free_connection) (struct wmbe_sql_conn *conn);
};

timestamp_t wmbe_sql_be_next_retry(void);

#ifdef PSQL_ENABLED
extern struct wmbe_sql_be_ifc wmbe_be_postgres;
#endif
#ifdef MYSQL_ENABLED
extern struct wmbe_sql_be_ifc wmbe_be_mysql;
#endif

#endif