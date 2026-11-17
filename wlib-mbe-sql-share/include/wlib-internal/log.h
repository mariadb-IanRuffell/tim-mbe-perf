/*
 * Log interface to g_log.
 */

#ifndef __log_h

#define __log_h

#include <stdarg.h>
#include <stdio.h>

/* Keep this enum in sync with log.c feature_names! */
enum l_feature {

    L_FEAT_INIT    = 0,
    L_FEAT_HEALTH,
    L_FEAT_IO,
    L_FEAT_SUPPORT,
    L_FEAT_IPC,
    L_FEAT_SIP,
    L_FEAT_HTTP2,
    L_FEAT_DATA,
    L_FEAT_SECURE,
    L_FEAT_KV,
    L_FEAT_ERAD,
    L_FEAT_SYS_LAST,

    L_FEAT_USER = 100,
    L_FEAT_USER_LAST = 110
#define L_FEAT_LAST L_FEAT_USER_LAST
};


enum l_be_log {
    L_L_DEBUG = 0,
    L_L_INFO,
    L_L_WARNING,
    L_L_ERROR,

    L_L_LAST
};

/* This should be set by the compiler */
#ifndef G_LOG_DOMAIN
#define G_LOG_DOMAIN "wlib"
#endif


/* If you get this error, #define L_FEATURE to one of the values from
 * the enum just above */
#ifndef L_FEATURE
#error Please define L_FEATURE to set the default feature of logs
#endif

#if defined(__GNUC__) || defined(__clang__)
#define WLIB_PRINTF_FORMAT(string_index, first_to_check) \
    __attribute__((format(printf, string_index, first_to_check)))
#else
#define WLIB_PRINTF_FORMAT(string_index, first_to_check)
#endif

void l_init(void);
void l_log(const char *domain, enum l_be_log level, const char *file, int line, enum l_feature feat, int id, const char *fmt, ...)
    WLIB_PRINTF_FORMAT(7, 8);
void l_logv(const char *domain, enum l_be_log level, const char *file, int line, enum l_feature feat, int id, const char *fmt, va_list args)
    WLIB_PRINTF_FORMAT(7, 0);
void l_set_label(const char *label);

/* l_feature to log level mappings */
extern int l_level[];

/* Where the log messages go. By default it is stdout */
extern FILE *l_log_output;

#define L_LOG(level, f, l, feat, ...) do {\
        if (l_level[feat] <= level) { \
            l_log(G_LOG_DOMAIN, level, f, l, feat, -1, __VA_ARGS__); \
        } \
    } while (0)


#define l_trace(...)  L_LOG(L_L_DEBUG, __FILE__, __LINE__, L_FEATURE, __VA_ARGS__)
#define l_debug(...)  L_LOG(L_L_DEBUG, __FILE__, __LINE__,  L_FEATURE, __VA_ARGS__)
#define l_info(...)   L_LOG(L_L_INFO, __FILE__, __LINE__, L_FEATURE, __VA_ARGS__)
#define l_warn(...)   L_LOG(L_L_WARNING, __FILE__, __LINE__, L_FEATURE, __VA_ARGS__)
#define l_error(...)  L_LOG(L_L_ERROR, __FILE__, __LINE__, L_FEATURE, __VA_ARGS__)

#endif