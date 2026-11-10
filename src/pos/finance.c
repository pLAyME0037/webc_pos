#include "finance.h"

#include "src/layout/master_child.h"
#include "src/core/webc_template.h"
#include "src/db/db.h"
#include "src/http/utils.h"

// =========================================================================
// /pos/finance: financial_ledgers, read_only (mirrors /pos/dictionaries).
// Account and amounts are stack cells; no create form, edit/delete or
// restore buttons render and no write routes are registered.
// =========================================================================

static MD_Column md_finance_columns[] = {
    { .name = "org_unit_id", .label = "Org", .type = COL_TYPE_FK_SELECT,
      .nullable = false, .fk_table = "org_units", .fk_label = "ou_name" },
    { .name = "reference_type", .label = "Type", .type = COL_TYPE_TEXT,
      .nullable = false },   // ORDER_PAYMENT | EXPENSE | CASH_IN | CASH_OUT
    MD_STACK("Account",
        MD_FIELD("account_code", "Account", COL_TYPE_TEXT, .nullable = false),
        MD_FIELD("entry_type", "Entry", COL_TYPE_TEXT, .nullable = false)
    ),
    MD_STACK("Amounts",
        MD_FIELD("amount", "Amount", COL_TYPE_NUM, .nullable = false),
        MD_FIELD("balance", "Balance", COL_TYPE_NUM, .nullable = false)
    ),
    { .name = "description", .label = "Description", .type = COL_TYPE_TEXT,
      .nullable = true },
};
static const size_t md_finance_columns_count = ARRAY_LEN(md_finance_columns);

void serve_pos_finance(Serve_Context *sc) {
    MD_MasterConfig config = {
        .table          = "financial_ledgers",
        .title          = "Finance",
        .id_column      = "id",
        .crud_path      = "/pos/finance",
        .columns        = md_finance_columns,
        .column_count   = md_finance_columns_count,
        .read_only      = 1,
        .soft_delete    = 1,
    };
    serve_master_child(sc, &config);
}
