#ifndef MASTER_CHILD_H_
#define MASTER_CHILD_H_

#include "../http/serve.h"
#include <stddef.h>
#include "../../src/db/sql.h"

// =========================================================================
// Master-child view interface.
//
// Stable API: modules compose their own MD_MasterConfig (column shapes come
// from the entity files) and hand it to serve_master_child(). Adding another
// master-child page never requires editing this header or its engine.
// =========================================================================

typedef struct {
    const char *id;     /* TEXT pk (legacy numeric string or uuid) */
    char       *label;
} MD_Option;

typedef enum {
    COL_TYPE_TEXT,
    COL_TYPE_NUM,
    COL_TYPE_DATE,
    COL_TYPE_TEXTAREA,
    COL_TYPE_FK_SELECT,
    COL_TYPE_IMG,
    COL_TYPE_COMPOSITE,
} Col_Type;

typedef struct MD_Column {
    const char       *name;
    const char       *label;
    Col_Type          type;
    int               nullable;
    const char       *fk_table;
    const char       *fk_label;
    const char       *fk_where;   /* raw WHERE fragment scoping the option query */
    const char       *fk_value;
    MD_Option        *opt;        /* runtime: FK dropdown options */
    size_t            opt_count;
    int               fk_cascade; /* leaf of a parent-scoped cascade */
    int               hidden;     /* never displayed; preserved via hidden input
                                     on edit */
    int               computed;   /* display-only value (server-derived);
                                     no form input */

    /* Sub-columns for composite cells */
    const char       *style;        /* "stack", "avatar", "activity" */
    struct MD_Column *subcols;      /* Array of child sub-columns */
    size_t            subcol_count; /* Number of sub-columns */
    const char       *read_sql;     /* Custom read/CASE SQL expression */
    const char      **choices;      /* NULL-terminated choice string list */
} MD_Column;

#define MD_FIELD(name_, label_, type_, ...) \
    ((MD_Column) {                          \
        .name  = (name_),                   \
        .label = (label_),                  \
        .type  = (type_), __VA_ARGS__ })

/* 4th arg is the display column (fk_label) used by the option query
 * `SELECT id, <4th> FROM <table>` and the row fallback lookup. Scope the
 * options with a raw WHERE fragment via `.fk_where = ...` in __VA_ARGS__
 * (later designated initializer wins), e.g. dict-backed selects:
 *   MD_FK("status_id", "Status", "dictionaries", "label",
 *         .fk_where = "category = 'ORDER_STATUS'", .nullable = false) */
#define MD_FK(name_, label_, table_, label_col_, ...)\
    ((MD_Column) {                                   \
       .name     = (name_),                          \
       .label    = (label_),                         \
       .type     = COL_TYPE_FK_SELECT,               \
       .fk_table = (table_),                         \
       .fk_label = (label_col_), __VA_ARGS__ })

#define MD_CHOICE(name_, label_, choices_, ...)\
    ((MD_Column) {                             \
        .name    = (name_),                    \
        .label   = (label_),                   \
        .type    = COL_TYPE_TEXT,              \
        .choices = (choices_), __VA_ARGS__ })

#define MD_STACK(label_, ...) {                   \
    .label        = (label_),                     \
    .type         = COL_TYPE_COMPOSITE,           \
    .style        = "stack",                      \
    .subcols      = (MD_Column[]) { __VA_ARGS__ },\
    .subcol_count = sizeof((MD_Column[]) { __VA_ARGS__ }) / sizeof(MD_Column) }

#define MD_AVATAR(label_, ...) {                  \
    .label        = (label_),                     \
    .type         = COL_TYPE_COMPOSITE,           \
    .style        = "avatar",                     \
    .subcols      = (MD_Column[]) { __VA_ARGS__ },\
    .subcol_count = sizeof((MD_Column[]) { __VA_ARGS__ }) / sizeof(MD_Column) }

#define MD_ACTIVITY(label_, ...) {                \
    .label        = (label_),                     \
    .type         = COL_TYPE_COMPOSITE,           \
    .style        = "activity",                   \
    .computed     = 1,                            \
    .subcols      = (MD_Column[]) { __VA_ARGS__ },\
    .subcol_count = sizeof((MD_Column[]){ __VA_ARGS__ }) / sizeof(MD_Column) }

/* values[0] is the row id; column ci starts at its first slot below. Flat
 * columns keep the historic values[ci + 1] identity; a composite column
 * spans subcol_count slots. */
static inline size_t md_col_slot(const MD_Column *cols, size_t ci) {
    size_t slot = 1;
    for (size_t i = 0; i < ci; ++i) {
        if (cols[i].subcols != NULL && cols[i].subcol_count > 0) {
            slot += cols[i].subcol_count;
        } else {
            slot += 1;
        }
    }
    return slot;
}

/* Is this FK id among the column's offered (LIVE-only) options? */
static inline int md_opt_has_id(const MD_Column *col, const char *id) {
    if (col == NULL || id == NULL || id[0] == '\0') {
        return 0;
    }
    for (size_t i = 0; i < col->opt_count; ++i) {
        if (strcmp(id, col->opt[i].id) == 0) {
            return 1;
        }
    }
    return 0;
}

typedef struct {
    char  **values;
    char  **disp;
    size_t  value_count;
} MD_ChildRow;

typedef struct {
    MD_ChildRow *items;
    size_t       count;
    size_t       capacity;
} MD_ChildRows;

typedef struct {
    const char  *table;
    const char  *title;
    const char  *fk_column;
    const char  *fk_subquery;
    const char  *id_column;
    const char  *crud_path;
    MD_Column   *columns;
    size_t       column_count;
    const char **sum_columns;
    size_t       sum_column_count;
    int          soft_delete;
} MD_ChildTab;

typedef struct {
    const char   *table;
    const char   *title;
    const char   *id_column;
    const char   *crud_path;   /* POST base: /create, /{id}/update|delete */
    MD_Column    *columns;
    size_t        column_count;
    const char  **sum_columns;
    size_t        sum_column_count;
    MD_ChildTab  *children;
    size_t        children_count;
    int           soft_delete;
    int           read_only;
    int           no_live;
    int           no_create;
    int           create_only;
} MD_MasterConfig;

typedef struct {
    MD_Column *col;
    MD_Option *items;
    size_t     count;
    size_t     capacity;
} MD_FormCol;

typedef struct {
    MD_FormCol *items;
    size_t      count;
    size_t      capacity;
} MD_FormCols;

typedef struct {
    const char    *id;
    char         **values;
    char         **disp;
    size_t         value_count;
    MD_ChildRows  *children;
    size_t         children_count;
} MD_MasterRow;

typedef struct {
    MD_MasterRow *items;
    size_t        count;
    size_t        capacity;
} MD_MasterRows;

const char *md_cascade_json(db_t *db, const MD_MasterRows *rows,
                            const MD_MasterConfig *config);

void serve_master_child(Serve_Context *sc, const MD_MasterConfig *config);

#endif // MASTER_CHILD_H_
