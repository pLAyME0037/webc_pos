#include "staff.h"

#include <string.h>

#include "src/layout/master_child.h"
#include "src/core/webc_template.h"
#include "src/db/db.h"
#include "src/pos/pos_util.h"
#include "src/pos/shifts.h"
#include "src/http/utils.h"

// =========================================================================
// /pos/staff: staff_code + computed Name (lives on users via user_id) +
// user/org/type FKs + hire_date/salary. Staff is a MASTER table: it
// renders only here, never as a child tab of /pos/users or /pos/org. The
// single create/update serves the one form body; user/org/type/hire/salary
// are opt fields (body first, query fallback) so a pre-0013 API body
// without them keeps working. Person names/phone/village were dropped
// from staff in the 0003 schema fix - they live on the linked users row.
// =========================================================================

// Name: read-only scalar subquery over the linked user (computed = no
// form input in either form; the row query and ?q_name search both read
// this expression through md_col_sql). One sub-column so the stack
// renderer draws a single line and the slot layout stays value_count = 2.
MD_Column md_staff_columns[] = {
    { .name = "staff_code", .label = "Code", .type = COL_TYPE_TEXT,
      .nullable = false },
    {
        .label        = "Name",
        .type         = COL_TYPE_COMPOSITE,
        .style        = "stack",
        .computed     = 1,
        .nullable     = true,
        .subcols      = (MD_Column[]) {
            {
                .name      = "name",
                .label     = "Name",
                .type      = COL_TYPE_TEXT,
                .read_sql  = "(SELECT u.name FROM users u "
                             "WHERE u.id = staff.user_id)",
            },
        },
        .subcol_count = 1,
    },
    { .name = "user_id", .label = "User", .type = COL_TYPE_FK_SELECT,
      .nullable = false, .fk_table = "users", .fk_label = "name" },
    { .name = "org_unit_id", .label = "Org", .type = COL_TYPE_FK_SELECT,
      .nullable = false, .fk_table = "org_units", .fk_label = "ou_name" },
    { .name = "staff_type_dict_id", .label = "Role", .type = COL_TYPE_FK_SELECT,
      .nullable = true, .fk_table = "dictionaries", .fk_label = "label",
      .fk_where = "category = 'STAFF_TYPE'" },
    { .name = "hire_date", .label = "Hired", .type = COL_TYPE_DATE,
      .nullable = true },
    { .name = "salary", .label = "Salary", .type = COL_TYPE_NUM,
      .nullable = true },
};
const size_t md_staff_columns_count = ARRAY_LEN(md_staff_columns);

// DB mutations only -- list loading lives in the master_child engine.
// Column order: (user_id, org_unit_id, staff_code, staff_type_dict_id,
// hire_date, salary).
static bool create_staff_member(db_t *db, String_View *fields, size_t count) {
    if (count < 6) return false;
    static const char *const q[SQL_LANG_COUNT] = {
        [SQL_SQLITE]   = "INSERT INTO staff "
                         "(user_id, org_unit_id, staff_code, "
                          "staff_type_dict_id, hire_date, salary) "
                         "VALUES (?, ?, ?, ?, ?, ?);",
        [SQL_MYSQL]    = "INSERT INTO staff "
                         "(user_id, org_unit_id, staff_code, "
                          "staff_type_dict_id, hire_date, salary) "
                         "VALUES (?, ?, ?, ?, ?, ?);",
        [SQL_POSTGRES] = "INSERT INTO staff "
                         "(user_id, org_unit_id, staff_code, "
                          "staff_type_dict_id, hire_date, salary) "
                         "VALUES ($1, $2, $3, $4, $5, $6);",
    };
    sql_stmt stmt = {0};
    bool result = true;
    if (!sql_prepare(db, q[db->lang], &stmt)) return_defer(false);
    if (!sql_bind(&stmt, 1, sql_fgsv(fields[1]))) return_defer(false);  // user
    if (!sql_bind(&stmt, 2, sql_fgsv(fields[2]))) return_defer(false);  // org
    if (!sql_bind(&stmt, 3, SQL_SV(fields[0]))) return_defer(false);  // code
    if (!sql_bind(&stmt, 4, sql_fgsv(fields[3]))) return_defer(false);  // type
    if (!sql_bind(&stmt, 5, sql_fgsv(fields[4]))) return_defer(false);  // hire
    if (!sql_bind(&stmt, 6, sql_fgsv(fields[5]))) return_defer(false);  // salary
    if (!sql_final_step(&stmt))                 return_defer(false);
defer:
    sql_finalize(&stmt);
    return result;
}

static bool update_staff_member(db_t *db, String_View *fields, size_t count,
                                String_View id)
{
    if (count < 6) return false;
    // Every set is COALESCE: fields missing from a view's body (type,
    // hire_date, salary) keep their stored values; the two fk opts come
    // from body (master) or query (child) -- both carry the same value.
    static const char *const q[SQL_LANG_COUNT] = {
        [SQL_SQLITE]   = "UPDATE staff SET "
                         "staff_code = COALESCE(NULLIF(?, ''), staff_code), "
                         "org_unit_id = COALESCE(NULLIF(?, ''), org_unit_id), "
                         "user_id = COALESCE(NULLIF(?, ''), user_id), "
                         "staff_type_dict_id = COALESCE(NULLIF(?, ''), "
                         "staff_type_dict_id), "
                         "hire_date = COALESCE(NULLIF(?, ''), hire_date), "
                         "salary = COALESCE(NULLIF(?, ''), salary) "
                         "WHERE id = ?;",
        [SQL_MYSQL]    = "UPDATE staff SET "
                         "staff_code = COALESCE(NULLIF(?, ''), staff_code), "
                         "org_unit_id = COALESCE(NULLIF(?, ''), org_unit_id), "
                         "user_id = COALESCE(NULLIF(?, ''), user_id), "
                         "staff_type_dict_id = COALESCE(NULLIF(?, ''), "
                         "staff_type_dict_id), "
                         "hire_date = COALESCE(NULLIF(?, ''), hire_date), "
                         "salary = COALESCE(NULLIF(?, ''), salary) "
                         "WHERE id = ?;",
        [SQL_POSTGRES] = "UPDATE staff SET "
                         "staff_code = COALESCE(NULLIF($1, ''), staff_code), "
                         "org_unit_id = COALESCE(NULLIF($2, ''), org_unit_id), "
                         "user_id = COALESCE(NULLIF($3, ''), user_id), "
                         "staff_type_dict_id = COALESCE(NULLIF($4, ''), "
                         "staff_type_dict_id), "
                         "hire_date = COALESCE(NULLIF($5, ''), hire_date), "
                         "salary = COALESCE(NULLIF($6, ''), salary) "
                         "WHERE id = $7;",
    };
    sql_stmt stmt = {0};
    bool result = true;
    if (!sql_prepare(db, q[db->lang], &stmt))     return_defer(false);
    if (!sql_bind(&stmt, 1, SQL_SV(fields[0])))   return_defer(false);  // code
    if (!sql_bind(&stmt, 2, sql_fgsv(fields[2]))) return_defer(false);  // org
    if (!sql_bind(&stmt, 3, sql_fgsv(fields[1]))) return_defer(false);  // user
    if (!sql_bind(&stmt, 4, sql_fgsv(fields[3]))) return_defer(false);  // type
    if (!sql_bind(&stmt, 5, sql_fgsv(fields[4]))) return_defer(false);  // hire
    if (!sql_bind(&stmt, 6, sql_fgsv(fields[5]))) return_defer(false);  // salary
    if (!sql_bind(&stmt, 7, SQL_SV(id)))          return_defer(false);
    if (!sql_final_step(&stmt))                   return_defer(false);
defer:
    sql_finalize(&stmt);
    return result;
}

static bool soft_delete_staff_member(db_t *db, String_View id) {
    static const char *const q[SQL_LANG_COUNT] = {
        [SQL_SQLITE]   = "UPDATE staff "
                         "SET deleted_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
                         "WHERE id = ?;",
        [SQL_MYSQL]    = "UPDATE staff SET deleted_at = UTC_TIMESTAMP() WHERE id = ?;",
        [SQL_POSTGRES] = "UPDATE staff SET deleted_at = now() WHERE id = $1;",
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

static bool restore_staff_member(db_t *db, String_View id) {
    static const char *const q[SQL_LANG_COUNT] = {
        [SQL_SQLITE]   = "UPDATE staff SET deleted_at = NULL WHERE id = ?;",
        [SQL_MYSQL]    = "UPDATE staff SET deleted_at = NULL WHERE id = ?;",
        [SQL_POSTGRES] = "UPDATE staff SET deleted_at = NULL WHERE id = $1;",
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

void serve_pos_staff(Serve_Context *sc) {
    MD_ChildTab children[] = {
        {
            .table        = "cash",
            .title        = "Shifts",
            .fk_column    = "staff_id",
            .id_column    = "id",
            .crud_path    = "/pos/shifts",
            .columns      = md_shifts_child_columns,
            .column_count = md_shifts_child_columns_count,
            .soft_delete  = 1,
        },
    };
    MD_MasterConfig config = {
        .table          = "staff",
        .title          = "Staff",
        .id_column      = "id",
        .crud_path      = "/pos/staff",
        .columns        = md_staff_columns,
        .column_count   = md_staff_columns_count,
        .children       = children,
        .children_count = ARRAY_LEN(children),
        .soft_delete    = 1,
    };
    serve_master_child(sc, &config);
}

// body keys first (present in all three views), query-driven fks after.
// No name/phone/village keys: person data lives on the linked users row
// (computed Name column reads it), employment facts only here.
static const char *stf_fields[] = {
    "staff_code",
};
static const char *stf_opt_fields[] = {
    "user_id", "org_unit_id", "staff_type_dict_id", "hire_date", "salary",
};
SERVE_CREATE(pos_staff, staff_member, stf_fields, stf_opt_fields)
SERVE_UPDATE(pos_staff, staff_member, stf_fields, stf_opt_fields)
SERVE_SOFT_DELETE(pos_staff, staff_member)
