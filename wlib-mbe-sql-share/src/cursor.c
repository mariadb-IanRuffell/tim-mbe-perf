/*
 * Cursor and key management. They are just a few functions, but handling these things requires
 * a bit of care.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define L_FEATURE L_FEAT_DATA
#include "wlib-internal/log.h"

#include "wlib/usrlib.h"
#include "wlib-mbe-sql/mbe-sql.h"
#include "wlib-mbe-sql/sql-be.h"

/*
 * Records are read a page at a time. The first page is small, because most of the times a few
 * records are all we need; then they grow, to go through a long file in few steps.
 */
#define FIRST_PAGE 8
#define PAGE_GROWTH 4
#define MAX_PAGE 1024

int wlib_mbe_sql_page_size;

static int first_page(void) {
    return wlib_mbe_sql_page_size > 0 ? wlib_mbe_sql_page_size : FIRST_PAGE;
}

static int next_page(int page) {
    if (wlib_mbe_sql_page_size > 0) {
        return wlib_mbe_sql_page_size;
    }

    return page * PAGE_GROWTH > MAX_PAGE ? MAX_PAGE : page * PAGE_GROWTH;
}

/* What we know of the position in the file, other than the key */
static void release_position(struct wmbe_key_specs *ksp) {
    free(ksp -> key_text);
    free(ksp -> last_key);
    free(ksp -> last_pk);
    ksp -> key_text = NULL;
    ksp -> last_key = NULL;
    ksp -> last_pk = NULL;
    ksp -> resume = 0;
    ksp -> consumed = 0;
}

enum mbe_status wlib_mbe_sql_cursor_set_specs(struct wmbe_sql_file *fdes, struct wmbe_sql_stmt *stmt,
    short keyspecs, short positioning, short options, short keylen, const void *key) {

    struct wmbe_key_specs *ksp = &fdes -> specs;    
    void *kdup;

    if (keylen) {
        kdup = malloc(keylen);
        if (kdup == NULL) {
            l_error("Failed to allocate memory for key");
            return MBE_S_OUT_OF_MEMORY;
        }
        memcpy(kdup, key, keylen);
    } else {
        kdup = NULL;
    }

    if (ksp -> key != NULL) {
        free(ksp -> key);
    }
    release_position(ksp);

    if (fdes -> cursor != NULL) {
        fdes -> be -> free_cursor(fdes -> cursor);
    }
    fdes -> cursor = NULL;

    ksp -> updated = 1;
    ksp -> stmt = stmt;
    ksp -> keyspecs = keyspecs;
    ksp -> positioning = positioning;
    ksp -> options = options;
    ksp -> keylen = keylen;
    ksp -> key = kdup;

    return MBE_S_OK;
}

/*
 * Runs the select that gets the page we need: the first one, or the one that follows the last
 * record that we have seen (if resume is set).
 */
enum mbe_status wlib_mbe_sql_cursor_update(struct wmbe_sql_file *fdes, int transactional, long timeout) {
    struct wmbe_key_specs *ksp = &fdes -> specs;
    struct wmbe_sql_key *key = &fdes -> keys[ksp -> keyspecs];
    struct wmbe_sql_cursor *cursor;
    struct wmbe_sql_stmt *stmt;
    enum mbe_status status;
    char *values[WMBE_SQL_MAX_CONT_PARAMS];
    char limit[24];
    int page;

    if (!fdes -> specs.updated) {
        return MBE_S_OK;
    }

    if (ksp -> key_text == NULL) {
        status = key -> field -> dt 
            -> serialize(ksp -> key, ksp -> keylen, ksp -> positioning == MBE_POS_GENERIC, &ksp -> key_text);
        if (status != MBE_S_OK) {
            ksp -> key_text = NULL;
            l_error("Key serialization failed. Status = %d", status);
            return status;
        }
    }

    page = ksp -> resume ? ksp -> next_limit : first_page();
    snprintf(limit, sizeof(limit), "%d", page);

    if (ksp -> resume) {
        status = wlib_mbe_sql_get_cont_stmt(fdes, key, ksp -> positioning, ksp -> options & WMBE_SQL_OPT_REVERSE, &stmt);
        if (status != MBE_S_OK) {
            l_error("Failed to prepare the SELECT of the next page. Status = %d", status);
            return status;
        }
        wlib_mbe_sql_cont_params(fdes, key, ksp -> key_text, ksp -> last_key, ksp -> last_pk, limit, values);
    } else {
        stmt = ksp -> stmt;
        values[0] = ksp -> key_text;
        values[1] = limit;
    }

    l_debug("Running SELECT statement");
    status = wlib_mbe_sql_fdes_exec(fdes, stmt, values, &cursor, transactional, timeout);
    if (status != MBE_S_OK) {
        l_error("SELECT query failed failed. Status = %d", status);
        return status;
    }

    l_debug("SELECT statement completed successfully");
    if (fdes -> cursor != NULL) {
        l_warn("Cursor is valid when it should not be");
        fdes -> be -> free_cursor(fdes -> cursor);
    }

    fdes -> specs.updated = 0;
    ksp -> resume = 0;
    ksp -> page_limit = page;
    ksp -> next_limit = next_page(page);
    fdes -> cursor = cursor;

    return MBE_S_OK;
}

/* Remembers where the current record is, to go on from it with the next page */
static enum mbe_status save_last_record(struct wmbe_sql_file *fdes) {
    struct wmbe_key_specs *ksp = &fdes -> specs;
    char key[WMBE_SQL_VALUE_BUFLEN];
    char pk[WMBE_SQL_VALUE_BUFLEN];
    enum mbe_status status;
    char *new_key, *new_pk;

    status = fdes -> be -> get(fdes -> cursor, fdes -> keys[ksp -> keyspecs].field -> param_pos, key, sizeof(key));
    if (status != MBE_S_OK) {
        return status;
    }
    status = fdes -> be -> get(fdes -> cursor, fdes -> keys[0].field -> param_pos, pk, sizeof(pk));
    if (status != MBE_S_OK) {
        return status;
    }

    new_key = strdup(key);
    new_pk = strdup(pk);
    if (new_key == NULL || new_pk == NULL) {
        free(new_key);
        free(new_pk);
        return MBE_S_OUT_OF_MEMORY;
    }

    free(ksp -> last_key);
    free(ksp -> last_pk);
    ksp -> last_key = new_key;
    ksp -> last_pk = new_pk;

    return MBE_S_OK;
}

/*
 * Moves the cursor to the next record. *valid tells if it is on a record now (otherwise there
 * are no more). Callers must use this and not be -> next(), because the cursor may need to go
 * back to the database to know.
 *
 * If it can't get the next page the cursor is left without records, and the next one to read
 * will try again to get it: the position is not lost.
 */
enum mbe_status wlib_mbe_sql_cursor_next(struct wmbe_sql_file *fdes, int transactional, long timeout, int *valid) {
    struct wmbe_key_specs *ksp = &fdes -> specs;
    struct wmbe_sql_cursor *cursor = fdes -> cursor;
    enum mbe_status status;

    if (cursor == NULL) {
        *valid = 0;
        return MBE_S_OK;
    }

    /* A page that is not full is the last one. This one is, and we are on its last record */
    if (fdes -> be -> rows(cursor) == ksp -> page_limit
        && fdes -> be -> position(cursor) == ksp -> page_limit - 1) {

        status = save_last_record(fdes);
        if (status != MBE_S_OK) {
            return status;
        }

        /* The old page must go first: with some backends the new one invalidates it */
        fdes -> be -> free_cursor(cursor);
        fdes -> cursor = NULL;
        ksp -> resume = 1;
        ksp -> updated = 1;

        status = wlib_mbe_sql_cursor_update(fdes, transactional, timeout);
        if (status != MBE_S_OK) {
            return status;
        }

        *valid = !fdes -> be -> eof(fdes -> cursor);

        return MBE_S_OK;
    }

    *valid = fdes -> be -> next(cursor);

    return MBE_S_OK;
}

/*
 * The value given to SET_KEY, as the primary key of a record (serialized). It makes sense
 * only if the key is the primary one.
 */
enum mbe_status wlib_mbe_sql_cursor_key_value(struct wmbe_sql_file *fdes, char *key, int keylen) {
    struct wmbe_sql_field *field = fdes -> keys[0].field;
    enum mbe_status status;
    char *value;

    if (field -> size > keylen) {
        l_error("Key is too large");
        return MBE_S_IO_ERROR;
    }

    /* specs.key holds the key value itself (not a record), so there's no field offset to add */
    status = field -> dt -> serialize(fdes -> specs.key, fdes -> specs.keylen, 0, &value);
    if (status != MBE_S_OK) {
        return status;
    }
    strncpy(key, value, keylen);
    free(value);

    return MBE_S_OK;
}

/*
 * Fetches the current primary key as efficiently as possible. It is in serialized form.
 */
enum mbe_status wlib_mbe_sql_cursor_get_primary_key(const char *fop, struct wmbe_sql_file *fdes, char *key, int keylen, 
        int *is_exact, int transactional, long timeout) {
    enum mbe_status status;

    l_debug("%s: retriving primary key", fop);
    if (is_exact != NULL) {
        *is_exact = fdes -> specs.positioning == MBE_POS_EXACT;
    }

    /* If the positioning is exact on the primary key, we already have the key,
     * so we avoid a useless SELECT. This use case is pretty frequent. */
    if (fdes -> specs.keyspecs == 0 && fdes -> specs.positioning == MBE_POS_EXACT) {
        l_debug("Key is known from previous SET_KEY");
        return wlib_mbe_sql_cursor_key_value(fdes, key, keylen);
    }

    l_debug("Need to use SELECT to fetch the key");

    /* Need to retrieve it from the database */
    status = wlib_mbe_sql_cursor_update(fdes, transactional,timeout);
    if (status != MBE_S_OK) {
        return status;
    }

    if (fdes -> cursor == NULL) {
        l_error("%s: called without prior MBE_FILESETKEY", fop);
        return MBE_S_INVALID_KEY;
    }

    if (fdes -> be -> eof(fdes -> cursor)) {
        l_debug("%s: on a record that does not exist", fop);
        return MBE_S_RECORD_NOT_FOUND;
    }

    status = fdes -> be -> get(fdes -> cursor, fdes -> keys[0].field -> param_pos, key, keylen);
    if (status != MBE_S_OK) {
        l_error("%s: Failed to fetch current key", fop);
        return status;
    }    

    return MBE_S_OK;
}

void wmbe_mbe_sql_cursor_reset(struct wmbe_sql_file *fdes) {
    if (fdes -> cursor != NULL) {
        fdes -> be -> free_cursor(fdes -> cursor);
        fdes -> cursor = NULL;
    }

    fdes -> specs.updated = 0;
    if (fdes -> specs.key != NULL) {
        free(fdes -> specs.key);
        fdes -> specs.key = NULL;
    }
    release_position(&fdes -> specs);
}

int wmbe_mbe_sql_cursor_is_set(struct wmbe_sql_file *fdes, int *just_set) {
    if (just_set != NULL) {
        *just_set = fdes -> specs.updated;
    }

    return fdes -> specs.updated || fdes -> cursor != NULL;
}
