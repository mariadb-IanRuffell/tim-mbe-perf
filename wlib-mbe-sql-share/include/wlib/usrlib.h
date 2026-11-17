#ifndef __usrlib_h

#define __usrlib_h

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int _cc_status;

#define MBE_POS_APPROX  0
#define MBE_POS_GENERIC 1
#define MBE_POS_EXACT   2

enum mbe_status {

    MBE_S_OK = 0,
    MBE_S_WARN = 1,
    MBE_S_RECORD_EXISTS = 10,
    MBE_S_RECORD_NOT_FOUND = 11,
    MBE_S_BAD_FD = 16,
    MBE_S_BAD_COUNT = 21,
    MBE_S_BAD_SPECS = 29,
    MBE_S_OUT_OF_MEMORY = 31,
    MBE_S_INVALID_TRANSACTION = 35,
    MBE_S_INVALID_KEY = 46,
    MBE_S_IO_ERROR = 53,
    MBE_S_TIMEOUT = 40
};

#define MBE_FILE_OPEN_                  FILE_OPEN_
#define MBE_FILE_CLOSE_                 FILE_CLOSE_
// #define MBE_FILE_GETINFO_               FILE_GETINFO_
#define MBE_FILE_SETKEY_                FILE_SETKEY_
// #define MBE_FILE_GETINFOLISTBYNAME_     FILE_GETINFOLISTBYNAME_
#define MBE_READX                       READX_
#define MBE_READLOCKX                   READLOCKX_
// #define MBE_READUPDATEX                 READUPDATEX_
#define MBE_READUPDATELOCKX             READUPDATELOCKX_
#define MBE_WRITEX                      WRITEX_
#define MBE_WRITEUPDATEUNLOCKX          WRITEUPDATEUNLOCKX_
#define MBE_WRITEUPDATEX                WRITEUPDATEX_
// #define MBE_LOCKREC                     LOCKREC_
#define MBE_UNLOCKREC                   UNLOCKREC_

#define FILE_OPEN_(filename, length, filenum) \
    _fFILE_OPEN_(filename, length, filenum, 0, 0, 0, 0, 0, -1, -1, NULL, -1)

short _fFILE_OPEN_(const char *filename, short length, short *filenum, short access, short exclusion,
    short nowait_depth, short sync_or_receive_depth, short options, short seq_block_buffer_id,
    short seq_block_buffer_len, short *primary_phandle, long elections);

#define FILE_SETKEY_(fd, key_val, key_len, key_spec, pos_mode, options) \
    _fFILE_SETKEY_(fd, key_val, key_len, key_spec, pos_mode, options, -1)
short _fFILE_SETKEY_(short fd, const char *key_val, short key_len, short key_spec, short pos_mode, short options, short compare_length);

#define READX_(fd, buffer, read_count, count_read, tag) \
    _fREADX_(fd, buffer, read_count, count_read, tag)
short _fREADX_(short fd, char *buffer, unsigned short read_count, unsigned short *count_read, int32_t tag);

#define READLOCKX_(fd, buffer, read_count, count_read, tag) \
    _fREADLOCKX_(fd, buffer, read_count, count_read, tag)
short _fREADLOCKX_(short fd, char *buffer, unsigned short read_count, unsigned short *count_read, int32_t tag);

#define READUPDATELOCKX_(fd, buffer, read_count, count_read, tag) \
    _fREADUPDATELOCKX_(fd, buffer, read_count, count_read, tag)
short _fREADUPDATELOCKX_(short fd, char *buffer, unsigned short read_count, unsigned short *count_read, unsigned tag);

#define WRITEUPDATEUNLOCKX_(fd, buffer, write_count, count_written, tag) \
    _fWRITEUPDATEUNLOCKX_(fd, buffer, write_count, count_written, tag)
short _fWRITEUPDATEUNLOCKX_(short fd, const char *buffer, short write_count, unsigned short *count_written, unsigned tag);

#define UNLOCKREC_(fd) \
    _fUNLOCKREC_(fd, -1)
short _fUNLOCKREC_(short fd, long tag);

#define WRITEX_(fd, buffer, write_count, count_written, tag) \
    _fWRITEX_(fd, buffer, write_count, count_written, tag)
short _fWRITEX_(short fd, const char *buffer, unsigned short write_coumt,
    unsigned short *count_written, int32_t tag);

#define WRITEUPDATEX_(fd, buffer, write_count, count_written, tag) \
    _fWRITEUPDATEX_(fd, buffer, write_count, count_written, tag)
short _fWRITEUPDATEX_(short fd, const char *buffer, short write_count, unsigned short *count_written, unsigned tag);

short FILE_CLOSE_(short fd);

#ifdef __cplusplus
}
#endif

#endif
