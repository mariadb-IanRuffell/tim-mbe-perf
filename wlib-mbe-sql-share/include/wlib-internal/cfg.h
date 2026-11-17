#ifndef __cfg_h

#define __cfg_h

#include "status.h"

struct wl_cfg_file;

#define WL_CFG_SYS ((struct wl_cfg_file *) NULL)

/*
 * Opens a configuration file, returning a config handle.
 */
enum wl_status wl_cfg_open(const char *fname, struct wl_cfg_file **cfg);

/*
 * Releases the config handle.
 */
void wl_cfg_close(struct wl_cfg_file *cfg);

/*
 * Returns a string key from the given section. The handle (cfg) can be
 * obtained by wl_cfg_open above, or it may be WL_CFG_SYS,
 * to access the system property config file.
 * If the string is longer than maxlen - 1, it is truncated, but it
 * will always be null-terminated.
 * If the key is not found, *found is set to zero, but
 * the return value is OK.
 */
enum wl_status wl_cfg_get_string(struct wl_cfg_file *cfg, const char *section, 
        const char *key, int *found, char *value, int maxlen);

/*
 * Returns a (signed) int key from the given section. The handle (cfg) can be
 * obtained by wl_cfg_open above, or it may be WL_CFG_SYS,
 * to access the system property config file.
 * If the key is not found, *found is set to zero, but
 * the return value is OK.
 */
enum wl_status wl_cfg_get_int(struct wl_cfg_file *cfg, const char *section, 
        const char *key, int *found, int *value);

#endif

