#include "customers.h"

#include <string.h>

#include "src/layout/master_child.h"
#include "src/pos/pos.h"     // md_pos_users_columns (users child tab)
#include "src/core/webc_template.h"
#include "src/db/db.h"
#include "src/pos/pos_util.h"
#include "src/http/utils.h"

// =========================================================================
// /pos/customers: customer_type dict FK (CUSTOMER_TYPE only) + loyalty
// points + created_at. Children: users (fk customer_id) +
// customer_interactions (fk customer_id).
// =========================================================================

// Event cell: plain fields only (user_id stays a flat FK column so its
// label -- not the raw id -- renders in the row).
static MD_Column md_interaction_columns[] = {
    { .name = "user_id", .label = "User", .type = COL_TYPE_FK_SELECT,
      .nullable = true, .fk_table = "users", .fk_label = "name" },
    MD_STACK("Event",
        MD_FIELD("interaction_type", "Kind", COL_TYPE_TEXT, .nullable = false),
        MD_FIELD("raw_payload", "Payload", COL_TYPE_TEXT, .nullable = false)
    ),
};
static const size_t md_interaction_columns_count =
    ARRAY_LEN(md_interaction_columns);

MD_Column md_customers_columns[] = {
    { .name = "customer_type_dict_id", .label = "Tier",
      .type = COL_TYPE_FK_SELECT, .nullable = false,
      .fk_table = "dictionaries", .fk_label = "label",
      .fk_where = "category = 'CUSTOMER_TYPE'" },
    { .name = "loyalty_points", .label = "Points", .type = COL_TYPE_NUM,
      .nullable = false },
    { .name = "created_at", .label = "Since", .type = COL_TYPE_DATE,
      .nullable = false, .computed = 1 },
};
const size_t md_customers_columns_count = ARRAY_LEN(md_customers_columns);

// DB mutations only -- list loading lives in the master_child engine.
static bool create_customer(db_t *db, String_View *fields, size_t count) {
    if (count < 2) return false;
    static const char *const q[SQL_LANG_COUNT] = {
        [SQL_SQLITE]   = "INSERT INTO customers "
                         "(customer_type_dict_id, loyalty_points) "
                         "VALUES (?, ?);",
        [SQL_MYSQL]    = "INSERT INTO customers "
                         "(customer_type_dict_id, loyalty_points) "
                         "VALUES (?, ?);",
        [SQL_POSTGRES] = "INSERT INTO customers "
                         "(customer_type_dict_id, loyalty_points) "
                         "VALUES ($1, $2);",
    };
    sql_stmt stmt = {0};
    bool result = true;
    if (!sql_prepare(db, q[db->lang], &stmt))     return_defer(false);
    if (!sql_bind(&stmt, 1, SQL_SV(fields[0])))   return_defer(false);  // tier
    if (!sql_bind(&stmt, 2, sql_num(fields[1])))  return_defer(false);  // points
    if (!sql_final_step(&stmt))                   return_defer(false);
defer:
    sql_finalize(&stmt);
    return result;
}

static bool update_customer(db_t *db, String_View *fields, size_t count,
                            String_View id)
{
    if (count < 2) return false;
    static const char *const q[SQL_LANG_COUNT] = {
        [SQL_SQLITE]   = "UPDATE customers SET "
                         "customer_type_dict_id = COALESCE(NULLIF(?, ''), "
                         "customer_type_dict_id), "
                         "loyalty_points = COALESCE(NULLIF(?, ''), loyalty_points) "
                         "WHERE id = ?;",
        [SQL_MYSQL]    = "UPDATE customers SET "
                         "customer_type_dict_id = COALESCE(NULLIF(?, ''), "
                         "customer_type_dict_id), "
                         "loyalty_points = COALESCE(NULLIF(?, ''), loyalty_points) "
                         "WHERE id = ?;",
        [SQL_POSTGRES] = "UPDATE customers SET "
                         "customer_type_dict_id = COALESCE(NULLIF($1, ''), "
                         "customer_type_dict_id), "
                         "loyalty_points = COALESCE(NULLIF($2, ''), loyalty_points) "
                         "WHERE id = $3;",
    };
    sql_stmt stmt = {0};
    bool result = true;
    if (!sql_prepare(db, q[db->lang], &stmt)) return_defer(false);
    if (!sql_bind(&stmt, 1, SQL_SV(fields[0])))  return_defer(false);
    if (!sql_bind(&stmt, 2, SQL_SV(fields[1])))  return_defer(false);
    if (!sql_bind(&stmt, 3, SQL_SV(id)))         return_defer(false);
    if (!sql_final_step(&stmt))                  return_defer(false);
defer:
    sql_finalize(&stmt);
    return result;
}

static bool soft_delete_customer(db_t *db, String_View id) {
    static const char *const q[SQL_LANG_COUNT] = {
        [SQL_SQLITE]   = "UPDATE customers "
                         "SET deleted_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
                         "WHERE id = ?;",
        [SQL_MYSQL]    = "UPDATE customers SET deleted_at = UTC_TIMESTAMP() WHERE id = ?;",
        [SQL_POSTGRES] = "UPDATE customers SET deleted_at = now() WHERE id = $1;",
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

static bool restore_customer(db_t *db, String_View id) {
    static const char *const q[SQL_LANG_COUNT] = {
        [SQL_SQLITE]   = "UPDATE customers SET deleted_at = NULL WHERE id = ?;",
        [SQL_MYSQL]    = "UPDATE customers SET deleted_at = NULL WHERE id = ?;",
        [SQL_POSTGRES] = "UPDATE customers SET deleted_at = NULL WHERE id = $1;",
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

void serve_pos_customers(Serve_Context *sc) {
    MD_ChildTab children[] = {
        {
            .table        = "users",
            .title        = "Users",
            .fk_column    = "customer_id",
            .id_column    = "id",
            .crud_path    = "/pos/users",
            .columns      = md_pos_users_columns,
            .column_count = md_pos_users_columns_count,
            .soft_delete  = 1,
        },
        {
            .table        = "customer_interactions",
            .title        = "Activity",
            .fk_column    = "customer_id",
            .id_column    = "id",
            .crud_path    = "/pos/customer_interactions",
            .columns      = md_interaction_columns,
            .column_count = md_interaction_columns_count,
            .soft_delete  = 1,
        },
    };
    MD_MasterConfig config = {
        .table          = "customers",
        .title          = "Customers",
        .id_column      = "id",
        .crud_path      = "/pos/customers",
        .columns        = md_customers_columns,
        .column_count   = md_customers_columns_count,
        .children       = children,
        .children_count = ARRAY_LEN(children),
        .soft_delete    = 1,
    };
    serve_master_child(sc, &config);
}

static const char *cst_fields[] = { "customer_type_dict_id", "loyalty_points" };
static const char *cst_opt_fields[] = {};
SERVE_CREATE(pos_customers, customer, cst_fields, cst_opt_fields)
SERVE_UPDATE(pos_customers, customer, cst_fields, cst_opt_fields)
SERVE_SOFT_DELETE(pos_customers, customer)
