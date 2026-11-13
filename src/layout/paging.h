#ifndef CORE_DISPLAY_PAGING_H_
#define CORE_DISPLAY_PAGING_H_

#include "third_party/nob/nob.h"
#include "src/http/serve.h"   // Serve_Context (session per_page cookie)
#include <stdbool.h>
#include <stddef.h>

#define PAGE_DEFAULT_PER_PAGE 20
#define PAGE_MAX_PER_PAGE     500

typedef struct {
    const char *page_key;    // query key for page number,
    const char *per_key;     // query key for page size.
    size_t      page;        // 1-based, clamped to [1, total_pages]
    size_t      per_page;    // clamped to [1, PAGE_MAX_PER_PAGE]
    size_t      offset;      // 0-based SQL OFFSET: (page - 1) * per_page
    size_t      row_start;   // 1-based first display row (0 when total = 0)
    size_t      row_end;     // 1-based last display row (0 when total = 0)
    size_t      total;       // row count, set by page_info_finish()
    size_t      total_pages; // ceil(total/per_page), always >= 1
} Page_Info;

// Parse ?<page_key> and ?<per_key> out of `query` (invalid -> defaults).
void page_info_parse(Page_Info *out, String_View query,
                     const char *page_key, const char *per_key);

// Global session size variant of page_info_parse(): a valid ?per_page= in
// `query` wins AND is saved to the webc_per_page cookie; otherwise the saved
// cookie applies; otherwise the default. Child per-table keys
// (?<table>_per_page) keep page_info_parse() and never touch the cookie.
void page_info_parse_session(Page_Info *out, Serve_Context *sc,
                             String_View query, const char *page_key);

// Call once total row count is known: computes total_pages, clamps page,
// and fills offset, row_start, and row_end.
void page_info_finish(Page_Info *out, size_t total);

// True when a single-page HTML fragment (?fragment=1) was requested.
bool page_fragment_requested(String_View query);

// Append `<base>?<page_key>=<page>&<per_key>=<per_page>` to
// `sb`, keeping other parameters from `query` (excluding page_key, per_key, and
// fragment) with unsafe HTML/attribute characters percent-encoded.
void page_href(String_Builder *sb, const char *base, String_View query,
               const Page_Info *info, size_t page, size_t per_page);

#endif // CORE_DISPLAY_PAGING_H_
