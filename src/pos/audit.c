#include "audit.h"

#include "src/layout/master_child.h"
#include "src/core/webc_template.h"
#include "src/db/db.h"
#include "src/http/utils.h"

// =========================================================================
// /pos/audit: audit_logs, read_only (mirrors /pos/finance). Old/new JSON
// live in one stack cell; no create form, edit/delete/restore buttons
// render and no write routes are registered.
// =========================================================================

static MD_Column md_audit_columns[] = {
    MD_FK("user_id", "User", "users", "username", .nullable = true),
    MD_FIELD("table_name", "Table", COL_TYPE_TEXT, .nullable = false),
    MD_FIELD("record_id", "Record ID", COL_TYPE_TEXT, .nullable = false),
    MD_FIELD("action", "Action", COL_TYPE_TEXT, .nullable = false),
    MD_STACK("Changes",
        MD_FIELD("old_values", "Before", COL_TYPE_TEXTAREA, .nullable = true),
        MD_FIELD("new_values", "After", COL_TYPE_TEXTAREA, .nullable = true)
    ),
    MD_FIELD("created_at", "Timestamp", COL_TYPE_DATE, .computed = 1),
};
static const size_t md_audit_columns_count = ARRAY_LEN(md_audit_columns);

void serve_pos_audit(Serve_Context *sc) {
    MD_MasterConfig config = {
        .table          = "audit_logs",
        .title          = "Audit",
        .id_column      = "id",
        .crud_path      = "/pos/audit",
        .columns        = md_audit_columns,
        .column_count   = md_audit_columns_count,
        .read_only      = 1,
        .soft_delete    = 1,
    };
    serve_master_child(sc, &config);
}
