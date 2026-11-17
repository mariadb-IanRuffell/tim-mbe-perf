/*
 * Utility functions that backends may need.
 */

#include <stdlib.h>

#include "wlib-mbe-sql/sql-be.h"
#include "wlib-internal/cfg.h"

static int config_loaded;
/* Settings specified by [mbe] section in wlib.ini */
static struct {

    int retry_timeout;

    int retry_random;

} config;

static void load_config(void) {
    enum wl_status res;
    int found;

    if (config_loaded) {
        return;
    }

    res = wl_cfg_get_int(NULL, "mbe", "retry_timeout", &found, &config.retry_timeout);
    if (res != WL_STATUS_OK || !found) {
        config.retry_timeout = 1000;
    }

    res = wl_cfg_get_int(NULL, "mbe", "retry_random", &found, &config.retry_random);
    if (res != WL_STATUS_OK || !found) {
        config.retry_random = 100;
    }

    config_loaded = 1;
}


timestamp_t wmbe_sql_be_next_retry(void) {
    load_config();

    return u_time() + config.retry_timeout + rand() % config.retry_random;
}
