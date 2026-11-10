#ifndef HELPER_CELLS_H_
#define HELPER_CELLS_H_

#include "src/layout/master_child.h"

// =========================================================================
// Composite column cell renderers (master_child engine).
// Strings are temp-arena owned (escape first, never free).
// =========================================================================

/* HTML body for a composite cell: values[] at slot, subcol_count slots. */
const char *md_cell_render(const MD_Column *col, char *const *values, size_t slot);

/* Label for edit/create inputs of sub-column `part`. */
const char *md_cell_part_label(const MD_Column *col, size_t part);

/* ISO stamp -> "05 Oct 2026, 11:56" (non-ISO passes through). */
const char *md_date_human(const char *iso);

/* ISO stamp -> "YYYY-MM-DD" for <input type="date"> prefills. */
const char *md_date_input(const char *iso);

#endif // !HELPER_CELLS_H_
