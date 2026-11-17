/*
 * File-like I/O. It groups all the IO subsystems that support the READ and WRITE operations.
 */

 #ifndef __fio_h

 #define __fio_h

#include "io.h"
#include "status.h"

#define FIO_OP_LOCK_FLAG     (1 << 31)
#define FIO_OP_UNLOCK_FLAG   (1 << 30)
#define FIO_LOCKED(op)       ((op) | FIO_OP_LOCK_FLAG)
#define FIO_UNLOCKED(op)     ((op) | FIO_OP_UNLOCK_FLAG)
#define FIO_IS_LOCKED(op)    (((op) & FIO_OP_LOCK_FLAG) != 0)
#define FIO_IS_UNLOCKED(op)  (((op) & FIO_OP_UNLOCK_FLAG) != 0)
#define FIO_MODULO_LOCK(op)  ((op) & ~(FIO_OP_LOCK_FLAG | FIO_OP_UNLOCK_FLAG))

/* Possible values for the operation.type field */
enum fio_op_type {
    FIO_OP_READ,                // read
#define FIO_OP_READ_LOCK FIO_LOCKED(FIO_OP_READ)
    FIO_OP_READ_UPDATE,         // read
#define FIO_OP_READ_UPDATE_LOCK FIO_LOCKED(FIO_OP_READ_UPDATE)
    FIO_OP_WRITE,               // write
    FIO_OP_WRITE_READ,          // write_read
    FIO_OP_WRITE_UPDATE,        // write
#define FIO_OP_WRITE_UPDATE_UNLOCK FIO_UNLOCKED(FIO_OP_WRITE_UPDATE)
    FIO_OP_SEEK,                // control
    FIO_OP_LOCK_RECORD,         // write
    FIO_OP_UNLOCK_RECORD        // write
};

/* Backends. The order should match their priority when calling can_open */
enum fio_backend {
    FIO_BE_RECEIVE,
    FIO_BE_PROCESS,
    FIO_BE_ENSCRIBE,
    FIO_BE_DISK,
    FIO_BE__LAST__
};

enum fio_open_flags {
    FIO_F_RECV_SYS_MSGS = 1,
    FIO_F_TIMED = 2,
    FIO_F_NOWAIT_OPEN = 4
};


enum fio_access {
    FIO_A_READ_WRITE,
    FIO_A_READ_ONLY,
    FIO_A_WRITE_ONLY
};

struct fio_backend_ifc {

    int (*can_open) (const char *fname);
    enum wl_status (*do_open) (const char *fname, int *fd, 
                int open_flags, short sync_or_recv_depth, int nowait_depth,
                enum fio_access, int exclusion);

};

void fio_register(enum fio_backend be, struct fio_backend_ifc *ifc);

enum wl_status fio_open(const char *fname, int *fd, int open_flags, 
        short sync_or_recv_depth, int nowait_depth, enum fio_access access, int exclusion);

/* Run a synchronous or asynchronous read-like operation. Detailed error is in errno */
enum wl_status fio_read(enum fio_op_type type, int fd, char *buffer, unsigned bufsz, 
        unsigned *count, long tag, int *wait_result);

/* Run a synchronous or asynchronous write-like operation. Detailed error is in errno */
enum wl_status fio_write(enum fio_op_type type, int fd, const char *data, unsigned datalen, unsigned *count, 
        long tag, int *wait_result);

/* Run a synchronous or asynchronous write + read-like operation. Detailed error is in errno */
enum wl_status fio_write_read(enum fio_op_type type, int fd, char *buffer, unsigned write_count, 
        unsigned bufsz, unsigned *count, int *wait_result);

/* Run a synchronous control operation. Detailed error is in errno */
enum wl_status fio_control(enum fio_op_type type, int fd, union io_op_param *param, int *wait_result);

#endif