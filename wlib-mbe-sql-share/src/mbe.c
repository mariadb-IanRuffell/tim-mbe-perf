/*
 * A simple implementation of MBE calls, using an SQL database.
 */

#include <sys/fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>

#include "wlib-internal/io.h"
#include "wlib-internal/fio.h"
#include "wlib-internal/status.h"
#include "wlib/usrlib.h"

#define L_FEATURE L_FEAT_DATA

#include "wlib-internal/fio.h"
#include "wlib-internal/constructors.h"
#include "wlib-internal/log.h"

#include "wlib-mbe-sql/mbe-sql.h"
#include "wlib-mbe-sql/sql-be.h"

#define OPT_REVERSE WMBE_SQL_OPT_REVERSE
#define OPT_POS_TO_LAST 0x2000

#define EXTENSION ".mbe.ini"

static int can_open (const char *fname);
static enum wl_status do_open (const char *fname, int *fd, 
                int open_flags, short sync_or_recv_depth, int nowait_depth,
                enum fio_access, int exclusion);

static int complete_operation(struct io_descriptor *iod, struct io_operation *iop, long timeout);
static void release_subsys_data(struct io_descriptor *iod);

static struct fio_backend_ifc fifc = { can_open, do_open };

static struct io_subsystem_ifc ifc = {
    .type = IO_T_FILE_IO,
    .complete_operation = complete_operation,
    .release_subsys_data = release_subsys_data
};

/* Reports the outcome. The count of bytes transferred is the one tracked by the operation
 * (see advance_transferred), and 0 if it failed: the caller must not trust the buffer */
static int set_complete(struct io_operation *iop, int result) {
    int transferred = result == MBE_S_OK ? iop -> u.in_progress_info.transferred : 0;

    iop -> completed = 1;
    /* Don't access in_progress_info below this line */
    iop -> u.completed_info.result = result;
    iop -> u.completed_info.transferred = transferred;

    return result;
}

/* Accounts for the field as transferred, if the caller's buffer holds it entirely. Fillers
 * count too: they are part of the record even if they have no column */
static void advance_transferred(struct io_operation *iop, const struct wmbe_sql_field *field) {
    unsigned last_byte = field -> ofs + field -> size;

    if (last_byte <= iop -> u.in_progress_info.buffer_size
        && iop -> u.in_progress_info.transferred < last_byte) {
        iop -> u.in_progress_info.transferred = last_byte;
    }
}

static int s(struct io_descriptor *iod, int result) {
    return iod -> last_error = result;
}

static enum mbe_status safe_serialize(struct io_operation *iop, struct wmbe_sql_field *field, char **dest) {

    enum mbe_status status;
    int last_byte;

    l_debug("Serializing field %s", field -> name);
    last_byte = field -> ofs + field -> size;
    if (last_byte > iop -> u.in_progress_info.buffer_size) {
        l_error("Written data is too short: got %d, need at least %d", 
            iop -> u.in_progress_info.buffer_size, last_byte);
        return MBE_S_BAD_SPECS;
    }

    l_debug("Size is ok. Let's call the dt");
    status = field -> dt -> serialize(iop -> buffer + field -> ofs, field -> size, 0, dest);
    if (status != MBE_S_OK) {
        l_error("Failed to serialize %s", field -> name);
        return status;
    }

    advance_transferred(iop, field);

    l_debug("Serialization done");

    return MBE_S_OK;
}

static void free_values(char **values, int count) {
    for (int i = 0; i < count; i++) {
        if (values[i] != NULL) {
            free(values[i]);
        }
    }

    free(values);
}

static int can_open (const char *fname) {
    return strlen(fname) >= strlen(EXTENSION) 
        && !strcasecmp(fname + strlen(fname) - strlen(EXTENSION), EXTENSION);
}

static int allocate_fd() {
    return open("/dev/zero", O_RDWR);
}

static enum wl_status do_open(const char *fname, int *fdp, 
                int open_flags, short sync_or_recv_depth, int nowait_depth,
                enum fio_access access, int exclusion)  {

    struct io_descriptor *iod;
    struct wmbe_sql_file *fdes;
    int fd;
    enum wl_status res;

    l_debug("Opening file <%s>", fname);
    fd = allocate_fd();
    if (fd < 0) {
        l_error("Failed to allocate file descriptor: %s", strerror(errno));
        return WL_STATUS_OUT_OF_MEMORY;
    }

    res = wlib_mbe_sql_fdes_open(fname, access, exclusion, &fdes);
    if (res != WL_STATUS_OK) {
        close(fd);
        return res;
    }

    iod = io_allocate(fd, open_flags, nowait_depth, &ifc);
    iod -> subsys_data = fdes;
    iod -> clazz = IO_C_MBE;

    *fdp = fd;

    return WL_STATUS_OK;
}

static void release_subsys_data(struct io_descriptor *iod) {
    wlib_mbe_sql_fdes_close(iod -> subsys_data);
}

static short setkey(struct io_descriptor *iod, const char *key, short keylen, 
        short keyspec, short positioning, short options, short compare_length) {

    struct wmbe_sql_file *fdes;
    struct wmbe_sql_stmt *stmt;
    enum mbe_status status;

    fdes = iod -> subsys_data;

    l_debug("Validating parameters: keylen=%d, keyspec=%d, positioning=%d, options=%xh, compare_length=%d",
        keylen, keyspec, positioning, options, compare_length);
    if (keyspec < 0 || keyspec >= fdes -> field_number 
        || fdes -> keys[keyspec].field == NULL) {
        l_error("MBE_FILE_SETKEY_ invoked with bad key_spec (%d)", keyspec);
        return s(iod, MBE_S_INVALID_KEY);
    }

    if (keylen > fdes -> keys[keyspec].field -> size) {
        l_error("MBE_FILE_SETKEY_ keylen (%d) does not match the size of field %d (%s)", 
                keylen, fdes -> keys[keyspec].field -> size, fdes -> keys[keyspec].field -> name);
        return s(iod, MBE_S_BAD_COUNT);
    }

    if (compare_length < 0) {
        compare_length = keylen;
    } else if (compare_length > keylen) {
        l_error("MBE_FILE_SETKEY_ called with compare_length (%d) > keylen (%d)", compare_length, keylen);
        return s(iod, MBE_S_BAD_COUNT);
    }

    if ((options & ~(OPT_REVERSE | OPT_POS_TO_LAST)) != 0) {
        l_error("MBE_FILE_SETKEY_ called with options=%xh, which is not supported", options);
        return s(iod, MBE_S_BAD_SPECS);
    }

    if (positioning == MBE_POS_GENERIC && !fdes -> keys[keyspec].field -> dt -> can_do_generic) {
        l_error("Positioning of type GENERIC is not supported for datatype %s", fdes -> keys[keyspec].field -> dt -> name);
        return s(iod, MBE_S_BAD_SPECS);
    }

    /* Note that POS_TO_LAST is ignored/implied */
    switch (positioning) {
    case MBE_POS_EXACT:
        stmt = fdes -> keys[keyspec].stmts.select_exact_stmt;
        break;

    case MBE_POS_APPROX:
        stmt = options & OPT_REVERSE ? fdes -> keys[keyspec].stmts.select_approx_inv_stmt : fdes -> keys[keyspec].stmts.select_approx_stmt;
        break;

    case MBE_POS_GENERIC:
        stmt = options & OPT_REVERSE ? fdes -> keys[keyspec].stmts.select_gen_inv_stmt : fdes -> keys[keyspec].stmts.select_gen_stmt;
        break;

    default:
        l_error("MBE_FILE_SETKEY_ called with unsupported positioning: %d", positioning);
        return s(iod, MBE_S_BAD_SPECS);
    }

    l_debug("Parameters ok. Creating specs structure");
    status = wlib_mbe_sql_cursor_set_specs(fdes, stmt, keyspec, positioning, options, 
            positioning == MBE_POS_APPROX ? keylen : compare_length, key);
    if (status != MBE_S_OK) {
        l_error("Specs creation failed. Status = %d", status);
        return s(iod, status);
    }

    l_debug("All done. Specs are ready");

    return s(iod, MBE_S_OK);
}

short _fFILE_SETKEY_(short iofd, const char *key, short keylen, 
        short keyspec, short positioning, short options, short compare_length) {

    return setkey(io_get_iod(iofd), key, keylen, keyspec, positioning, options, compare_length);
}

/* Runs the lock statement. On MBE_S_OK, *cursor is a non-empty result owned by the caller.
 * An empty result is released here and reported as MBE_S_RECORD_NOT_FOUND. */
static enum mbe_status exec_lock_stmt(struct wmbe_sql_file *fdes, const char *primary_key,
        struct wmbe_sql_cursor **cursor, long timeout) {
    char *values[1] = { (char *) primary_key };
    enum mbe_status status;

    status = wlib_mbe_sql_fdes_exec(fdes, fdes -> stmts.lock_stmt, values, cursor, 1, timeout);
    if (status != MBE_S_OK) {
        return status;
    }

    if (fdes -> be -> eof(*cursor)) {
        fdes -> be -> free_cursor(*cursor);
        *cursor = NULL;
        return MBE_S_RECORD_NOT_FOUND;
    }

    return MBE_S_OK;
}

/* known_key is the primary key of the record to lock, if the caller has it (NULL: it is the one of the current record) */
static int lock_record(const char *fop, struct wmbe_sql_file *fdes, struct wmbe_sql_cursor **cursor, long timeout,
        const char *known_key) {
    enum mbe_status status;
    char primary_key[WMBE_SQL_VALUE_BUFLEN];
    int is_exact;

    *cursor = NULL;

    status = wlib_mbe_sql_fdes_lock(fdes);
    if (status != MBE_S_OK) {
        return status;
    }

    if (known_key != NULL) {
        strcpy(primary_key, known_key);
        is_exact = 1;
        status = MBE_S_OK;
    } else {
        status = wlib_mbe_sql_cursor_get_primary_key(fop, fdes, primary_key, sizeof(primary_key), &is_exact, 1, timeout);
    }
    if (status != MBE_S_OK) {
        wlib_mbe_sql_fdes_unlock(fdes);
        return status;
    }

    status = exec_lock_stmt(fdes, primary_key, cursor, timeout);
    if (status == MBE_S_OK) {
        return MBE_S_OK;
    }
    if (status != MBE_S_RECORD_NOT_FOUND) {
        /* A real error (timeout, I/O...): don't mistake it for a vanished record */
        wlib_mbe_sql_fdes_unlock(fdes);
        return status;
    }

    l_debug("record disappeared: let's see if there are other eilgible records");
    if (is_exact) {
        l_debug("no more records: positioning was exact");
        wlib_mbe_sql_fdes_unlock(fdes);
        return MBE_S_RECORD_NOT_FOUND;
    }

    /* Positioning is not exact, so there's a chance we may find another eligible record.
     * Note that currently get_primary_key already updates the cursor when it is not exact, 
     * but this may change in the future. Calling this function twice is not expensive */
    status = wlib_mbe_sql_cursor_update(fdes, 1, timeout);
    if (status != MBE_S_OK) {
        wlib_mbe_sql_fdes_unlock(fdes);
        return status;
    }

    status = MBE_S_RECORD_NOT_FOUND;
    for (;;) {
        int valid;

        status = wlib_mbe_sql_cursor_next(fdes, 1, timeout, &valid);
        if (status != MBE_S_OK) {
            break;
        }
        if (!valid) {
            status = MBE_S_RECORD_NOT_FOUND;
            break;
        }

        status = fdes -> be -> get(fdes -> cursor, fdes -> keys[0].field -> param_pos, primary_key, sizeof(primary_key));
        if (status != MBE_S_OK) {
            break;
        }
        status = exec_lock_stmt(fdes, primary_key, cursor, timeout);
        if (status != MBE_S_RECORD_NOT_FOUND) {
            break;      /* locked, or a real error */
        }
        l_debug("record disappeared: advance the cursor");
    }

    if (status != MBE_S_OK) {
        wlib_mbe_sql_fdes_unlock(fdes);
    }

    return status;
}

static void cleanup_lock_cursor(struct wmbe_sql_file *fdes, struct wmbe_sql_cursor *cursor) {
    if (cursor != fdes -> cursor) {
        fdes -> be -> free_cursor(cursor);
    }
}        

static enum mbe_status mbe_rewind(struct wmbe_sql_file *fdes) {

    enum mbe_status status;
    char primary_key[fdes -> keys[0].field -> size];

    memset(primary_key, 0, sizeof(primary_key));

    status = wlib_mbe_sql_cursor_set_specs(fdes, fdes -> keys[0].stmts.select_approx_stmt,
        0, MBE_POS_APPROX, 0, sizeof(primary_key), primary_key);
    if (status != MBE_S_OK) {
        return status;
    }

    return MBE_S_OK;
}

/* Reads the record with this primary key, without touching the position */
static enum mbe_status select_by_key(struct wmbe_sql_file *fdes, const char *primary_key, long timeout,
        struct wmbe_sql_cursor **cursor) {
    char *values[2] = { (char *) primary_key, "1" };

    return wlib_mbe_sql_fdes_exec(fdes, fdes -> keys[0].stmts.select_exact_stmt, values, cursor, 0, timeout);
}

static int read_record(const char *fop, struct wmbe_sql_file *fdes, struct io_descriptor *iod, struct io_operation *iop, long timeout,
        int next_record, int lock) {
    struct wmbe_sql_cursor *cursor;
    enum mbe_status status;
    int just_set;
    int by_key = 0;
    char buf[WMBE_SQL_VALUE_BUFLEN];
    char key_value[WMBE_SQL_VALUE_BUFLEN];

    l_debug("%s on fd %s", fop, fdes -> table);
    if (!wmbe_mbe_sql_cursor_is_set(fdes, &just_set)) {
        /* READUPDATEX requires a key to be set */
        if (!next_record) {
            l_error("%s called without prior MBE_FILESETKEY", fop);
            return set_complete(iop, MBE_S_RECORD_NOT_FOUND);
        }
        /* READ simply starts from the beginning */
        status = mbe_rewind(fdes);
        if (status != MBE_S_OK) {
            return set_complete(iop, status);
        }
        // The cursor state does not reflect the specs any more (well.. it's NULL)
        just_set = 1;
    }

    /* A read with a lock on an exact primary key doesn't need the cursor, so nothing tells that
     * the record has been read already: that is what this is for. Reads that go through the cursor
     * run out of records by themselves */
    if (next_record && fdes -> specs.consumed) {
        l_debug("%s: the record of an exact key has been read already", fop);
        return set_complete(iop, MBE_S_RECORD_NOT_FOUND);
    }

    /* READUPDATE right after SET_KEY has no record to read again. Enscribe takes the key as the
     * one of the record, whatever the positioning, and leaves the position where it is. It
     * can't do that for alternate keys, which are not unique */
    if (!next_record && just_set && !fdes -> specs.resume) {
        if (fdes -> specs.keyspecs != 0) {
            l_error("%s: needs a record read before when the key is not the primary one", fop);
            return set_complete(iop, MBE_S_INVALID_KEY);
        }
        by_key = fdes -> specs.positioning != MBE_POS_EXACT;
        if (by_key) {
            status = wlib_mbe_sql_cursor_key_value(fdes, key_value, sizeof(key_value));
            if (status != MBE_S_OK) {
                return set_complete(iop, status);
            }
        }
    }

    /* If the last operation was not a SET_KEY, let's advance the cursor.
     * This apply to MBE_READX only: READUPDATE does not work this way */
    if (next_record && !just_set) {
        int valid;

        l_debug("%s: key not set, so we advance the cursor", fop);
        status = wlib_mbe_sql_cursor_next(fdes, lock, timeout, &valid);
        if (status != MBE_S_OK) {
            l_error("%s: failed to advance the cursor. Error code: %d", fop, status);
            return set_complete(iop, status);
        }
    }

    if (lock) {
        status = lock_record(fop, fdes, &cursor, timeout, by_key ? key_value : NULL);
        if (status != MBE_S_OK) {
            l_debug("%s: failed to lock cursor. Error code: %d", fop, status);
            return set_complete(iop, status);
        }
        if (next_record && fdes -> specs.updated) {
            /* The key is still waiting for its select: the record was found without it */
            fdes -> specs.consumed = 1;
        }
    } else if (by_key) {
        status = select_by_key(fdes, key_value, timeout, &cursor);
        if (status != MBE_S_OK) {
            l_error("%s: failed to read the record. Error code: %d", fop, status);
            return set_complete(iop, status);
        }
    } else {
        status = wlib_mbe_sql_cursor_update(fdes, lock, timeout);
        if (status != MBE_S_OK) {
            l_error("%s: failed to open cursor. Error code: %d", fop, status);
            return set_complete(iop, status);
        }
        cursor = fdes -> cursor;
    }

    /* --- Make sure to call cleanup_lock_cursor before exiting below this line ---- */

    if (fdes -> be -> eof(cursor)) {
        l_debug("%s on a record that does not exist", fop);
        cleanup_lock_cursor(fdes, cursor);
        return set_complete(iop, MBE_S_RECORD_NOT_FOUND);
    }

    for (int i = 0; i < fdes -> field_number; i++) {
        struct wmbe_sql_field *field = &fdes -> fields[i];
        unsigned buffer_size = iop -> u.in_progress_info.buffer_size;
        unsigned last_byte = field -> ofs + field -> size;
        char *dest = iop -> buffer + field -> ofs;
        char *scratch = NULL;

        if (field -> ofs >= buffer_size) {
            break;
        }

        if (!field -> dt -> virt) {
            status = fdes -> be -> get(cursor, field -> param_pos, buf, sizeof(buf));
            if (status != MBE_S_OK) {
                cleanup_lock_cursor(fdes, cursor);
                return set_complete(iop, status);
            }
        }

        /* A field that doesn't fit entirely is cut: like Enscribe, we give as many bytes as there are room for */
        if (last_byte > buffer_size) {
            scratch = malloc(field -> size);
            if (scratch == NULL) {
                cleanup_lock_cursor(fdes, cursor);
                return set_complete(iop, MBE_S_OUT_OF_MEMORY);
            }
            dest = scratch;
        }

        status = field -> dt -> deserialize(buf, dest, field -> size);
        if (scratch != NULL) {
            if (status == MBE_S_OK) {
                memcpy(iop -> buffer + field -> ofs, scratch, buffer_size - field -> ofs);
            }
            free(scratch);
        }
        if (status != MBE_S_OK) {
            cleanup_lock_cursor(fdes, cursor);
            return set_complete(iop, status);
        }
    }

    /* The whole record, or the part of it that fits in the buffer */
    iop -> u.in_progress_info.transferred = iop -> u.in_progress_info.buffer_size < fdes -> record_size
        ? iop -> u.in_progress_info.buffer_size : fdes -> record_size;

    cleanup_lock_cursor(fdes, cursor);

    return set_complete(iop, MBE_S_OK);
}

static int complete_read_update_operation(struct wmbe_sql_file *fdes, struct io_descriptor *iod, struct io_operation *iop, long timeout, int lock) {
    return read_record("MBE_READUPDATEX", fdes, iod, iop, timeout, 0, lock);
}

static int complete_lock_operation(struct wmbe_sql_file *fdes, struct io_descriptor *iod, struct io_operation *iop, long timeout) {
    struct wmbe_sql_cursor *cursor;
    enum mbe_status res;

    l_debug("Locking current record" );

    if (!wmbe_mbe_sql_cursor_is_set(fdes, NULL)) {
        l_debug("MBE_LOCK_RECORD called without prior MBE_FILESETKEY. Rewind");
        res = mbe_rewind(fdes);
        if (res != MBE_S_OK) {
            return set_complete(iop, res);
        }
    }

    res = lock_record("MBE_LOCK_RECORD", fdes, &cursor, timeout, NULL);
    if (res != MBE_S_OK) {
        l_error("Failed to lock record: %d", res);
        return set_complete(iop, res);
    }

    fdes -> be -> free_cursor(cursor);

    l_debug("Record successfully locked" );

    return set_complete(iop, MBE_S_OK);
}

static int complete_read_operation(struct wmbe_sql_file *fdes, struct io_descriptor *iod, struct io_operation *iop, long timeout, int lock) {
    int res;

    res = read_record("MBE_READX", fdes, iod, iop, timeout, 1, lock);
    if (res == MBE_S_RECORD_NOT_FOUND) {
        iop -> u.completed_info.result = res = MBE_S_WARN;
    }

    return res;
}

/* Writes take the record as a whole: any other count is an error, like in Enscribe */
static int bad_write_count(struct wmbe_sql_file *fdes, struct io_operation *iop) {
    if (iop -> u.in_progress_info.buffer_size == fdes -> record_size) {
        return 0;
    }

    l_error("Bad count for a write: got %d, the record is %d bytes long",
        iop -> u.in_progress_info.buffer_size, fdes -> record_size);

    return 1;
}

static int complete_write_operation(struct wmbe_sql_file *fdes, struct io_descriptor *iod, struct io_operation *iop, long timeout) {
    struct wmbe_sql_cursor *cursor;
    enum mbe_status status;
    char **values;
    int count;

    if (bad_write_count(fdes, iop)) {
        return set_complete(iop, MBE_S_BAD_COUNT);
    }

    count = 0;
    values = calloc(fdes -> field_number, sizeof(char *));
    if (values == NULL) {
        l_error("Failed to allocate memory for actual parameters");
        return set_complete(iop, MBE_S_OUT_OF_MEMORY);
    }
    for (int i = 0; i < fdes -> field_number; i++) {
        struct wmbe_sql_field *field = &fdes -> fields[i];

        if (field -> dt -> virt) {
            advance_transferred(iop, field);
            continue;
        }
        /* Rejects records that are too short instead of reading past the caller's buffer */
        status = safe_serialize(iop, field, &values[count]);
        if (status != MBE_S_OK) {
            free_values(values, count);
            return set_complete(iop, status);
        }
        count++;
    }

    status = wlib_mbe_sql_fdes_exec(fdes, fdes -> stmts.insert_stmt, values, &cursor, 0, timeout);
    free_values(values, count);
    if (status != MBE_S_OK) {
        return set_complete(iop, status);
    }

    fdes -> be -> free_cursor(cursor);

    return set_complete(iop, MBE_S_OK);
}

static int complete_write_update_operation(struct wmbe_sql_file *fdes, struct io_descriptor *iod, struct io_operation *iop, long timeout, int inside_lock) {
    struct wmbe_sql_stmt *stmt;
    struct wmbe_sql_cursor *cursor;
    enum mbe_status status;
    char primary_key[WMBE_SQL_VALUE_BUFLEN];
    char **values;
    int is_primary;
    int is_delete;
    int count;

    is_primary = fdes -> specs.keyspecs == 0;
    is_delete = iop -> u.in_progress_info.buffer_size == 0;

    l_debug("MBE_WRITEDUPDATEX on fd %s", fdes -> table);
    if (!is_delete && bad_write_count(fdes, iop)) {
        return set_complete(iop, MBE_S_BAD_COUNT);
    }

    status = wlib_mbe_sql_cursor_get_primary_key("MBE_WRITEDUPDATEX", fdes, primary_key, sizeof(primary_key), NULL, inside_lock, timeout);
    /* Enscribe compatibility status code adjustment */
    if (status == MBE_S_RECORD_NOT_FOUND && !is_primary) {
        status = MBE_S_INVALID_KEY;
    } else if (status == MBE_S_INVALID_KEY && is_primary && is_delete) {
        status = MBE_S_RECORD_NOT_FOUND;
    }
    if (status != MBE_S_OK) {
        return set_complete(iop, status);
    }

    values = calloc(fdes -> field_number + 1, sizeof (char *));
    if (values == NULL) {
        l_error("Failed to allocate memory for actual parameters");
        return set_complete(iop, MBE_S_OUT_OF_MEMORY);
    }

    count = 0;
    status = MBE_S_OK;
    if (!is_delete) {

        stmt = fdes -> stmts.update_stmt;
        for (int i = 0; i < fdes -> field_number; i++) {
            if (fdes -> fields[i].dt -> virt) {
                advance_transferred(iop, &fdes -> fields[i]);
                continue;
            }

            status = safe_serialize(iop, &fdes -> fields[i], &values[count]);
            if (status != MBE_S_OK) {
                l_error("Failed to serialize %s.%s", fdes -> table, fdes -> fields[i].name);
                break;
            }

            count++;
        }
    } else {
        stmt = fdes -> stmts.delete_stmt;
    }

    /* key */
    values[count] = primary_key;    // Don't increment count, otherwise free_value will try to free it
    if (status != MBE_S_OK) {
        free_values(values, count);
        return set_complete(iop, status);
    }
    if (inside_lock && fdes -> be -> commit_next != NULL) {
        /* The lock is released after this: the backend may commit along with the statement */
        fdes -> be -> commit_next(fdes -> conn);
    }
    status = wlib_mbe_sql_fdes_exec(fdes, stmt, values, &cursor, inside_lock, timeout);
    free_values(values, count);
    if (status != MBE_S_OK) {
        l_error("UPDATE/DELETE failed, code = %d", status);
        return set_complete(iop, status);
    }
    if (fdes -> be -> affected_rows(cursor) != 1) {
        /* I prefer to log the number of affected rows in case something went really wrong */
        l_error("UPDATE/DELETE on a record that does not exist (affected rows: %d)",
            fdes -> be -> affected_rows(cursor));
        fdes -> be -> free_cursor(cursor);
        return set_complete(iop, MBE_S_RECORD_NOT_FOUND);
    }

    l_debug("UPDATE/DELETE ok");
    fdes -> be -> free_cursor(cursor);

    return set_complete(iop, MBE_S_OK);
}

static int complete_operation(struct io_descriptor *iod, struct io_operation *iop, long timeout) {

    struct wmbe_sql_file *fdes;
    int lock, unlock;
    int res;

    fdes = iod -> subsys_data;

    /* Operations account for the bytes they transfer here, see set_complete() */
    iop -> u.in_progress_info.transferred = 0;

    lock = FIO_IS_LOCKED(iop -> type);
    unlock = FIO_IS_UNLOCKED(iop -> type);
    
    switch (FIO_MODULO_LOCK(iop -> type)) {
    case FIO_OP_READ:
        res = complete_read_operation(fdes, iod, iop, timeout, lock);
        break;

    case FIO_OP_READ_UPDATE:
        res = complete_read_update_operation(fdes, iod, iop, timeout, lock);
        break;

    case FIO_OP_WRITE:
        res = complete_write_operation(fdes, iod, iop, timeout);
        break;

    case FIO_OP_WRITE_UPDATE:
        res = complete_write_update_operation(fdes, iod, iop, timeout, unlock);
        break;

    case FIO_OP_LOCK_RECORD:
        res = complete_lock_operation(fdes, iod, iop, timeout);
        break;

    case FIO_OP_UNLOCK_RECORD:
        unlock = 1;
        res = set_complete(iop, MBE_S_OK);
        break;

    default:
        res = set_complete(iop, 22);
    }

    if (unlock) {
        enum mbe_status unlock_status = wlib_mbe_sql_fdes_unlock(fdes);

        if (unlock_status != MBE_S_OK && res == MBE_S_OK) {
            /* Releasing the lock commits what was written under it. If that fails the data is
             * lost, and the caller must know (if the operation had failed already, it says so) */
            l_error("Failed to release the lock: %d", unlock_status);
            iop -> u.completed_info.result = res = unlock_status;
            iop -> u.completed_info.transferred = 0;
        }
    }

    return res;
}

void  __attribute__ ((constructor(HK_CONSTRUCTOR_REGISTER))) wlib_mbe_init(void) {
    fio_register(FIO_BE_ENSCRIBE, &fifc);
}