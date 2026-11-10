#define NOB_STRIP_PREFIX
#include "third_party/nob/nob.h"

#include <time.h>

#include "cells.h"
#include "src/http/serve.h"

const char *md_date_human(const char *iso) {
    if (iso == NULL || iso[0] == '\0') {
        return "";
    }
    int y, mo, d, h = 0, mi = 0;
    int got = sscanf(iso, "%d-%d-%d%*c%d:%d", &y, &mo, &d, &h, &mi);
    if (got < 3 || mo < 1 || mo > 12 || d < 1 || d > 31) {
        return iso;
    }
    if (got >= 5) {
        struct tm utc = {0};
        utc.tm_year = y - 1900;
        utc.tm_mon  = mo - 1;
        utc.tm_mday = d;
        utc.tm_hour = h;
        utc.tm_min  = mi;
        utc.tm_isdst = -1;
        time_t ep = timegm(&utc);
        struct tm loc = {0};
        if (ep != (time_t)-1 && localtime_r(&ep, &loc) != NULL) {
            y  = loc.tm_year + 1900;
            mo = loc.tm_mon + 1;
            d  = loc.tm_mday;
            h  = loc.tm_hour;
            mi = loc.tm_min;
        }
    }
    static const char *const mon[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    if (got >= 5) {
        return temp_sprintf("%02d %s %04d, %02d:%02d",
                            d, mon[mo - 1], y, h, mi);
    }
    return temp_sprintf("%02d %s %04d", d, mon[mo - 1], y);
}

const char *md_date_input(const char *iso) {
    if (iso == NULL || iso[0] == '\0') {
        return "";
    }
    size_t n = strlen(iso);
    if (n < 10) {
        return iso;
    }
    return temp_sprintf("%.10s", iso);
}

const char *md_cell_part_label(const MD_Column *col, size_t part) {
    if (col == NULL || col->subcols == NULL || part >= col->subcol_count) {
        return "";
    }
    if (col->subcols[part].label != NULL && col->subcols[part].label[0] != '\0') {
        return col->subcols[part].label;
    }
    if (col->subcols[part].name != NULL) {
        return col->subcols[part].name;
    }
    return "";
}

static void cell_render_stack(String_Builder *sb, const MD_Column *col,
                              char *const *values, size_t slot)
{
    static const char *const rank[] = {
        "text-gray-900 dark:text-gray-100 font-medium",
        "text-gray-700 dark:text-gray-300",
        "text-gray-500 dark:text-gray-400",
        "text-gray-400 dark:text-gray-500",
    };
    const size_t n = sizeof(rank) / sizeof(rank[0]);
#define OUT(buf, size) sb_append_buf(sb, buf, size);
#define STR(s) sb_append_cstr(sb, s);
#define ESCAPED(s) \
    do { \
        if ((s) != NULL) { \
            sb_append_html_escaped(sb, (s)); \
        } else { \
            sb_append_html_escaped(sb, ""); \
        } \
    } while (0)
#include "build/h_to_html/component/cell_stack.h"
#undef OUT
#undef STR
#undef ESCAPED
}

static const char *status_accent(const char *status) {
    if (status != NULL && status[0] != '\0') {
        if (strcmp(status, "ACTIVE") == 0)   return "blue";
        if (strcmp(status, "INACTIVE") == 0) return "peach";
        if (strcmp(status, "SUSPENDED") == 0) return "red";
    }
    return NULL;
}

static void cell_render_avatar(String_Builder *sb, const MD_Column *col,
                               char *const *values, size_t slot)
{
    const char *img = "";
    if (col->subcol_count > 0 && values[slot] != NULL) {
        img = values[slot];
    }
    const char *name = "";
    const char *sub = "";
    const char *status = "";
    if (col->subcol_count >= 5) {
        const char *f = "";
        if (values[slot + 1] != NULL) f = values[slot + 1];
        const char *l = "";
        if (values[slot + 2] != NULL) l = values[slot + 2];
        if (f[0] != '\0' && l[0] != '\0') {
            name = temp_sprintf("%s %s", f, l);
        } else if (f[0] != '\0') {
            name = f;
        } else {
            name = l;
        }
        if (col->subcol_count > 3 && values[slot + 3] != NULL) {
            sub = values[slot + 3];
        }
        if (col->subcol_count > 4 && values[slot + 4] != NULL) {
            status = values[slot + 4];
        }
    } else {
        if (col->subcol_count > 1 && values[slot + 1] != NULL) {
            name = values[slot + 1];
        }
        if (col->subcol_count > 2 && values[slot + 2] != NULL) {
            sub = values[slot + 2];
        }
        if (col->subcol_count > 3 && values[slot + 3] != NULL) {
            status = values[slot + 3];
        }
    }
    const char *accent = status_accent(status);

    const char *ring = "";
    if (accent != NULL) {
        ring = temp_sprintf("ring-2 ring-%s", accent);
    }
    const char *status_cls = "text-overlay0";
    if (accent != NULL) {
        status_cls = temp_sprintf("text-%s", accent);
    }

    char initial_buf[8] = {0};
    const char *initial = initial_buf;
    if (img[0] == '\0' && name[0] != '\0') {
        const unsigned char *p = (const unsigned char *) name;
        size_t n = 1;
        if ((p[0] & 0xE0) == 0xC0)      n = 2;
        else if ((p[0] & 0xF0) == 0xE0) n = 3;
        else if ((p[0] & 0xF8) == 0xF0) n = 4;
        else if (p[0] & 0x80)           n = 1;

        size_t copy_len = sizeof(initial_buf) - 1;
        if (n < copy_len) {
            copy_len = n;
        }
        memcpy(initial_buf, name, copy_len);
    }
#define OUT(buf, size) sb_append_buf(sb, buf, size);
#define STR(s) sb_append_cstr(sb, s);
#define ESCAPED(s) \
    do { \
        if ((s) != NULL) { \
            sb_append_html_escaped(sb, (s)); \
        } else { \
            sb_append_html_escaped(sb, ""); \
        } \
    } while (0)
#include "build/h_to_html/component/cell_avatar.h"
#undef OUT
#undef STR
#undef ESCAPED
}

static void cell_render_activity(String_Builder *sb, const MD_Column *col,
                                 char *const *values, size_t slot)
{
    const char *created = values[slot];
    const char *updated = "";
    if (col->subcol_count > 1 && values[slot + 1] != NULL) {
        updated = values[slot + 1];
    }
    const char *deleted = "";
    if (col->subcol_count > 2 && values[slot + 2] != NULL) {
        deleted = values[slot + 2];
    }
    bool wrote = false;
    if (created != NULL && created[0] != '\0') {
        wrote = true;
    } else if (updated != NULL && updated[0] != '\0') {
        if (created == NULL || strcmp(updated, created) != 0) {
            wrote = true;
        }
    } else if (deleted != NULL && deleted[0] != '\0') {
        wrote = true;
    }
#define OUT(buf, size) sb_append_buf(sb, buf, size);
#define STR(s) sb_append_cstr(sb, s);
#include "build/h_to_html/component/cell_activity.h"
#undef OUT
#undef STR
}

const char *md_cell_render(const MD_Column *col, char *const *values, size_t slot) {
    if (col == NULL || col->subcol_count == 0) {
        return "";
    }
    String_Builder sb = {0};
    if (col->style != NULL && strcmp(col->style, "avatar") == 0) {
        cell_render_avatar(&sb, col, values, slot);
    } else if (col->style != NULL && strcmp(col->style, "activity") == 0) {
        cell_render_activity(&sb, col, values, slot);
    } else {
        cell_render_stack(&sb, col, values, slot);
    }
    sb_append_null(&sb);
    const char *out = temp_strdup(sb.items);
    sb_free(sb);
    return out;
}
