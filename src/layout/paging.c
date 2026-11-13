#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#include "paging.h"
#include "src/http/utils.h"

// Copy a query value into a NUL-terminated scratch buffer and parse it as
// an unsigned long. Returns false when the value is absent, empty or not a
// plain number (caller keeps its default).
static bool page_query_size(String_View query, const char *key, size_t *out) {
    String_View v = {0};
    if (!form_find(query, key, &v) || v.count == 0) return false;
    for (size_t i = 0; i < v.count; ++i) {
        if (v.data[i] < '0' || v.data[i] > '9') return false;
    }
    if (v.count >= 21) {
        *out = SIZE_MAX;
        return true;
    }

    char buf[32];
    memcpy(buf, v.data, v.count);
    buf[v.count] = '\0';

    errno = 0;
    char *end = NULL;
    unsigned long long val = strtoul(buf, &end, 10);
    if (end == buf || *end != '\0') return false;
    if (errno == ERANGE || val > (unsigned long long)SIZE_MAX) {
        *out = SIZE_MAX;
        return true;
    }
    if (val == 0) return false;
    *out = (size_t)val;
    return true;
}

void page_info_parse(Page_Info *out, String_View query,
                     const char *page_key, const char *per_key)
{
    memset(out, 0, sizeof(*out));
    if (page_key != NULL && page_key[0] != '\0') {
        out->page_key = page_key;
    } else {
        out->page_key = "page";
    }
    if (per_key != NULL && per_key[0] != '\0') {
        out->per_key = per_key;
    } else {
        out->per_key = "per_key";
    }
    out->page = 1;
    out->per_page = PAGE_DEFAULT_PER_PAGE;
    size_t v = 0;
    if (page_query_size(query, out->page_key, &v) && v >= 1) out->page = v;
    if (page_query_size(query, out->per_key, &v) && v >= 1) {
        if (v <= PAGE_MAX_PER_PAGE) {
            out->per_page = v;
        } else {
            out->per_page = PAGE_MAX_PER_PAGE;
        }
    }
}

void page_info_finish(Page_Info *out, size_t total) {
    if (out->per_page == 0) out->per_page = PAGE_DEFAULT_PER_PAGE;
    out->total = total;
    if (total == 0) {
        out->total_pages = 1;
    } else {
        out->total_pages = (total - 1) / out->per_page + 1;
    }
    if (out->page < 1) out->page = 1;
    if (out->page > out->total_pages) out->page = out->total_pages;
    out->offset = (out->page - 1) * out->per_page;
    if (total == 0) {
        out->row_start = 0;
        out->row_end   = 0;
    } else {
        out->row_start = out->offset + 1;
        if (out->per_page < total - out->offset) {
            out->row_end = out->offset + out->per_page;
        } else {
            out->row_end = total;
        }
    }
}

bool page_fragment_requested(String_View query) {
    String_View v = {0};
    return form_find(query, "fragment", &v)
        && v.count == 1 && v.data[0] == '1';
}

static void sb_append_url_attr_safe(String_Builder *sb, String_View sv) {
    for (size_t i = 0; i < sv.count; ++i) {
        unsigned char c = (unsigned char)sv.data[i];
        if (c == '"' || c == '\'' || c == '<' || c == '>' || c == '\\' ||
            c == 0x20 || c == 0x7f) {
            sb_appendf(sb, "%%%02X", (unsigned int)c);
        } else {
            sb_append_buf(sb, &sv.data[i], 1);
        }
    }
}

void page_href(String_Builder *sb, const char *base, String_View query,
               const Page_Info *info, size_t page, size_t per_page)
{
    const char *page_key = "page";
    const char *per_key = "per_key";

    if (info != NULL) {
        if (info->page_key != NULL && info->page_key[0] != '\0') {
            page_key = info->page_key;
        }
        if (info->per_key != NULL && info->per_key[0] != '\0') {
            per_key = info->per_key;
        }
    }

    size_t page_key_len = strlen(page_key);
    size_t per_key_len  = strlen(per_key);

    if (base != NULL) {
        sb_append_url_attr_safe(sb, sv_from_cstr(base));
    }

    char sep = '?';
    String_View rest = query;
    while (rest.count > 0) {
        const char *amp  = memchr(rest.data, '&', rest.count);
        size_t plen      = rest.count;
        if (amp != NULL) {
            plen         = (size_t)(amp - rest.data);
        }
        String_View pair = { .data = rest.data, .count = plen };
        if (amp != NULL) {
            rest.data  += plen + 1;
            rest.count -= plen + 1;
        } else {
            rest.data  += plen;
            rest.count  = 0;
        }
        if (pair.count == 0) continue;

        const char *eq = memchr(pair.data, '=', pair.count);
        String_View name = {
            .data  = pair.data,
            .count = eq != NULL ? (size_t)(eq - pair.data) : pair.count,
        };
        if (name.count == page_key_len && memcmp(name.data, page_key, page_key_len) == 0) continue;
        if (name.count == per_key_len && memcmp(name.data, per_key, per_key_len) == 0) continue;
        if (name.count == 8 && memcmp(name.data, "fragment", 8) == 0) continue;

        sb_append_buf(sb, &sep, 1);
        sep = '&';
        sb_append_url_attr_safe(sb, pair);
    }

    sb_append_buf(sb, &sep, 1);
    sb_appendf(sb, "%s=%zu&%s=%zu", page_key, page, per_key, per_page);
}
