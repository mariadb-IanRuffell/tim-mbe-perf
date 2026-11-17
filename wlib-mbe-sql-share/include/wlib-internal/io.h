/*
 * I/O Subsystem API.
 */

#ifndef __io_h

#define __io_h

#include <sys/types.h>

#include "wlib-internal/status.h"

struct io_operation;
struct io_descriptor;

enum io_lock_mode {
    IO_L_SUSPEND, IO_L_REJECT
};

enum io_lock_read_mode {
    IO_RL_SUSPEND, IO_RL_REJECT, IO_RL_IGNORE, IO_RL_WARN
};

/* Classes identify groups of descriptors on which to perform a waited operation.
 * They exist because AWAITIO is not the same as MBE_AWAITIO and they operate
 * on different sets of desciptors. */
enum io_class {
    /* The value 0 is assumed in cextdecs.h */
    IO_C_DEFAULT = 0,
    /* The value 1 is assumed in usrlib.h */
    IO_C_MBE = 1, 

    IO_C__LAST__
};

/* The I/O substem interface. */
struct io_subsystem_ifc {

    enum io_type {
        IO_T_FILE_IO,   // File-like I/O (e.g $INPUT, MBE, files...), see fio.h
        IO_T_SOCKET     // Sockets
    } type;

    /* Completes a pending operation. Data is guaranteed to be available (i.e. the
     * conditions set by in_progress_info are met). Returns 1 if the operation can be
     * removed, 0 if there is still work to do (e.g. write() tranferred less bytes
     * than the buffer size). If the operation fails, returns 1 and the subsys_result 
     * is set accordingly.
     * If this operation is blocking even if the descriptor is ready, it must
     * return within the timeout. This is needed e.g. on databases because often they don't
     * provide a usable async interface. Other backends should not lie and give
     * a descriptor that is ready only when the operation is nonblocking.
     */
    int (*complete_operation)(struct io_descriptor *iod, struct io_operation *iop, long timeout);

    /*
     * Cancels a pending operation. This happens if AWAITIOX times out,
     * so procedures described to the Guardian manual should be applied
     * (e.g. cleanup on queue files). It is also called when the deadline of
     * an operation expires: in this case the operation is still queued and
     * io.c completes it right after this call, so the subsystem must not
     * rely on the operation having been removed from the queue.
     */
    void (*cancel_operation)(struct io_descriptor *iod, struct io_operation *iop);

    /*
     * Set file permissions. The mode parameter has the same range as chmod(2).
     * Detailed error core is conveyed through errno.
     */
    enum wl_status (*set_mode)(struct io_descriptor *iod, mode_t mode);

    /*
     * Set file owner.
     */
    enum wl_status (*set_owner)(struct io_descriptor *iod, uid_t owner, gid_t group);

    /*
     * Set lock mode. See SETMODE, function 4 for the semantics and setmode.c for
     * the (obvious) mapping.
     */
    enum wl_status (*set_lock_mode)(struct io_descriptor *iod, enum io_lock_mode lock, enum io_lock_read_mode rlock);

    /*
     * Set echo mode.
     */
    enum wl_status (*set_echo)(struct io_descriptor *iod, int enabled);

    /*
     * Flush.
     */
    enum wl_status (*flush)(struct io_descriptor *iod);

    /*
     * Set timeout on queue files. Timeout is expressed in milliseconds.
     */
    enum wl_status (*set_timeout)(struct io_descriptor *iod, int64_t timeout);

    /*
     * Delete data. Corresponds to CONTROL(fd, 20).
     */
    enum wl_status (*delete_data)(struct io_descriptor *iod);

    /*
     * Release subsystem data. The pending operations are still there (they are
     * released by io_close() right after this call), so the subsystem can look
     * at them, e.g. to abandon the ones that are still in progress.
     */
    void (*release_subsys_data)(struct io_descriptor *iod);

    /*
     * Set if the operations of a descriptor can complete in any order. Normally they are
     * assumed to complete in the order they were issued: only the oldest one is reported to
     * the application, and the others wait for their turn even if they are ready. If this
     * flag is set, the operations that are ready are reported in the order they are found,
     * and an operation that is not ready doesn't hold back the ones that follow it. This is
     * what a descriptor needs when it stands for several unrelated requests, like the replies
     * to nowait Pathsend calls.
     */
    int unordered;

    /*
     * Optional. Called when an operation is handed over to the application (AWAITIOX) or
     * canceled, right before it is removed from the queue. It allows to publish information
     * that belongs to the operation (see io_operation.info) at the moment the application is
     * told about it, and not when the operation completes, which may happen earlier and in
     * a different order.
     */
    void (*on_dequeue)(struct io_descriptor *iod, struct io_operation *iop);
};

/* An input/output operation */
struct io_operation {

    /* -- Reserved fields: subsystems should not tamper with them */

    /* Allocation flag. This is used to look for a spare operation
     * in the io_descriptor.opa array */
    int used;

    /* Next operation */
    struct io_operation *next;

    /* -- Shared fields: inited by subsystems */

    /* Operation type. Values are not specified here,
     * and subsystems are free to define their own values. */
    int type;

    /* Op has been completed or is it still in progress? */
    int completed;

    /* Polling bitmask (POLLIN, POLLOUT etc.) */
    int events;

    union {

        /* When completed = 0, op is in progress */
        struct {

            /* Buffer size (amount of data to write, or max bytes to read) */
            int buffer_size;

            /* Bytes transferred so far. This is handy for subsystems that
             * need to perform multiple writes to trasfer the whole buffer  */
            unsigned transferred;

            /* Opaque parameters */
            union io_op_param {
                int idata;
                unsigned udata;
                unsigned long long lldata;
                void *pdata;
            } param;

        } in_progress_info;

        struct {

            /* Subsystem specific status */
            int result;

            /* Bytes actually transferred */
            unsigned transferred;

        } completed_info;

    } u;

    /* Buffer */
    void *buffer;

    /* Tag */
    long long tag;

    /* Optional deadline, expressed in milliseconds since the Unix epoch (see u_time()).
     * 0 means no deadline and it is the value that io_enqueue_op() sets. Subsystems can
     * set it after enqueueing the operation. If the operation is still in progress when
     * the deadline passes, io_await() cancels it (cancel_operation is called) and
     * completes it with result 40 (timeout) and no bytes transferred. */
    long long deadline;

    /* Subsystem specific information about the outcome of the operation, e.g. the
     * extended error code of a call. io.c doesn't look at it, it only hands the operation to
     * the on_dequeue callback. It is zero when the operation is enqueued. */
    int info1;
    int info2;
};

/* 
 * An entry in the file descriptor table.
 */
struct io_descriptor {

    /* Next open descriptor */
    struct io_descriptor *next;

    /* Set if we enqueued at least one operation since last epoll_ctl */
    int dirty;

    /* Descriptor clazz */
    enum io_class clazz;

    /* Set if open */
    int open;

    /* The file name, if applicable */
    char fname[24];

    /* Subsystem-specific flags */
    int flags;

    /* Receive depth (where applicable) */
    int recv_depth;

    /* Nowait depth */
    int nowait_depth;

    /* Parallel mode enabled. When set, read and writes run in parallel: this
     * is enabled by SETMODE(30, 1) or SETMODE(30, 3). It is disabled 
     * by SETMODE(30, 0).
     */
    int parallel_mode;

    /* Last error code on this file descriptor */
    int last_error;

    /* The subsystem-specific primitives */
    struct io_subsystem_ifc *ifc;

    /* Optional, subsystem specific data. If not NULL, it is released
     * by io_close() by using free() */
    void *subsys_data;

    /* The list of pending operations, i.e. non-blocking
     * ops that have been issued but whose result has not
     * (yet) been retrieved via AWAITIOX  */
    struct io_operation *pending;

    /*
     * The array of io_operation's from which 
     * the list above is formed. It's allocated by
     * io_allocate and its size is equal to nowait_depth.
     */
    struct io_operation *opa;
};

/* Initializes the module */
enum wl_status io_init(void);

/* Stops the IO module */
void io_stop(void);

/*
 * Allocates a file descriptor. The fd number is specified by the 
 * caller because it is conventent to keep it equal to the
 * fd number that the operating system gave us.
 * If it fails, it sets errno accordingly.
 */
struct io_descriptor *io_allocate(int fd, int flags, int nowait_depth,
    struct io_subsystem_ifc *ifc);                // + errno

enum wl_status io_close(int fd);                  // + errno

int io_get_fd(struct io_descriptor *iod);         // Does not fail

struct io_descriptor *io_get_iod(int fd);         // + errno

/*
 * Looks for a descriptor matching a given condition.
 */
struct io_descriptor *io_find(int (*match)(struct io_descriptor *, void *), void *data);

/* Note that io_await() never sleeps past the deadline of an operation that has one:
 * it wakes up, cancels the operation (see io_operation.deadline) and returns it as completed. */
enum wl_status io_await(int *fd, long timeout, enum io_class clazz, struct io_operation **iopp);
                                                 // + errno unless WL_STATUS_NO_OPERATION
                                                 //    (there is no valid mapping on Unices)

struct io_operation *io_cancel(int fd);          // + errno

int io_set_error(int fd, int code);              // Does not fail

/* 
 * Tries to complete all the operations associated to a given fd, using the specified
 * mode (parallel read/write or serial). It is used by e.g. write operations to start
 * the send operation immediately. The timeout is expressed in milliseconds, and
 * is given to each operation individually, so there is no guarantee that the call
 * returns within this time limit. Conceptually there is no need for a timeout, because
 * operations are performed only when they are non-blocking, but some backends 
 * (e.g. databases) may lie and tell they are ready although they are not.
 */
void io_flush(int fd, long timeout);                           // Does not fail

/*
 * Set parallel fd mode.
 */
void io_set_parallel(int fd, int parallel);

/*
 * Allocates a new operation. It returns NULL if this can't be
 * done (errno is set accordingly). 
 * Note that since there's no convenient mapping, if there are too many
 * outstanding ops, this function return NULL and sets errno to ENOERR.
 */
struct io_operation *io_enqueue_op(int fd);     // + errno

/*
 * Takes back an operation that was just allocated by io_enqueue_op() and that the application
 * has not seen yet, because the subsystem could not start it. The queue is left as if
 * the operation was never there. This must not be used on operations that are in progress,
 * see io_cancel() for them.
 */
void io_discard_op(int fd, struct io_operation *iop);

#endif