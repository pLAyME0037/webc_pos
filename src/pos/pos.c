#include <string.h>

#include "pos.h"
#include "src/layout/master_child.h"

#include "src/core/webc_template.h"
#include "src/db/db.h"
#include "src/pos/pos_util.h"
#include "src/layout/header.h"
#include "src/layout/footer.h"
#include "src/http/utils.h"

// =========================================================================
// /pos/users: avatar cell (profile_pic, name, username) + email.
// Column shapes are separate from the demo md_users_columns so the demo
// pages do not move. 
// =========================================================================

// Avatar/status/activity shapes live inline in md_pos_users_columns
// below (MD_AVATAR/MD_STACK/MD_ACTIVITY): picture + first/last name +
// username + account status, type & gender stack, and the display-only
// activity stamps. Status/gender are INTEGER in storage (0003) - each
// sub-column's read_sql maps them back to the label the form posts.
MD_Column md_pos_users_columns[] = {
    MD_AVATAR("User",
        MD_FIELD("profile_pic", "Photo", COL_TYPE_IMG, .nullable = true),
        MD_FIELD("first_name", "First Name", COL_TYPE_TEXT, .nullable = false),
        MD_FIELD("last_name", "Last Name", COL_TYPE_TEXT, .nullable = false),
        MD_FIELD("username", "Username", COL_TYPE_TEXT, .nullable = false),
        MD_CHOICE("status", "Status", ((const char*[]) {
            "ACTIVE",
            "INACTIVE",
            "SUSPENDED",
            NULL
        }),
                  .read_sql = "CASE status WHEN 1 THEN 'ACTIVE' WHEN 0 THEN 'INACTIVE' ELSE 'SUSPENDED' END")
    ),
    MD_STACK("Type & Gender",
        MD_CHOICE("gender", "Gender", ((const char*[]) {
            "MALE",
            "FEMALE",
            "NON",
            NULL
        }),
                  .read_sql = "CASE gender WHEN 1 THEN 'MALE' WHEN 0 THEN 'FEMALE' ELSE 'NON' END"),
        MD_FK("user_type_dict_id", "Type", "dictionaries", "label",
              .fk_where = "category = 'USER_TYPE'", .nullable = false)
    ),
    MD_FIELD("phone", "Phone", COL_TYPE_TEXT, .nullable = false),
    MD_FIELD("email", "Email", COL_TYPE_TEXT, .nullable = true),
    MD_FK("org_unit_id", "Branch", "org_units", "ou_name", .nullable = true),
    MD_FK("customer_id", "Customer", "customers", "id", .nullable = true),
    MD_FK("village_id", "Village", "villages", "name_kh", .nullable = true, .fk_cascade = 1),
    MD_ACTIVITY("Activity",
        MD_FIELD("created_at", "Joined", COL_TYPE_DATE),
        MD_FIELD("updated_at", "Edited", COL_TYPE_DATE),
        MD_FIELD("deleted_at", "Deleted", COL_TYPE_DATE)
    ),
};
const size_t md_pos_users_columns_count = ARRAY_LEN(md_pos_users_columns);

static bool create_pos_user(db_t *db, String_View *fields, size_t count) {
    if (count < 12) return false;
    // Plain INSERT (not OR REPLACE): a colliding username/phone must fail
    // loudly instead of silently dropping the other row. Field order is
    // usr_fields {phone, first_name, last_name, username, email} then
    // usr_opt_fields {profile_pic, status, customer_id, org_unit_id,
    // village_id, user_type_dict_id, gender}. phone (values[0]) must be
    // non-empty - the SERVE_* 400 guard enforces it. email/profile_pic/
    // fks store NULL when blank (0007: email is optional, only filled on
    // deliberate account creation); status/gender are the 0003 INTEGER
    // enums: the form posts the label, the CASE stores 1/0/-1, and an
    // absent key falls back to the column default (1 = ACTIVE / MALE).
    static const char *const q[SQL_LANG_COUNT] = {
        [SQL_SQLITE]   = "INSERT INTO users "
                         "(phone, first_name, last_name, username, email, "
                          "profile_pic, status, customer_id, org_unit_id, "
                          "village_id, user_type_dict_id, gender) "
                         "VALUES (?, ?, ?, ?, NULLIF(?, ''), NULLIF(?, ''), "
                          "CASE NULLIF(?, '') WHEN 'ACTIVE' THEN 1 "
                          "WHEN 'INACTIVE' THEN 0 WHEN 'SUSPENDED' THEN -1 "
                          "ELSE 1 END, "
                          "NULLIF(?, ''), NULLIF(?, ''), NULLIF(?, ''), "
                          "COALESCE(NULLIF(?, ''), 'CUSTOMER'), "
                          "CASE NULLIF(?, '') WHEN 'MALE' THEN 1 "
                          "WHEN 'FEMALE' THEN 0 WHEN 'NON' THEN -1 "
                          "ELSE 1 END);",
        [SQL_MYSQL]    = "INSERT INTO users "
                         "(phone, first_name, last_name, username, email, "
                          "profile_pic, status, customer_id, org_unit_id, "
                          "village_id, user_type_dict_id, gender) "
                         "VALUES (?, ?, ?, ?, NULLIF(?, ''), NULLIF(?, ''), "
                          "CASE NULLIF(?, '') WHEN 'ACTIVE' THEN 1 "
                          "WHEN 'INACTIVE' THEN 0 WHEN 'SUSPENDED' THEN -1 "
                          "ELSE 1 END, "
                          "NULLIF(?, ''), NULLIF(?, ''), NULLIF(?, ''), "
                          "COALESCE(NULLIF(?, ''), 'CUSTOMER'), "
                          "CASE NULLIF(?, '') WHEN 'MALE' THEN 1 "
                          "WHEN 'FEMALE' THEN 0 WHEN 'NON' THEN -1 "
                          "ELSE 1 END);",
        [SQL_POSTGRES] = "INSERT INTO users "
                         "(phone, first_name, last_name, username, email, "
                          "profile_pic, status, customer_id, org_unit_id, "
                          "village_id, user_type_dict_id, gender) "
                         "VALUES ($1, $2, $3, $4, NULLIF($5, ''), NULLIF($6, ''), "
                          "CASE NULLIF($7, '') WHEN 'ACTIVE' THEN 1 "
                          "WHEN 'INACTIVE' THEN 0 WHEN 'SUSPENDED' THEN -1 "
                          "ELSE 1 END, "
                          "NULLIF($8, ''), NULLIF($9, ''), NULLIF($10, ''), "
                          "COALESCE(NULLIF($11, ''), 'CUSTOMER'), "
                          "CASE NULLIF($12, '') WHEN 'MALE' THEN 1 "
                          "WHEN 'FEMALE' THEN 0 WHEN 'NON' THEN -1 "
                          "ELSE 1 END);",
    };
    sql_stmt stmt = {0};
    bool result = true;
    if (!sql_prepare(db, q[db->lang], &stmt)) return_defer(false);
    for (int i = 1; i <= 12; ++i) {
        if (!sql_bind(&stmt, i, SQL_SV(fields[i - 1]))) return_defer(false);
    }
    if (!sql_final_step(&stmt)) return_defer(false);
defer:
    sql_finalize(&stmt);
    return result;
}

static bool update_pos_user(db_t *db, String_View *fields, size_t count,
                            String_View id)
{
    if (count < 12) return false;
    // Empty phone/profile_pic/status/fks keep the stored one (same
    // COALESCE rule as /users): a child edit form carries only the shared
    // columns, its fk rides the query string, while a master edit sends
    // both. email is the one contact an edit may clear - it stores NULL
    // (0007: optional, only filled on deliberate account creation).
    // status/gender post their labels; the CASE maps back to the stored
    // 1/0/-1 and an empty key keeps the stored value.
    static const char *const q[SQL_LANG_COUNT] = {
        [SQL_SQLITE]   = "UPDATE users SET phone = COALESCE(NULLIF(?, ''), phone), "
                         "first_name = ?, last_name = ?, username = ?, "
                         "email = NULLIF(?, ''), "
                         "profile_pic = COALESCE(NULLIF(?, ''), profile_pic), "
                         "status = CASE NULLIF(?, '') WHEN 'ACTIVE' THEN 1 "
                          "WHEN 'INACTIVE' THEN 0 WHEN 'SUSPENDED' THEN -1 "
                          "ELSE status END, "
                         "customer_id = COALESCE(NULLIF(?, ''), customer_id), "
                         "org_unit_id = COALESCE(NULLIF(?, ''), org_unit_id), "
                         "village_id = COALESCE(NULLIF(?, ''), village_id), "
                         "user_type_dict_id = COALESCE(NULLIF(?, ''), user_type_dict_id), "
                         "gender = CASE NULLIF(?, '') WHEN 'MALE' THEN 1 "
                          "WHEN 'FEMALE' THEN 0 WHEN 'NON' THEN -1 "
                          "ELSE gender END "
                         "WHERE id = ?;",
        [SQL_MYSQL]    = "UPDATE users SET phone = COALESCE(NULLIF(?, ''), phone), "
                         "first_name = ?, last_name = ?, username = ?, "
                         "email = NULLIF(?, ''), "
                         "profile_pic = COALESCE(NULLIF(?, ''), profile_pic), "
                         "status = CASE NULLIF(?, '') WHEN 'ACTIVE' THEN 1 "
                          "WHEN 'INACTIVE' THEN 0 WHEN 'SUSPENDED' THEN -1 "
                          "ELSE status END, "
                         "customer_id = COALESCE(NULLIF(?, ''), customer_id), "
                         "org_unit_id = COALESCE(NULLIF(?, ''), org_unit_id), "
                         "village_id = COALESCE(NULLIF(?, ''), village_id), "
                         "user_type_dict_id = COALESCE(NULLIF(?, ''), user_type_dict_id), "
                         "gender = CASE NULLIF(?, '') WHEN 'MALE' THEN 1 "
                          "WHEN 'FEMALE' THEN 0 WHEN 'NON' THEN -1 "
                          "ELSE gender END "
                         "WHERE id = ?;",
        [SQL_POSTGRES] = "UPDATE users SET phone = COALESCE(NULLIF($1, ''), phone), "
                         "first_name = $2, last_name = $3, username = $4, "
                         "email = NULLIF($5, ''), "
                         "profile_pic = COALESCE(NULLIF($6, ''), profile_pic), "
                         "status = CASE NULLIF($7, '') WHEN 'ACTIVE' THEN 1 "
                          "WHEN 'INACTIVE' THEN 0 WHEN 'SUSPENDED' THEN -1 "
                          "ELSE status END, "
                         "customer_id = COALESCE(NULLIF($8, ''), customer_id), "
                         "org_unit_id = COALESCE(NULLIF($9, ''), org_unit_id), "
                         "village_id = COALESCE(NULLIF($10, ''), village_id), "
                         "user_type_dict_id = COALESCE(NULLIF($11, ''), user_type_dict_id), "
                         "gender = CASE NULLIF($12, '') WHEN 'MALE' THEN 1 "
                          "WHEN 'FEMALE' THEN 0 WHEN 'NON' THEN -1 "
                          "ELSE gender END "
                         "WHERE id = $13;",
    };
    sql_stmt stmt = {0};
    bool result = true;
    if (!sql_prepare(db, q[db->lang], &stmt))    return_defer(false);
    for (int i = 1; i <= 12; ++i) {
        if (!sql_bind(&stmt, i, SQL_SV(fields[i - 1]))) return_defer(false);
    }
    if (!sql_bind(&stmt, 13, SQL_SV(id)))        return_defer(false);
    if (!sql_final_step(&stmt))                  return_defer(false);
defer:
    sql_finalize(&stmt);
    return result;
}

// Rows referenced by roles/staff hit FK RESTRICT on a hard delete; with
// soft_delete the UPDATE only stamps deleted_at (cascade trigger covers
// user_roles/system_alerts/customer_interactions).
static bool soft_delete_pos_user(db_t *db, String_View id) {
    static const char *const q[SQL_LANG_COUNT] = {
        [SQL_SQLITE]   = "UPDATE users "
                         "SET deleted_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
                         "WHERE id = ?;",
        [SQL_MYSQL]    = "UPDATE users SET deleted_at = UTC_TIMESTAMP() WHERE id = ?;",
        [SQL_POSTGRES] = "UPDATE users SET deleted_at = now() WHERE id = $1;",
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

static bool restore_pos_user(db_t *db, String_View id) {
    static const char *const q[SQL_LANG_COUNT] = {
        [SQL_SQLITE]   = "UPDATE users SET deleted_at = NULL WHERE id = ?;",
        [SQL_MYSQL]    = "UPDATE users SET deleted_at = NULL WHERE id = ?;",
        [SQL_POSTGRES] = "UPDATE users SET deleted_at = NULL WHERE id = $1;",
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

// role links under a user: role_id FK + link timestamp.
static MD_Column md_pos_user_role_columns[] = {
    { .name = "role_id", .label = "Role", .type = COL_TYPE_FK_SELECT,
      .nullable = false, .fk_table = "roles", .fk_label = "role_name" },
    { .name = "created_at", .label = "Linked", .type = COL_TYPE_DATE,
      .nullable = false, .computed = 1 },
};
static const size_t md_pos_user_role_columns_count = ARRAY_LEN(md_pos_user_role_columns);

void serve_pos_users(Serve_Context *sc) {
    // Children: the editable relations of an account (role links). Staff
    // is a master table - it renders only on /pos/staff, never as another
    // table's child (Phase 13).
    MD_ChildTab children[] = {
        {
            .table        = "user_roles",
            .title        = "Roles",
            .fk_column    = "user_id",
            .id_column    = "id",
            .crud_path    = "/pos/user_roles",
            .columns      = md_pos_user_role_columns,
            .column_count = md_pos_user_role_columns_count,
            .soft_delete  = 1,
        },
    };
    MD_MasterConfig config = {
        .table          = "users",
        .title          = "Users",
        .id_column      = "id",
        .crud_path      = "/pos/users",
        .columns        = md_pos_users_columns,
        .column_count   = md_pos_users_columns_count,
        .children       = children,
        .children_count = ARRAY_LEN(children),
        .soft_delete    = 1,
    };
    serve_master_child(sc, &config);
}

// Required keys first: phone MUST be present and non-empty (values[0]
// drives the SERVE_* 400), then the identity fields the email input
// always send. profile_pic rides the optional block: the master form
// renders it as a file input (COL_TYPE_IMG), and an empty file input is
// ABSENT from the POST - treating it as required turned every no-file
// create/update into a 400. Absent/empty keeps the stored pic (COALESCE
// below) or stores NULL on insert, which is what create_pos_user always
// documented. Optional keys ride body first, query second - the
// /pos/customers and /pos/org child tabs pass their fk in the query
// string; status/location/user_type default when absent (see SQL above).
// Bind order stays phone,first_name,last_name,username,email,profile_pic,
// status,... : the opt block appends in list order, so profile_pic must
// lead it.
static const char *usr_fields[] = {
    "phone", "first_name", "last_name", "username", "email",
};
static const char *usr_opt_fields[] = {
    "profile_pic", "status", "customer_id", "org_unit_id", "village_id",
    "user_type_dict_id", "gender",
};
SERVE_CREATE(pos_users, pos_user, usr_fields, usr_opt_fields)
SERVE_UPDATE(pos_users, pos_user, usr_fields, usr_opt_fields)
SERVE_SOFT_DELETE(pos_users, pos_user)

// ---- /pos/dictionaries -----------------------------

MD_Column md_dictionaries_columns[] = {
    { .name = "category",   .label = "Category", .type = COL_TYPE_TEXT, .nullable = false },
    { .name = "code",       .label = "Code",     .type = COL_TYPE_TEXT, .nullable = false },
    { .name = "label",      .label = "Label",    .type = COL_TYPE_TEXT, .nullable = false },
    { .name = "sort_order", .label = "Sort",     .type = COL_TYPE_NUM,  .nullable = false },
};
const size_t md_dictionaries_columns_count = ARRAY_LEN(md_dictionaries_columns);

// Required keys first: category drives the SERVE_CREATE 400 guard; the
// sort key is optional and defaults to 0 (pos_num: empty -> SQL_I(0)),
// matching the column DEFAULT on both dialects. The macro pastes
// create_ + singular -> create_pos_dict and generates the route handler
// serve_pos_dict_create.
static const char *dict_fields[]     = { "category", "code", "label" };
static const char *dict_opt_fields[] = { "sort_order" };

static bool create_pos_dict(db_t *db, String_View *fields, size_t count) {
    if (count < 4) return false;

    static const char *const q[SQL_LANG_COUNT] = {
        [SQL_SQLITE]   = "INSERT INTO dictionaries "
                         "(category, code, label, sort_order) "
                         "VALUES (?, ?, ?, ?);",
        [SQL_MYSQL]    =  "INSERT INTO dictionaries "
                         "(category, code, label, sort_order) "
                         "VALUES (?, ?, ?, ?);",
        [SQL_POSTGRES] = "INSERT INTO dictionaries "
                         "(category, code, label, sort_order) "
                         "VALUES ($1, $2, $3, $4);",
    };
    sql_stmt stmt = {0};
    bool result = true;
    if (!sql_prepare(db, q[db->lang], &stmt))    return_defer(false);
    if (!sql_bind(&stmt, 1, SQL_SV(fields[0])))  return_defer(false);  // category
    if (!sql_bind(&stmt, 2, SQL_SV(fields[1])))  return_defer(false);  // code
    if (!sql_bind(&stmt, 3, SQL_SV(fields[2])))  return_defer(false);  // label
    if (!sql_bind(&stmt, 4, sql_num(fields[3]))) return_defer(false);  // sort_order
    if (!sql_final_step(&stmt))                  return_defer(false);
defer:
    sql_finalize(&stmt);
    return result;
}
SERVE_CREATE(pos_dict, pos_dict, dict_fields, dict_opt_fields)

// create-only page (create_only): the add form + POST /create; no row
// actions (edit/delete/restore hidden, no such routes registered).
void serve_pos_dict(Serve_Context *sc) {
    MD_MasterConfig config = {
        .table          = "dictionaries",
        .title          = "Dictionaries",
        .id_column      = "id",
        .crud_path      = "/pos/dictionaries",
        .columns        = md_dictionaries_columns,
        .column_count   = md_dictionaries_columns_count,
        .create_only    = 1,
    };
    serve_master_child(sc, &config);
}
