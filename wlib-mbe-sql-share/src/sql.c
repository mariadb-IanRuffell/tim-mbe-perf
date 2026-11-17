/*
 * A set of functions that build the statements for our MBE emulator.
 */

#include <string.h>
#include <stdio.h>

#include "wlib/usrlib.h"

#define L_FEATURE L_FEAT_DATA

#include "wlib-internal/log.h"
#include "wlib-mbe-sql/mbe-sql.h"
#include "wlib-mbe-sql/sql-be.h"

#define PSN_INSERT "mbe_insert"
#define PSN_UPDATE "mbe_update"
#define PSN_DELETE "mbe_delete"
#define PSN_LOCK   "mbe_lock"

#define PSN_SELECT_EXACT       "mbe_select_exact_"
#define PSN_SELECT_APPROX      "mbe_select_approx_"
#define PSN_SELECT_APPROX_INV  "mbe_select_approx_inv_"
#define PSN_SELECT_GEN         "mbe_select_gen_"
#define PSN_SELECT_GEN_INV     "mbe_select_gen_inv_"

/* Continuation of a select: the next page, after the last record of the previous one. Statement
 * names are limited to 63 bytes (see WMBE_SQL_MAX_TABLE_KEY_LEN): none of these may be longer
 * than the longest of the names above */
#define PSN_CONT_EXACT         "mbe_cont_exact_"
#define PSN_CONT_APPROX        "mbe_cont_approx_"
#define PSN_CONT_APPROX_INV    "mbe_cont_approx_inv_"
#define PSN_CONT_GEN           "mbe_cont_gen_"
#define PSN_CONT_GEN_INV       "mbe_cont_gen_inv_"

#define LIKE_ESCAPE_CLAUSE " ESCAPE '" WMBE_SQL_LIKE_ESCAPE "'"

#define SCOPED_STMT_SIZE(fdes, stmt) (strlen((fdes) -> table) + 1 + strlen(stmt) + 1)

/*
 * Whether records with the same value of the key have to be told apart by primary key: true for
 * an alternate key that may have duplicates. A unique key (the primary one, or an alternate
 * declared with unique = 1) is enough by itself, and then the selects don't ask for the extra
 * ordering. This matters: with "ORDER BY key, pk" MariaDB can't stop at the LIMIT on a unique
 * secondary index, it reads the whole range and sorts it.
 */
static int needs_pk_tiebreak(struct wmbe_sql_file *fdes, struct wmbe_sql_key *key) {
    return key -> field != fdes -> keys[0].field && !key -> field -> unique;
}

static void append(char **dest, const char *end, const char *s) {
    strncpy(*dest, s, end - *dest);
    *dest += strlen(*dest);
}

static void get_scoped_statement(char *scoped, size_t scoped_len, struct wmbe_sql_file *fdes, const char *stmt) {
    snprintf(scoped, scoped_len, "%s_%s", fdes -> table, stmt);
}

/* So far, our backends all use '?' as placeholders */
static void append_phold(struct wmbe_sql_file *fdes, char **dest, const char *end, int pos) {
    char buf[16];

    fdes -> be -> get_phold(buf, sizeof(buf), pos);

    append(dest, end, buf);
}

static enum mbe_status prepare_insert_statement(struct wmbe_sql_file *fdes) {
    enum wmbe_sql_be_param_type types[fdes -> field_number];
    char buf[1024];
    char *p, *end;
    enum mbe_status status;
    char scoped[SCOPED_STMT_SIZE(fdes, PSN_INSERT)];
    int count;

    get_scoped_statement(scoped, sizeof(scoped), fdes, PSN_INSERT);

    l_debug("Preparing INSERT statement (%s)", scoped);

    p = buf;
    end = buf + sizeof(buf) - 1;
    end[1] = 0;

    append(&p, end, "INSERT INTO ");
    append(&p, end,     fdes -> table);
    append(&p, end,          "(");
    count = 0;
    for (int i = 0; i < fdes -> field_number; i++) {
        if (fdes -> fields[i].dt -> virt) {
            continue;
        }

        if (count) {
            append(&p, end, ", ");
        }
        append(&p, end, fdes -> be -> column_escape);
        append(&p, end, fdes -> fields[i].name);
        append(&p, end, fdes -> be -> column_escape);
        types[count] = fdes -> fields[i].dt -> type;
        count++;
    }
    append(&p, end, ") VALUES(");
    for (int i = 0; i < count; i++) {
        if (i) {
            append(&p, end, ", ");
        }
        append_phold(fdes, &p, end, i);
    }
    append(&p, end, ")");

    l_debug("Statement is: %s", buf);

    status = fdes -> be -> prepare_stmt(fdes -> conn, scoped,
            buf, count, types, &fdes -> stmts.insert_stmt);
    if (status != MBE_S_OK) {
        l_error("Failed to compile statement: [%s] -- error code: %d", buf, status);
        return status;
    }

    return MBE_S_OK;
}

static enum mbe_status prepare_update_statement(struct wmbe_sql_file *fdes) {
    enum wmbe_sql_be_param_type types[fdes -> field_number + 1];
    char buf[1024];
    char *p, *end;
    enum mbe_status status;
    char scoped[SCOPED_STMT_SIZE(fdes, PSN_UPDATE)];
    int count;

    get_scoped_statement(scoped, sizeof(scoped), fdes, PSN_UPDATE);

    l_debug("Preparing UPDATE statement (%s)", scoped);

    p = buf;
    end = buf + sizeof(buf) - 1;
    end[1] = 0;

    append(&p, end, "UPDATE ");
    append(&p, end,     fdes -> table);
    append(&p, end,          " SET ");
    count = 0;
    for (int i = 0; i < fdes -> field_number; i++) {
        if (fdes -> fields[i].dt -> virt) {
            continue;
        }

        if (count) {
            append(&p, end, ", ");
        }
        append(&p, end, fdes -> be -> column_escape);
        append(&p, end, fdes -> fields[i].name);
        append(&p, end, fdes -> be -> column_escape);
        append(&p, end, "=");
        append_phold(fdes, &p, end, count);
        types[count] = fdes -> fields[i].dt -> type;
        count++;
    }
    append(&p, end, " WHERE ");
    append(&p, end,      fdes -> be -> column_escape);
    append(&p, end,      fdes -> keys[0].field -> name);
    append(&p, end,      fdes -> be -> column_escape);
    append(&p, end,        " = ");
    append_phold(fdes, &p, end, count);

    types[count++] = fdes -> keys[0].field -> dt -> type;

    l_debug("Statement is: %s", buf);

    status = fdes -> be -> prepare_stmt(fdes -> conn, scoped,
                buf, count, types, &fdes -> stmts.update_stmt);
    if (status != MBE_S_OK) {
        l_error("Failed to compile statement: [%s] -- error code: %d", buf, status);
        return status;
    }

    return MBE_S_OK;
}

static enum mbe_status prepare_delete_statement(struct wmbe_sql_file *fdes) {
    enum wmbe_sql_be_param_type type;
    char buf[1024];
    char *p, *end;
    char scoped[SCOPED_STMT_SIZE(fdes, PSN_DELETE)];
    enum mbe_status status;

    get_scoped_statement(scoped, sizeof(scoped), fdes, PSN_DELETE);

    l_debug("Preparing DELETE statement (%s)", scoped);

    p = buf;
    end = buf + sizeof(buf) - 1;
    end[1] = 0;

    append(&p, end, "DELETE FROM ");
    append(&p, end,     fdes -> table);
    append(&p, end, " WHERE ");
    append(&p, end,      fdes -> be -> column_escape);
    append(&p, end,      fdes -> keys[0].field -> name);
    append(&p, end,      fdes -> be -> column_escape);
    append(&p, end,        " = ");
    append_phold(fdes, &p, end, 0);

    l_debug("Statement is: %s", buf);

    type = fdes -> keys[0].field -> dt -> type;
    status = fdes -> be -> prepare_stmt(fdes -> conn, scoped,
            buf, 1, &type, &fdes -> stmts.delete_stmt);
    if (status != MBE_S_OK) {
        l_error("Failed to compile statement: [%s] -- error code: %d", buf, status);
        return status;
    }

    return MBE_S_OK;
}

static enum mbe_status prepare_lock_statement(struct wmbe_sql_file *fdes) {
    enum wmbe_sql_be_param_type type;
    char buf[1024];
    char *p, *end;
    char scoped[SCOPED_STMT_SIZE(fdes, PSN_LOCK)];
    enum mbe_status status;
    int count = 0;

    get_scoped_statement(scoped, sizeof(scoped), fdes, PSN_LOCK);

    l_debug("Preparing LOCK statement (%s)", scoped);

    p = buf;
    end = buf + sizeof(buf) - 1;
    end[1] = 0;

    append(&p, end, "SELECT ");
    for (int i = 0; i < fdes -> field_number; i++) {
        if (fdes -> fields[i].dt -> virt) {
            continue;
        }

        if (count) {
            append(&p, end, ", ");
        }
        append(&p, end, fdes -> be -> column_escape);
        append(&p, end, fdes -> fields[i].name);
        append(&p, end, fdes -> be -> column_escape);
        count++;
    }
    append(&p, end, " FROM ");
    append(&p, end,     fdes -> table);
    append(&p, end, " WHERE ");
    append(&p, end,      fdes -> be -> column_escape);
    append(&p, end,      fdes -> keys[0].field -> name);
    append(&p, end,      fdes -> be -> column_escape);
    append(&p, end,        " = ");
    append_phold(fdes, &p, end, 0);
    append(&p, end, " FOR UPDATE");

    l_debug("Statement is: %s", buf);

    type = fdes -> keys[0].field -> dt -> type;
    status = fdes -> be -> prepare_stmt(fdes -> conn, scoped,
                        buf, 1, &type, &fdes -> stmts.lock_stmt);
    if (status != MBE_S_OK) {
        l_error("Failed to compile statement: [%s] -- error code: %d", buf, status);
        return status;
    }

    return MBE_S_OK;
}

/*
 * 'op' is the SQL comparison operator WITHOUT the parameter placeholder,
 * e.g. " = ", " >= ", " <= ", " LIKE ".
 * The placeholder is appended via append_phold() and rendered as "$1"
 * (PostgreSQL) or "?" (MySQL) depending on the compile-time backend.
 * 'op_suffix' goes right after the placeholder (e.g. the ESCAPE clause of LIKE).
 */
/*
 * Builds one of the selects on a key:
 *
 *   SELECT <columns> FROM <table> WHERE <key> <op> ? [<op_suffix>] [AND <after last record>]
 *      ORDER BY <key>, <primary key> LIMIT ?
 *
 * The selects return a page of records, not all of them. When a page is over, the next one is
 * obtained with the "continuation" form of the select (cont != 0), that skips what we have seen
 * already: records are ordered by key and primary key, so the condition is "after (last key,
 * last primary key)". The parameters are:
 *
 *   first page:    the key, the limit
 *   continuation:  the key, what wlib_mbe_sql_cont_params() describes, the limit
 *
 * How to say "after" depends on the server: PostgreSQL is fast with a row comparison, but MariaDB
 * scans the index with it, and needs the two ranges spelled out. Both need an index on (key,
 * primary key) to be efficient: in InnoDB secondary indexes have it already.
 */
static enum mbe_status prepare_select_statement_be(struct wmbe_sql_file *fdes, struct wmbe_sql_key *key,
        const char *name, const char *op, const char *op_suffix, int asc, int cont, struct wmbe_sql_stmt **stmt) {

    enum wmbe_sql_be_param_type types[8];
    struct wmbe_sql_field *pk = fdes -> keys[0].field;
    int secondary = needs_pk_tiebreak(fdes, key);
    const char *esc = fdes -> be -> column_escape;
    const char *after = asc ? " > " : " < ";
    char sname[SCOPED_STMT_SIZE(fdes, name) + strlen(key -> field -> name) + 3];
    char buf[1024];
    char *p, *end;
    char scoped[SCOPED_STMT_SIZE(fdes, name)];
    enum mbe_status status;
    int count = 0;
    int params = 0;

    get_scoped_statement(scoped, sizeof(scoped), fdes, name);
    snprintf(sname, sizeof(sname), "%s%s%s%s", scoped, 
            fdes -> be -> column_escape, key -> field -> name, fdes -> be -> column_escape);

    l_debug("Preparing SELECT statement (%s)", sname);

    p = buf;
    end = buf + sizeof(buf) - 1;
    end[1] = 0;

    append(&p, end, "SELECT ");
    for (int i = 0; i < fdes -> field_number; i++) {
        if (fdes -> fields[i].dt -> virt) {
            continue;
        }

        if (count) {
            append(&p, end, ", ");
        }
        append(&p, end, fdes -> be -> column_escape);
        append(&p, end, fdes -> fields[i].name);
        append(&p, end, fdes -> be -> column_escape);
        count++;
    }
    append(&p, end,    " FROM ");
    append(&p, end,     fdes -> table);
    append(&p, end, " WHERE ");
    append(&p, end,      esc);
    append(&p, end,      key -> field -> name);
    append(&p, end,      esc);
    append(&p, end,        op);
    append_phold(fdes, &p, end, params);
    types[params++] = key -> field -> dt -> type;
    append(&p, end, op_suffix);

    if (cont) {
        append(&p, end, " AND ");
        if (!secondary) {
            /* The key is unique (primary, or declared so): the last key is enough */
            append(&p, end, esc);
            append(&p, end, key -> field -> name);
            append(&p, end, esc);
            append(&p, end, after);
            append_phold(fdes, &p, end, params);
            types[params++] = key -> field -> dt -> type;
        } else if (fdes -> be -> keyset_row_compare) {
            append(&p, end, "(");
            append(&p, end, esc);
            append(&p, end, key -> field -> name);
            append(&p, end, esc);
            append(&p, end, ", ");
            append(&p, end, esc);
            append(&p, end, pk -> name);
            append(&p, end, esc);
            append(&p, end, ")");
            append(&p, end, after);
            append(&p, end, "(");
            append_phold(fdes, &p, end, params);
            types[params++] = key -> field -> dt -> type;
            append(&p, end, ", ");
            append_phold(fdes, &p, end, params);
            types[params++] = pk -> dt -> type;
            append(&p, end, ")");
        } else {
            /* ((key = last key AND pk after last pk) OR key after last key) */
            append(&p, end, "((");
            append(&p, end, esc);
            append(&p, end, key -> field -> name);
            append(&p, end, esc);
            append(&p, end, " = ");
            append_phold(fdes, &p, end, params);
            types[params++] = key -> field -> dt -> type;
            append(&p, end, " AND ");
            append(&p, end, esc);
            append(&p, end, pk -> name);
            append(&p, end, esc);
            append(&p, end, after);
            append_phold(fdes, &p, end, params);
            types[params++] = pk -> dt -> type;
            append(&p, end, ") OR ");
            append(&p, end, esc);
            append(&p, end, key -> field -> name);
            append(&p, end, esc);
            append(&p, end, after);
            append_phold(fdes, &p, end, params);
            types[params++] = key -> field -> dt -> type;
            append(&p, end, ")");
        }
    }

    append(&p, end, " ORDER BY ");
    append(&p, end,      esc);
    append(&p, end,      key -> field -> name);
    append(&p, end,      esc);
    if (!asc) {
        append(&p, end,       " DESC");
    }
    if (secondary) {
        /* Records with the same secondary key are ordered by primary key, as in Enscribe.
         * Without this the server returns them in any order, and the scan is not repeatable */
        append(&p, end, ", ");
        append(&p, end,      esc);
        append(&p, end,      pk -> name);
        append(&p, end,      esc);
        if (!asc) {
            append(&p, end,   " DESC");
        }
    }
    append(&p, end, " LIMIT ");
    append_phold(fdes, &p, end, params);
    types[params++] = WMBE_SQL_BE_PT_INTEGER;

    l_debug("Statement is: %s", buf);

    status = fdes -> be -> prepare_stmt(fdes -> conn, sname, buf, params, types, stmt);
    if (status != MBE_S_OK) {
        l_error("Failed to compile statement: [%s] -- error code: %d", buf, status);
        return status;
    }

    return MBE_S_OK;
}

static enum mbe_status prepare_select_exact_statement(struct wmbe_sql_file *fdes, struct wmbe_sql_key *key) {
    return prepare_select_statement_be(fdes, key, PSN_SELECT_EXACT, " = ", "", 1, 0, &key -> stmts.select_exact_stmt);
}

static enum mbe_status prepare_select_approx_statement(struct wmbe_sql_file *fdes, struct wmbe_sql_key *key) {
    return prepare_select_statement_be(fdes, key, PSN_SELECT_APPROX, " >= ", "", 1, 0, &key -> stmts.select_approx_stmt);
}

static enum mbe_status prepare_select_gen_statement(struct wmbe_sql_file *fdes, struct wmbe_sql_key *key) {
    return prepare_select_statement_be(fdes, key, PSN_SELECT_GEN, " LIKE ", LIKE_ESCAPE_CLAUSE, 1, 0, &key -> stmts.select_gen_stmt);
}

static enum mbe_status prepare_select_approx_inv_statement(struct wmbe_sql_file *fdes, struct wmbe_sql_key *key) {
    return prepare_select_statement_be(fdes, key, PSN_SELECT_APPROX_INV, " <= ", "", 0, 0, &key -> stmts.select_approx_inv_stmt);
}

static enum mbe_status prepare_select_gen_inv_statement(struct wmbe_sql_file *fdes, struct wmbe_sql_key *key) {
    return prepare_select_statement_be(fdes, key, PSN_SELECT_GEN_INV, " LIKE ", LIKE_ESCAPE_CLAUSE, 0, 0, &key -> stmts.select_gen_inv_stmt);
}

/* The continuations are prepared when they are needed: most files never get to a second page */

static enum mbe_status prepare_cont_exact_statement(struct wmbe_sql_file *fdes, struct wmbe_sql_key *key) {
    return prepare_select_statement_be(fdes, key, PSN_CONT_EXACT, " = ", "", 1, 1, &key -> stmts.cont_exact_stmt);
}

static enum mbe_status prepare_cont_approx_statement(struct wmbe_sql_file *fdes, struct wmbe_sql_key *key) {
    return prepare_select_statement_be(fdes, key, PSN_CONT_APPROX, " >= ", "", 1, 1, &key -> stmts.cont_approx_stmt);
}

static enum mbe_status prepare_cont_gen_statement(struct wmbe_sql_file *fdes, struct wmbe_sql_key *key) {
    return prepare_select_statement_be(fdes, key, PSN_CONT_GEN, " LIKE ", LIKE_ESCAPE_CLAUSE, 1, 1, &key -> stmts.cont_gen_stmt);
}

static enum mbe_status prepare_cont_approx_inv_statement(struct wmbe_sql_file *fdes, struct wmbe_sql_key *key) {
    return prepare_select_statement_be(fdes, key, PSN_CONT_APPROX_INV, " <= ", "", 0, 1, &key -> stmts.cont_approx_inv_stmt);
}

static enum mbe_status prepare_cont_gen_inv_statement(struct wmbe_sql_file *fdes, struct wmbe_sql_key *key) {
    return prepare_select_statement_be(fdes, key, PSN_CONT_GEN_INV, " LIKE ", LIKE_ESCAPE_CLAUSE, 0, 1, &key -> stmts.cont_gen_inv_stmt);
}

/*
 * Returns the statement that gets the page after the last record of another one, for the kind
 * of positioning given. It is prepared the first time.
 */
enum mbe_status wlib_mbe_sql_get_cont_stmt(struct wmbe_sql_file *fdes, struct wmbe_sql_key *key,
        short positioning, int reverse, struct wmbe_sql_stmt **stmt) {
    struct wmbe_sql_stmt **slot;
    enum mbe_status (*prepare)(struct wmbe_sql_file *, struct wmbe_sql_key *);
    enum mbe_status status;

    switch (positioning) {
    case MBE_POS_EXACT:
        slot = &key -> stmts.cont_exact_stmt;
        prepare = prepare_cont_exact_statement;
        break;

    case MBE_POS_APPROX:
        slot = reverse ? &key -> stmts.cont_approx_inv_stmt : &key -> stmts.cont_approx_stmt;
        prepare = reverse ? prepare_cont_approx_inv_statement : prepare_cont_approx_statement;
        break;

    case MBE_POS_GENERIC:
        slot = reverse ? &key -> stmts.cont_gen_inv_stmt : &key -> stmts.cont_gen_stmt;
        prepare = reverse ? prepare_cont_gen_inv_statement : prepare_cont_gen_statement;
        break;

    default:
        return MBE_S_BAD_SPECS;
    }

    if (*slot == NULL) {
        status = prepare(fdes, key);
        if (status != MBE_S_OK) {
            return status;
        }
    }

    *stmt = *slot;

    return MBE_S_OK;
}

/*
 * Fills in the parameters of a continuation statement (the pointers are copied, not the strings):
 * the key it was run with, the key and primary key of the last record, and the limit. Returns how
 * many there are. values must have room for WMBE_SQL_MAX_CONT_PARAMS.
 */
int wlib_mbe_sql_cont_params(struct wmbe_sql_file *fdes, struct wmbe_sql_key *key, char *key_text,
        char *last_key, char *last_pk, char *limit, char **values) {
    int n = 0;

    values[n++] = key_text;
    if (!needs_pk_tiebreak(fdes, key)) {
        values[n++] = last_key;
    } else if (fdes -> be -> keyset_row_compare) {
        values[n++] = last_key;
        values[n++] = last_pk;
    } else {
        values[n++] = last_key;
        values[n++] = last_pk;
        values[n++] = last_key;
    }
    values[n++] = limit;

    return n;
}

enum mbe_status wlib_mbe_sql_prepare_file_stmts(struct wmbe_sql_file *fdes) {
    enum mbe_status status;

    status = prepare_insert_statement(fdes);
    if (status != MBE_S_OK) {
        return status;
    }

    status = prepare_update_statement(fdes);
    if (status != MBE_S_OK) {
        return status;
    }

    status = prepare_delete_statement(fdes);
    if (status != MBE_S_OK) {
        return status;
    }

    status = prepare_lock_statement(fdes);
    if (status != MBE_S_OK) {
        return status;
    }

    return MBE_S_OK;
}

enum mbe_status wlib_mbe_sql_prepare_key_stmts(struct wmbe_sql_file *fdes, struct wmbe_sql_key *key) {
    enum mbe_status status;

    status = prepare_select_exact_statement(fdes, key);
    if (status != MBE_S_OK) {
        return status;
    }

    status = prepare_select_approx_statement(fdes, key);
    if (status != MBE_S_OK) {
        return status;
    }

    status = prepare_select_approx_inv_statement(fdes, key);
    if (status != MBE_S_OK) {
        return status;
    }

    if (key -> field -> dt -> can_do_generic) {
        status = prepare_select_gen_statement(fdes, key);
        if (status != MBE_S_OK) {
            return status;
        }
        status = prepare_select_gen_inv_statement(fdes, key);
        if (status != MBE_S_OK) {
            return status;
        }
    }

    /* Continuations are never prepared here for the first time. But if there are any it's because
     * they've been used, and this is called again after losing the connection: they are gone too */
    if (key -> stmts.cont_exact_stmt != NULL) {
        status = prepare_cont_exact_statement(fdes, key);
        if (status != MBE_S_OK) {
            return status;
        }
    }
    if (key -> stmts.cont_approx_stmt != NULL) {
        status = prepare_cont_approx_statement(fdes, key);
        if (status != MBE_S_OK) {
            return status;
        }
    }
    if (key -> stmts.cont_approx_inv_stmt != NULL) {
        status = prepare_cont_approx_inv_statement(fdes, key);
        if (status != MBE_S_OK) {
            return status;
        }
    }
    if (key -> stmts.cont_gen_stmt != NULL) {
        status = prepare_cont_gen_statement(fdes, key);
        if (status != MBE_S_OK) {
            return status;
        }
    }
    if (key -> stmts.cont_gen_inv_stmt != NULL) {
        status = prepare_cont_gen_inv_statement(fdes, key);
        if (status != MBE_S_OK) {
            return status;
        }
    }

    return MBE_S_OK;
}

static void free_stmt(struct wmbe_sql_file *fdes, struct wmbe_sql_stmt *pstmt) {
    if (pstmt != NULL) {
        fdes -> be -> free_statement(pstmt);
    }
}

void wlib_mbe_sql_free_file_stmts(struct wmbe_sql_file *fdes) {
    free_stmt(fdes, fdes -> stmts.insert_stmt);
    free_stmt(fdes, fdes -> stmts.update_stmt);
    free_stmt(fdes, fdes -> stmts.delete_stmt);
    free_stmt(fdes, fdes -> stmts.lock_stmt);
}

void wlib_mbe_sql_free_key_stmts(struct wmbe_sql_file *fdes, struct wmbe_sql_key *key) {
    free_stmt(fdes, key -> stmts.select_exact_stmt);
    free_stmt(fdes, key -> stmts.select_approx_stmt);
    free_stmt(fdes, key -> stmts.select_gen_stmt);
    free_stmt(fdes, key -> stmts.select_approx_inv_stmt);
    free_stmt(fdes, key -> stmts.select_gen_inv_stmt);
    free_stmt(fdes, key -> stmts.cont_exact_stmt);
    free_stmt(fdes, key -> stmts.cont_approx_stmt);
    free_stmt(fdes, key -> stmts.cont_gen_stmt);
    free_stmt(fdes, key -> stmts.cont_approx_inv_stmt);
    free_stmt(fdes, key -> stmts.cont_gen_inv_stmt);
}

