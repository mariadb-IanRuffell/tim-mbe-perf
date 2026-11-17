/*
 * Datatypes.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "wlib/usrlib.h"

#include "wlib-mbe-sql/mbe-sql.h"
#include "wlib-mbe-sql/sql-be.h"

static enum mbe_status int_serialize(const void *data, unsigned size, int generic, char **dest);
static enum mbe_status int_deserialize(const char *text, void *dest, unsigned size);

static enum mbe_status float_serialize(const void *data, unsigned size, int generic, char **dest);
static enum mbe_status float_deserialize(const char *text, void *dest, unsigned size);

static enum mbe_status str_serialize(const void *data, unsigned size, int generic, char **dest);
static enum mbe_status str_deserialize(const char *text, void *dest, unsigned size);

static enum mbe_status rstr_serialize(const void *data, unsigned size, int generic, char **dest);
static enum mbe_status rstr_deserialize(const char *text, void *dest, unsigned size);

static enum mbe_status bin_serialize(const void *data, unsigned size, int generic, char **dest);
static enum mbe_status bin_deserialize(const char *text, void *dest, unsigned size);

static enum mbe_status fill_serialize(const void *data, unsigned size, int generic, char **dest);
static enum mbe_status fill_deserialize(const char *text, void *dest, unsigned size);

static unsigned int_max_text_len(unsigned size) { return 20; }      /* uint64 in decimal */
static unsigned float_max_text_len(unsigned size) { return 32; }    /* "%lg" of a double */
static unsigned str_max_text_len(unsigned size) { return size; }
static unsigned bin_max_text_len(unsigned size) { return 2 * size; }    /* hex */
static unsigned fill_max_text_len(unsigned size) { return 0; }

struct wmbe_sql_datatype wmbe_sql_datatypes[] = {

    { "int",     0, 1, 0, 1, WMBE_SQL_BE_PT_INTEGER, int_serialize, int_deserialize, int_max_text_len },
    { "float",   0, 1, 0, 1, WMBE_SQL_BE_PT_FLOAT,   float_serialize, float_deserialize, float_max_text_len },
    { "string",  0, 1, 1, 0, WMBE_SQL_BE_PT_VARCHAR, str_serialize, str_deserialize, str_max_text_len },
    { "rstring", 0, 1, 0, 0, WMBE_SQL_BE_PT_VARCHAR, rstr_serialize, rstr_deserialize, str_max_text_len },
    { "binary",  0, 1, 1, 0, WMBE_SQL_BE_PT_VARCHAR, bin_serialize, bin_deserialize, bin_max_text_len },
    { "filler",  1, 0, 0, 1, WMBE_SQL_BE_PT_VARCHAR, fill_serialize, fill_deserialize, fill_max_text_len },
    { NULL }

};

static char hex[] = { '0', '1', '2', '3', 
                      '4', '5', '6', '7', 
                      '8', '9', 'A', 'B',
                      'C', 'D', 'E', 'F'};

enum mbe_status int_serialize(const void *data, unsigned size, int generic, char **dest) {

    uint64_t val;

    switch (size) {
    case 1:
        val = *(uint8_t *) data;
        break;

    case 2:
        val = *(uint16_t *) data;
        break;

    case 4:
        val = *(uint32_t *) data;
        break;

    case 8:
        val = *(uint64_t *) data;
        break;

    default:
        return MBE_S_BAD_SPECS;
    }

    *dest = malloc(80);
    if (*dest == NULL) {
        return MBE_S_OUT_OF_MEMORY;
    }

    snprintf(*dest, 80, "%llu", (unsigned long long) val);

    return MBE_S_OK;
}

enum mbe_status int_deserialize(const char *text, void *dest, unsigned size) {
    unsigned long long val;

    if (!sscanf(text, "%llu", &val)) {
        return MBE_S_BAD_SPECS;
    }

    switch (size) {
    case 1:
        *(uint8_t *) dest = (uint8_t) val;
        break;

    case 2:
        *(uint16_t *) dest = (uint16_t) val;
        break;

    case 4:
        *(uint32_t *) dest = (uint32_t) val;
        break;

    case 8:
        *(uint64_t *) dest = (uint64_t) val;
        break;

    default:
        return MBE_S_BAD_SPECS;
    }

    return MBE_S_OK;
}

/* Number of significant digits needed to round-trip an IEEE-754 float/double (FLT_DECIMAL_DIG
 * and DBL_DECIMAL_DIG, which are not available before C11). The default of %g, 6, loses data. */
#define FLT_FMT "%.9g"
#define DBL_FMT "%.17g"

enum mbe_status float_serialize(const void *data, unsigned size, int generic, char **dest) {

    double val;

    if (size == sizeof(float)) {
        val = *(float *) data;
    } else if (size == sizeof(double)) {
        val = *(double *) data;
    } else {
        return MBE_S_BAD_SPECS;
    }

    *dest = malloc(80);
    if (*dest == NULL) {
        return MBE_S_OUT_OF_MEMORY;
    }

    snprintf(*dest, 80, size == sizeof(float) ? FLT_FMT : DBL_FMT, val);

    return MBE_S_OK;
}

enum mbe_status float_deserialize(const char *text, void *dest, unsigned size) {
    double val;

    if (!sscanf(text, "%lf", &val)) {
        return MBE_S_BAD_SPECS;
    }

    if (size == sizeof(float)) {
        *(float *) dest = val;
    } else if (size == sizeof(double)) {
        *(double *) dest = val;
    } else {
        return MBE_S_BAD_SPECS;
    }

    return MBE_S_OK;
}

enum mbe_status str_serialize(const void *data, unsigned size, int generic, char **dest) {
    const char *src = data;
    char *p;

    if (generic) {
        /* The key is the prefix of a LIKE pattern: its own % and _ must be taken literally.
         * The text ends at the first NUL, which is where the padding of the field starts. */
        unsigned len = strnlen(src, size);

        *dest = malloc(2 * len + 2);    // Every character may need an escape, then the % and the terminator
        if (*dest == NULL) {
            return MBE_S_OUT_OF_MEMORY;
        }

        p = *dest;
        for (unsigned i = 0; i < len; i++) {
            if (src[i] == '%' || src[i] == '_' || src[i] == WMBE_SQL_LIKE_ESCAPE[0]) {
                *p++ = WMBE_SQL_LIKE_ESCAPE[0];
            }
            *p++ = src[i];
        }
        *p++ = '%';
        *p = 0;

        return MBE_S_OK;
    }

    *dest = malloc(size + 1);
    if (*dest == NULL) {
        return MBE_S_OUT_OF_MEMORY;
    }

    memcpy(*dest, data, size);
    (*dest)[size] = 0;

    return MBE_S_OK;
}

enum mbe_status str_deserialize(const char *text, void *dest, unsigned size) {
    memset(dest, 0, size);
    strncpy(dest, text, size);

    return MBE_S_OK;
}

static enum mbe_status rstr_serialize(const void *data, unsigned size, int generic, char **dest) {
    int blanks;

    *dest = malloc(size + 1);

    if (*dest == NULL) {
        return MBE_S_OUT_OF_MEMORY;
    }

    for (int i = 0; i < size; i++) {
        (*dest)[i] = ((const char *) data) [size - i - 1];
    }
    (*dest)[size] = 0;

    blanks = strspn(*dest, " ");
    if (blanks) {
        memmove(*dest, *dest + blanks, strlen(*dest) - blanks + 1);
    }

    return MBE_S_OK;
}

static enum mbe_status rstr_deserialize(const char *text, void *dest, unsigned size) {
    int len;

    memset(dest, ' ', size);

    len = strlen(text);
    if (len > size) {
        len = size;
    }

    for (int i = 0; i < len; i++) {
        ((char *) dest) [i] = text[len - i - 1];
    }

    return MBE_S_OK;
}

enum mbe_status bin_serialize(const void *data, unsigned size, int generic, char **dest) {
    const uint8_t *d8 = data;

    *dest = malloc(2 * size + 2);   // Leave room for %

    if (*dest == NULL) {
        return MBE_S_OUT_OF_MEMORY;
    }

    for (int i = 0; i < size; i++) {
        (*dest)[2 * i] = hex[d8[i] >> 4];    
        (*dest)[2 * i + 1] = hex[d8[i] & 0xf];
    }
        
    if (generic) {
        (*dest)[2 * size] =  '%';
        (*dest)[2 * size + 1] =  0;
    } else {
        (*dest)[2 * size] = 0;
    }

    return MBE_S_OK;
}

/* Value of a hex digit, or -1 if c is not one */
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = toupper(c);
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }

    return -1;
}

enum mbe_status bin_deserialize(const char *text, void *dest, unsigned size) {
    memset(dest, 0, size);

    for (int i = 0; i < size; i++) {
        int hi, lo;

        /* A value shorter than the field is zero padded. Never look beyond the terminator:
         * checking the first digit is enough, because the second is read only if it is there */
        if (!text[2 * i]) {
            break;
        }

        hi = hex_digit(text[2 * i]);
        lo = hex_digit(text[2 * i + 1]);    /* -1 if this is the terminator: odd number of digits */
        if (hi < 0 || lo < 0) {
            return MBE_S_BAD_SPECS;
        }

        ((uint8_t *) dest)[i] = (hi << 4) | lo;
    }

    return MBE_S_OK;
}

static enum mbe_status fill_serialize(const void *data, unsigned size, int generic, char **dest) {
    *dest = strdup("");
    if (*dest == NULL) {
        return MBE_S_OUT_OF_MEMORY;
    }

    return MBE_S_OK;
}

static enum mbe_status fill_deserialize(const char *text, void *dest, unsigned size) {
    memset(dest, 0, size);
    
    return MBE_S_OK;
}
