/*
 * SQL-based MBE implementation. This is not an API, so I'm collecting
 * all the structures and functions in a single header file.
 * 
 * See comments to locate the .c file providing each function.
 */

#ifndef __wlib_sql_mbe_h

#define __wlib_sql_mbe_h

#include "sql-be.h"

/* Escape character of generic (LIKE) keys: dt.c uses it to escape the key, and the SELECT
 * statements declare it with ESCAPE (sql.c). It is declared explicitly because the default
 * depends on the server and its SQL mode, and the quoting of '\\' differs between servers. */
#define WMBE_SQL_LIKE_ESCAPE "!"

/* The selects on a key return a page of records: the limit is the last parameter. This
 * one means "all of them" */
#define WMBE_SQL_NO_LIMIT "9223372036854775807"

/* Most parameters of a continuation statement (wlib_mbe_sql_cont_params): the key, the last key
 * (twice, for some backends), the last primary key, the limit */
#define WMBE_SQL_MAX_CONT_PARAMS 5

/* Limit on strlen(table) + strlen(key column name). Prepared statements are named
 * "<table>_<prefix>[<key column>]" (sql.c) and PostgreSQL silently truncates names to 63
 * bytes, which makes different statements collide: a reverse scan would run the SQL of the
 * forward one. The longest name is "<table>_mbe_select_approx_inv_<column>" (23 characters
 * besides the two names), so 63 - 23 = 40. If a longer statement prefix is ever added, this
 * must be lowered. It is checked when the file is opened (cfg.c). */
#define WMBE_SQL_MAX_TABLE_KEY_LEN 40

struct wmbe_sql_datatype {

    const char *name;

    int virt;

    int can_be_key;

    int can_do_generic;

    int align;

    enum wmbe_sql_be_param_type type;

    enum mbe_status (*serialize) (const void *data, unsigned size, int generic, char **dest);

    enum mbe_status (*deserialize) (const char *text, void *dest, unsigned size);

    /* Maximum length (terminator excluded) of the text form of a field of the given size.
     * Checked against WMBE_SQL_VALUE_BUFLEN when the file is opened. */
    unsigned (*max_text_len) (unsigned size);

};

struct wmbe_sql_field {

    struct wmbe_sql_datatype *dt;

    char name[256];

    int ofs;

    int size;

    int param_pos;

    /* Alternate key whose values are unique (declared with unique = 1): a page of records is
     * ordered by the key alone, and the database doesn't have to sort by primary key as well */
    int unique;

};

struct wmbe_per_file_stmts {

    struct wmbe_sql_stmt *insert_stmt;

    struct wmbe_sql_stmt *update_stmt;

    struct wmbe_sql_stmt *delete_stmt;

    struct wmbe_sql_stmt *lock_stmt;
};

struct wmbe_per_key_stmts {
    
    struct wmbe_sql_stmt *select_exact_stmt;

    struct wmbe_sql_stmt *select_approx_stmt;

    struct wmbe_sql_stmt *select_gen_stmt;

    struct wmbe_sql_stmt *select_approx_inv_stmt;

    struct wmbe_sql_stmt *select_gen_inv_stmt;    

    /* Same, for the next page. They are prepared the first time they are used (NULL until then) */
    struct wmbe_sql_stmt *cont_exact_stmt;

    struct wmbe_sql_stmt *cont_approx_stmt;

    struct wmbe_sql_stmt *cont_gen_stmt;

    struct wmbe_sql_stmt *cont_approx_inv_stmt;

    struct wmbe_sql_stmt *cont_gen_inv_stmt;
};

struct wmbe_sql_key {

    struct wmbe_sql_field *field;

    struct wmbe_per_key_stmts stmts;

};

/* SETKEY option: read backwards */
#define WMBE_SQL_OPT_REVERSE 0x4000

/* cursor.c private data. Do not access these fields outside of it */
struct wmbe_key_specs {
    short updated;
    struct wmbe_sql_stmt *stmt;
    short keyspecs;
    short positioning;
    short options;
    short keylen;
    void *key;

    /* Records are read a page at a time. When one is over, the next starts after the last record
     * of the previous: these are its key and primary key, as they are in the database */
    char *key_text;     /* The key, serialized, for the statements */
    char *last_key;
    char *last_pk;
    short resume;       /* The next page is what has to be read, not the first one */
    short consumed;     /* The record that an exact key finds has been read: the next read finds nothing */
    int page_limit;     /* Records that the page that we have may have */
    int next_limit;     /* Same, for the next one */
};

struct wmbe_sql_file {

    enum {
        /* Entry has never been used */
        WMBE_F_FREE = 0,
        /* Entry is currently in use */
        WMBE_F_USED,
        /* Entry has been closed but the fields are still valid */
        WMBE_F_VALID
    } state;

    enum {

        /* No record has been locked by this fdes */
        WMBE_L_NONE = 0,

        /* A record is being locked */
        WMBE_L_LOCKED,

        /* The transaction that holds the lock is aborted (the server lost a prepared statement),
         * but it is still open on the connection until the lock is released */
        WMBE_L_BROKEN,

        /* The connection to the DB was reset while holding a lock: the transaction is gone, and
         * the connection does not count it any more. Releasing the lock must not commit: whatever
         * the connection is doing now belongs to somebody else */
        WMBE_L_LOST

    } lock_state;

    /* The server lost our prepared statements while a transaction was open: they can only be
     * prepared again when it ends */
    int stmts_stale;

    short access;

    short exclusion;

    char fname[256];

    char table[256];

    char url[256];

    struct wmbe_sql_conn *conn;

    struct wmbe_sql_be_ifc *be;

    int field_number;

    /* Length of a record, which is where the last field ends */
    int record_size;

    struct wmbe_sql_field *fields;

    struct wmbe_sql_key *keys;

    struct wmbe_per_file_stmts stmts;

    struct wmbe_key_specs specs;

    struct wmbe_sql_cursor *cursor;
};

/* Defined in dt.c */
extern struct wmbe_sql_datatype wmbe_sql_datatypes[];

/* sql.c */
enum mbe_status wlib_mbe_sql_prepare_file_stmts(struct wmbe_sql_file *fdes);
enum mbe_status wlib_mbe_sql_prepare_key_stmts(struct wmbe_sql_file *fdes, struct wmbe_sql_key *key);

enum mbe_status wlib_mbe_sql_get_cont_stmt(struct wmbe_sql_file *fdes, struct wmbe_sql_key *key,
    short positioning, int reverse, struct wmbe_sql_stmt **stmt);
int wlib_mbe_sql_cont_params(struct wmbe_sql_file *fdes, struct wmbe_sql_key *key, char *key_text,
    char *last_key, char *last_pk, char *limit, char **values);
void wlib_mbe_sql_free_file_stmts(struct wmbe_sql_file *fdes);
void wlib_mbe_sql_free_key_stmts(struct wmbe_sql_file *fdes, struct wmbe_sql_key *key);

/* fdes.c */
enum wl_status wlib_mbe_sql_fdes_open(const char *fname,
        short access, int exclusion, struct wmbe_sql_file **fdesp);
void wlib_mbe_sql_fdes_close(struct wmbe_sql_file *fdes);
void wlib_mbe_sql_shutdown(void);
enum mbe_status wlib_mbe_sql_fdes_lock(struct wmbe_sql_file *fdes);
enum mbe_status wlib_mbe_sql_fdes_unlock(struct wmbe_sql_file *fdes);

/* Always use this function to execute prepared statements. If the connection goes away it will
 * restore it, reset the prepared statements and re-issue the statement.
 * If transactional is set, the statement is not re-issued and an error will be returned. This
 * is important when the caller is holding a lock because it went lost with the connection. */
enum mbe_status wlib_mbe_sql_fdes_exec(struct wmbe_sql_file *fdes, struct wmbe_sql_stmt *pstmt, char **values, 
        struct wmbe_sql_cursor **cursor, int transactional, long timeout);

/* cursor.c */
enum mbe_status wlib_mbe_sql_cursor_set_specs(struct wmbe_sql_file *fdes, struct wmbe_sql_stmt *stmt,
    short keyspecs, short positioning, short options, short keylen, const void *key);
/* Size of the pages. 0, the default, grows from a few records (most reads need just a few) to
 * many (a long scan costs a round trip per page). Tests set it to have pages of a fixed size */
extern int wlib_mbe_sql_page_size;

enum mbe_status wlib_mbe_sql_cursor_update(struct wmbe_sql_file *fdes, int transactional, long timeout);
enum mbe_status wlib_mbe_sql_cursor_next(struct wmbe_sql_file *fdes, int transactional, long timeout, int *valid);
enum mbe_status wlib_mbe_sql_cursor_key_value(struct wmbe_sql_file *fdes, char *key, int keylen);
enum mbe_status wlib_mbe_sql_cursor_get_primary_key(const char *fop, struct wmbe_sql_file *fdes, char *key, int keylen, int *is_exact, 
        int transactional, long timeout);
void wmbe_mbe_sql_cursor_reset(struct wmbe_sql_file *fdes);
int wmbe_mbe_sql_cursor_is_set(struct wmbe_sql_file *fdes, int *just_set) ;

/* cfg.c */
struct wl_cfg_file;
enum wl_status wlib_mbe_cfg_parse_cfg_file(struct wmbe_sql_file *fdes, struct wl_cfg_file *cfg);

#endif