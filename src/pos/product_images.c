#include "product_images.h"

#include "src/layout/master_child.h"
#include "src/core/webc_template.h"
#include "src/db/db.h"
#include "src/pos/pos_util.h"
#include "src/http/utils.h"

// =========================================================================
// product_images rows: created/edited from the /pos/products Pictures
// child tab (product_id rides the form's query string).
//
// The fixed kind enum (MAIN/GALLERY/DETAIL/PACKAGE) is validated here on
// every dialect - the DB CHECK is enforced by sqlite and MySQL 8 but
// parsed and ignored by MariaDB. Create is hand-written (not SERVE_CREATE)
// because the shared macros can only 400 on a MISSING key or on values[0]:
// a picture without a file part, or with a garbage kind, must answer 400
// BEFORE any row is attempted. Update keeps the macro path - an untouched
// file input posts an empty part and COALESCE keeps the stored image.
// =========================================================================

static bool pim_kind_valid(String_View kind) {
    for (size_t i = 0; pim_kind_choices[i] != NULL; ++i) {
        if (sv_eq(kind, sv_from_cstr(pim_kind_choices[i]))) return true;
    }
    return false;
}

const char *pim_kind_choices[] = { "MAIN", "GALLERY", "DETAIL", "PACKAGE", NULL };

static bool create_product_image(db_t *db, String_View *fields, size_t count) {
    if (count < 5 || !pim_kind_valid(fields[0])) return false;
    static const char *const q[SQL_LANG_COUNT] = {
        [SQL_SQLITE]   = "INSERT INTO product_images "
                         "(kind, image, name, sort_order, product_id) "
                         "VALUES (?, ?, ?, ?, ?);",
        [SQL_MYSQL]    = "INSERT INTO product_images "
                         "(kind, image, name, sort_order, product_id) "
                         "VALUES (?, ?, ?, ?, ?);",
        [SQL_POSTGRES] = "INSERT INTO product_images "
                         "(kind, image, name, sort_order, product_id) "
                         "VALUES ($1, $2, $3, $4, $5);",
    };
    sql_stmt stmt = {0};
    bool result = true;
    if (!sql_prepare(db, q[db->lang], &stmt))     return_defer(false);
    if (!sql_bind(&stmt, 1, SQL_SV(fields[0])))   return_defer(false);  // kind
    if (!sql_bind(&stmt, 2, SQL_SV(fields[1])))   return_defer(false);  // image
    if (!sql_bind(&stmt, 3, sql_fgsv(fields[2]))) return_defer(false);  // name
    if (!sql_bind(&stmt, 4, sql_num(fields[3])))  return_defer(false);  // sort
    if (!sql_bind(&stmt, 5, sql_fgsv(fields[4]))) return_defer(false);  // product
    if (!sql_final_step(&stmt))                   return_defer(false);
defer:
    sql_finalize(&stmt);
    return result;
}

static bool update_product_image(db_t *db, String_View *fields, size_t count,
                                 String_View id)
{
    if (count < 5 || !pim_kind_valid(fields[0])) return false;
    static const char *const q[SQL_LANG_COUNT] = {
        [SQL_SQLITE]   = "UPDATE product_images SET "
                         "kind = COALESCE(NULLIF(?, ''), kind), "
                         "image = COALESCE(NULLIF(?, ''), image), "
                         "name = COALESCE(NULLIF(?, ''), name), "
                         "sort_order = COALESCE(NULLIF(?, ''), sort_order) "
                         "WHERE id = ?;",
        [SQL_MYSQL]    = "UPDATE product_images SET "
                         "kind = COALESCE(NULLIF(?, ''), kind), "
                         "image = COALESCE(NULLIF(?, ''), image), "
                         "name = COALESCE(NULLIF(?, ''), name), "
                         "sort_order = COALESCE(NULLIF(?, ''), sort_order) "
                         "WHERE id = ?;",
        [SQL_POSTGRES] = "UPDATE product_images SET "
                         "kind = COALESCE(NULLIF($1, ''), kind), "
                         "image = COALESCE(NULLIF($2, ''), image), "
                         "name = COALESCE(NULLIF($3, ''), name), "
                         "sort_order = COALESCE(NULLIF($4, ''), sort_order) "
                         "WHERE id = $5;",
    };
    sql_stmt stmt = {0};
    bool result = true;
    if (!sql_prepare(db, q[db->lang], &stmt)) return_defer(false);
    for (int i = 1; i <= 4; ++i) {
        if (!sql_bind(&stmt, i, SQL_SV(fields[i - 1]))) return_defer(false);
    }
    if (!sql_bind(&stmt, 5, SQL_SV(id))) return_defer(false);
    if (!sql_final_step(&stmt))          return_defer(false);
defer:
    sql_finalize(&stmt);
    return result;
}

static bool soft_delete_product_image(db_t *db, String_View id) {
    static const char *const q[SQL_LANG_COUNT] = {
        [SQL_SQLITE]   = "UPDATE product_images "
                         "SET deleted_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
                         "WHERE id = ?;",
        [SQL_MYSQL]    = "UPDATE product_images SET deleted_at = UTC_TIMESTAMP() "
                         "WHERE id = ?;",
        [SQL_POSTGRES] = "UPDATE product_images SET deleted_at = now() "
                         "WHERE id = $1;",
    };
    sql_stmt stmt = {0};
    bool result = true;
    if (!sql_prepare(db, q[db->lang], &stmt)) return_defer(false);
    if (!sql_bind(&stmt, 1, SQL_SV(id)))      return_defer(false);
    if (!sql_final_step(&stmt))               return_defer(false);
defer:
    sql_finalize(&stmt);
    return result;
}

static bool restore_product_image(db_t *db, String_View id) {
    static const char *const q[SQL_LANG_COUNT] = {
        [SQL_SQLITE]   = "UPDATE product_images SET deleted_at = NULL "
                         "WHERE id = ?;",
        [SQL_MYSQL]    = "UPDATE product_images SET deleted_at = NULL "
                         "WHERE id = ?;",
        [SQL_POSTGRES] = "UPDATE product_images SET deleted_at = NULL "
                         "WHERE id = $1;",
    };
    sql_stmt stmt = {0};
    bool result = true;
    if (!sql_prepare(db, q[db->lang], &stmt)) return_defer(false);
    if (!sql_bind(&stmt, 1, SQL_SV(id)))      return_defer(false);
    if (!sql_final_step(&stmt))               return_defer(false);
defer:
    sql_finalize(&stmt);
    return result;
}

// kind + image are required and must be NON-EMPTY on create (an untouched
// file input posts an empty part); name, sort_order and the product_id FK
// are optional (body first, then the child form's ?product_id= query).
static const char *pim_fields[] = { "kind", "image" };
static const char *pim_opt_fields[] = { "name", "sort_order", "product_id" };

void serve_pos_product_images_create(Serve_Context *sc) {
    size_t field_count = ARRAY_LEN(pim_fields) + ARRAY_LEN(pim_opt_fields);
    Nob_String_View values[field_count];
    Nob_String_View body_sv = sb_to_sv(sc->body);

    SERVE_EXTRACT_FIELDS(sc, body_sv, values, pim_fields, ARRAY_LEN(pim_fields));
    SERVE_EXTRACT_OPT_FIELDS(sc, body_sv, values, pim_opt_fields,
                             ARRAY_LEN(pim_opt_fields), ARRAY_LEN(pim_fields));

    // Present but empty = no picture uploaded (or a blank kind).
    if (values[1].count == 0 || !pim_kind_valid(values[0])) {
        serve_error(sc, 400);
        return;
    }

    db_t *db = open_webc_db();
    if (!db) { serve_error(sc, 500); return; }
    if (!sql_txn_begin(db)) {
        db_close(db);
        serve_error(sc, 500);
        return;
    }
    bool ok = create_product_image(db, values, field_count);
    if (ok) { sql_txn_commit(db); }
    else    { sql_txn_rollback(db); }
    db_close(db);

    if (!ok) { serve_error(sc, 500); return; }
    http_render_redirect(sc, 302,
                         http_redirect_target(sc, "/pos/products"));
}

SERVE_UPDATE(pos_product_images, product_image, pim_fields, pim_opt_fields)
SERVE_SOFT_DELETE(pos_product_images, product_image)
