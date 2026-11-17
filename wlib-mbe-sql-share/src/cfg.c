/*
 * Configuration file support.
 * We use the narive WLIB .ini support, and the file format is:
 * [general]
 * url = <db url>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wlib-internal/status.h"
#include "wlib/usrlib.h"

#define L_FEATURE L_FEAT_DATA

#include "wlib-internal/cfg.h"
#include "wlib-internal/log.h"

#include "wlib-mbe-sql/mbe-sql.h"

#ifdef __x86_64__
#define ALIGN 8
#elif defined(__i386__)
#define ALIGN 4
#else
#error Cannot find target alignment
#endif

static enum mbe_status parse_field(struct wl_cfg_file *cfg, struct wmbe_sql_field *field, int fno, 
                                int *ofs, int *param_pos, struct wmbe_sql_file *file) {
    struct wmbe_sql_datatype *dt;
    char section[20];
    char type[256];
    int found, key_spec, has_align;
    int align;

    snprintf(section, 20, "field-%d", fno + 1);
    if (wl_cfg_get_string(cfg, section, "type", &found, type, sizeof(type)) != WL_STATUS_OK
        || !found) {
        l_error("Missing or invalid %s/type key", section);
        return MBE_S_BAD_SPECS;
    }

    if (wl_cfg_get_string(cfg, section, "name", &found, field -> name, sizeof(field -> name)) != WL_STATUS_OK
        || !found) {
        l_error("Missing or invalid %s/name key", section);
        return MBE_S_BAD_SPECS;
    }

    if (wl_cfg_get_int(cfg, section, "size", &found, &field -> size) != WL_STATUS_OK
        || !found) {
        l_error("Missing or invalid %s/size key", section);
        return MBE_S_BAD_SPECS;
    }

    if (wl_cfg_get_int(cfg, section, "align", &has_align, &align) != WL_STATUS_OK) {
        l_error("Invalid %s/align key", section);
        return MBE_S_BAD_SPECS;
    }

    if (wl_cfg_get_int(cfg, section, "unique", &found, &field -> unique) != WL_STATUS_OK) {
        l_error("Invalid %s/unique key", section);
        return MBE_S_BAD_SPECS;
    }
    if (!found) {
        field -> unique = 0;
    }
    field -> unique = field -> unique != 0;

    if (wl_cfg_get_int(cfg, section, "key_spec", &found, &key_spec) != WL_STATUS_OK) {
        l_error("Invalid %s/key_spec key", section);
        return MBE_S_BAD_SPECS;
    }
    if (field -> unique && (!found || key_spec == 0)) {
        l_error("Invalid %s/unique key: it only applies to alternate keys (key_spec > 0)", section);
        return MBE_S_BAD_SPECS;
    }
    if (found) {
        if (file < 0 || key_spec >= file -> field_number) {
            l_error("Invalid %s/key_spec value (%d). Must not be larger than number of fields (%d)",
                section, key_spec, file -> field_number);
                return MBE_S_BAD_SPECS;
        }   
        if (file -> keys[key_spec].field != NULL) {
            l_error("Invalid %s/key_spec value (%d). This key_spec is already used for field %s",
                section, key_spec, file -> keys[key_spec].field -> name);
                return MBE_S_BAD_SPECS;
        }
        if (strlen(file -> table) + strlen(field -> name) > WMBE_SQL_MAX_TABLE_KEY_LEN) {
            l_error("Table <%s> and key column <%s> have names too long: together they must not exceed %d characters",
                file -> table, field -> name, WMBE_SQL_MAX_TABLE_KEY_LEN);
            return MBE_S_BAD_SPECS;
        }
        file -> keys[key_spec].field = field;  
    } else {
        key_spec = -1;
    }

    for (dt = wmbe_sql_datatypes; dt -> name != NULL; dt++) {
        if (!strcasecmp(dt -> name, type)) {
            break;
        }
    }

    if (dt -> name == NULL) {
        l_error("Unknown type: %s/type = <%s>", section, type);
        return MBE_S_BAD_SPECS;
    }

    l_debug("Found matching datatype");

    /* Column values are fetched into fixed buffers: refuse fields that can't fit */
    if (!dt -> virt && dt -> max_text_len(field -> size) >= WMBE_SQL_VALUE_BUFLEN) {
        l_error("Field %s is invalid: a %s of size %d needs up to %u characters, but the limit is %d",
            section, type, field -> size, dt -> max_text_len(field -> size), WMBE_SQL_VALUE_BUFLEN - 1);
        return MBE_S_BAD_SPECS;
    }

    if (!has_align) {
        if (dt -> align) {
            align = field -> size;
        } else {
            align = 0;
        }
    }

    if (align > ALIGN) {
        align = ALIGN;
    }

    l_debug("Alignment is set to %d", align);

    field -> dt = dt;
    if (align && *ofs % align) {
        *ofs += align - *ofs % align;
    }
    field -> ofs = *ofs, *ofs += field -> size;
    if (!dt -> virt) {
        field -> param_pos = (*param_pos)++;
    }

    if (key_spec >= 0 && !dt -> can_be_key) {
        l_error("Field %s is invalid: key_spec is set but %s can't be a key", section, type);
        return MBE_S_BAD_SPECS;
    }

    l_debug("Initialization of field is complete: %s goes at offset %d", field -> name, field -> ofs);

    return MBE_S_OK;
}

enum wl_status wlib_mbe_cfg_parse_cfg_file(struct wmbe_sql_file *fdes, struct wl_cfg_file *cfg) {

    int found;
    int fields;
    int ofs, param_pos;

    l_debug("Starting to parse file");
    if (wl_cfg_get_string(cfg, "general", "url", &found, fdes -> url, sizeof(fdes -> url)) != WL_STATUS_OK
        || !found) {
        l_error("Missing or invalid general/url key");
        return WL_STATUS_PARSE_ERROR;
    }

    if (wl_cfg_get_string(cfg, "general", "table", &found, fdes -> table, sizeof(fdes -> table)) != WL_STATUS_OK
        || !found) {
        l_error("Missing or invalid general/table key");
        return WL_STATUS_PARSE_ERROR;
    }

    if (wl_cfg_get_int(cfg, "general", "fields", &found, &fields) != WL_STATUS_OK
        || !found || fields <= 0) {
        l_error("Missing or invalid general/fields key");
        return WL_STATUS_PARSE_ERROR;
    }

    fdes -> field_number = fields;
    fdes -> fields = calloc(fields, sizeof (struct wmbe_sql_field));
    fdes -> keys = calloc(fields, sizeof(struct wmbe_sql_key));
    if (fdes -> fields == NULL || fdes -> keys == NULL) {
        l_error("Not eoungh memory to allocate schema data");
        return WL_STATUS_OUT_OF_MEMORY;
    }

    l_debug("Need to parse %d field", fields);
    ofs = 0;
    param_pos = 1;
    for (int i = 0; i < fields; i++) {
        enum mbe_status status;

        l_debug("Parsing field #%d", i);
        status = parse_field(cfg, &fdes -> fields[i], i, &ofs, &param_pos, fdes);
        if (status != MBE_S_OK) {
            l_error("Failed to parse field #%d", i);
            return WL_STATUS_PARSE_ERROR;
        }
    }

    fdes -> record_size = ofs;

    return WL_STATUS_OK;
}