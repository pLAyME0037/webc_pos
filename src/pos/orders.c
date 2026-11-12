#include "orders.h"

#include <string.h>

#include "src/layout/master_child.h"
#include "src/core/webc_template.h"
#include "src/db/db.h"
#include "src/pos/pos_util.h"
#include "src/http/utils.h"

// =========================================================================
// /pos/orders: order_number + org/staff/customer/status FKs + 4-part
// amounts cell. Children: order_items, payments, deliveries.
// shift_id/order_metadata/delivery_fee are not form columns (populated
// by seeds or left on their defaults).
// =========================================================================

MD_Column md_orders_columns[] = {
    MD_FIELD("order_number", "Order #", COL_TYPE_TEXT, .nullable = false),
    MD_FK("org_unit_id", "Branch", "org_units", "ou_name", .nullable = false),
    MD_FK("staff_id", "Cashier", "staff", "staff_code", .nullable = false),
    MD_FK("customer_id", "Customer", "customers", "id", .nullable = true),
    MD_FK("order_status_dict_id", "Status", "dictionaries", "label",
          .fk_where = "category = 'ORDER_STATUS'", .nullable = true),
    MD_STACK("Amounts",
        MD_FIELD("subtotal", "Subtotal", COL_TYPE_NUM, .nullable = false),
        MD_FIELD("discount_amount", "Discount", COL_TYPE_NUM, .nullable = false),
        MD_FIELD("tax_amount", "Tax", COL_TYPE_NUM, .nullable = false),
        MD_FIELD("delivery_fee", "Delivery Fee", COL_TYPE_NUM, .nullable = false),
        MD_FIELD("total_amount", "Total", COL_TYPE_NUM, .nullable = false)
    ),
};
const size_t md_orders_columns_count = ARRAY_LEN(md_orders_columns);

// Child tab shapes (view-local).
static MD_Column md_pos_order_item_columns[] = {
    { .name = "product_id", .label = "Product", .type = COL_TYPE_FK_SELECT,
      .nullable = false, .fk_table = "products", .fk_label = "name" },
    { .name = "variant_id", .label = "Variant", .type = COL_TYPE_FK_SELECT,
      .nullable = true, .fk_table = "product_variants",
      .fk_label = "variant_name" },
    MD_STACK("Line",
        MD_FIELD("unit_price", "Unit", COL_TYPE_NUM, .nullable = false),
        MD_FIELD("quantity", "Qty", COL_TYPE_NUM, .nullable = false),
        MD_FIELD("total_line", "Total", COL_TYPE_NUM, .nullable = false)
    ),
};
static const size_t md_pos_order_item_columns_count =
    ARRAY_LEN(md_pos_order_item_columns);

static MD_Column md_pos_payment_columns[] = {
    { .name = "payment_method_dict_id", .label = "Method",
      .type = COL_TYPE_FK_SELECT, .nullable = true,
      .fk_table = "dictionaries", .fk_label = "label",
      .fk_where = "category = 'PAYMENT_METHOD'" },
    MD_STACK("Payment",
        MD_FIELD("amount", "Amount", COL_TYPE_NUM, .nullable = false),
        MD_FIELD("payment_status", "Status", COL_TYPE_TEXT, .nullable = false)
    ),
    { .name = "transaction_ref", .label = "Ref", .type = COL_TYPE_TEXT,
      .nullable = true },
};
static const size_t md_pos_payment_columns_count =
    ARRAY_LEN(md_pos_payment_columns);

static MD_Column md_pos_delivery_columns[] = {
    { .name = "driver_staff_id", .label = "Driver",
      .type = COL_TYPE_FK_SELECT, .nullable = true,
      .fk_table = "staff",
      .fk_label = "COALESCE((SELECT u.name FROM users u "
                       "WHERE u.id = staff.user_id), staff.id)" },
    MD_STACK("Recipient",
        MD_FIELD("recipient_name", "Name", COL_TYPE_TEXT, .nullable = false),
        MD_FIELD("recipient_phone", "Phone", COL_TYPE_TEXT, .nullable = false),
        MD_FIELD("delivery_address", "Address", COL_TYPE_TEXT, .nullable = false)
    ),
    MD_STACK("Delivery",
        MD_FIELD("delivery_status", "Status", COL_TYPE_TEXT, .nullable = false),
        MD_FIELD("delivery_cost", "Cost", COL_TYPE_NUM, .nullable = false)
    ),
};
static const size_t md_pos_delivery_columns_count =
    ARRAY_LEN(md_pos_delivery_columns);

// DB mutations only -- list loading lives in the master_child engine.
// shift_id, delivery_fee and order_metadata are not form columns: they
// come in as missing opts (-> NULL/0) and are never UPDATEd.
static bool create_order(db_t *db, String_View *fields, size_t count) {
    if (count < 12) return false;
    static const char *const q[SQL_LANG_COUNT] = {
        [SQL_SQLITE]   = "INSERT INTO orders "
                         "(order_number, org_unit_id, staff_id, customer_id, "
                          "order_status_dict_id, subtotal, discount_amount, "
                          "tax_amount, delivery_fee, total_amount, shift_id, "
                          "order_metadata) "
                         "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
        [SQL_MYSQL]    = "INSERT INTO orders "
                         "(order_number, org_unit_id, staff_id, customer_id, "
                          "order_status_dict_id, subtotal, discount_amount, "
                          "tax_amount, delivery_fee, total_amount, shift_id, "
                          "order_metadata) "
                         "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
        [SQL_POSTGRES] = "INSERT INTO orders "
                         "(order_number, org_unit_id, staff_id, customer_id, "
                          "order_status_dict_id, subtotal, discount_amount, "
                          "tax_amount, delivery_fee, total_amount, shift_id, "
                          "order_metadata) "
                         "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12);",
    };
    sql_stmt stmt = {0};
    bool result = true;
    if (!sql_prepare(db, q[db->lang], &stmt))       return_defer(false);
    if (!sql_bind(&stmt, 1,  SQL_SV(fields[0])))    return_defer(false); // number
    if (!sql_bind(&stmt, 2,  SQL_SV(fields[1])))    return_defer(false); // org
    if (!sql_bind(&stmt, 3,  SQL_SV(fields[2])))    return_defer(false); // staff
    if (!sql_bind(&stmt, 4,  sql_fgsv(fields[3])))  return_defer(false); // customer
    if (!sql_bind(&stmt, 5,  sql_fgsv(fields[4])))  return_defer(false); // status
    if (!sql_bind(&stmt, 6,  sql_num(fields[5])))   return_defer(false); // subtotal
    if (!sql_bind(&stmt, 7,  sql_num(fields[6])))   return_defer(false); // discount
    if (!sql_bind(&stmt, 8,  sql_num(fields[7])))   return_defer(false); // tax
    if (!sql_bind(&stmt, 9,  sql_num(fields[10])))  return_defer(false); // delivery
    if (!sql_bind(&stmt, 10, sql_num(fields[8])))   return_defer(false); // total
    if (!sql_bind(&stmt, 11, sql_fgsv(fields[9])))  return_defer(false); // shift
    if (!sql_bind(&stmt, 12, sql_fgsv(fields[11]))) return_defer(false); // metadata
    if (!sql_final_step(&stmt))                     return_defer(false);
defer:
    sql_finalize(&stmt);
    return result;
}

// shift_id/order_metadata/delivery_fee are intentionally not UPDATEd:
// they are not form columns, so a bind could only ever keep them.
static bool update_order(db_t *db, String_View *fields, size_t count,
                         String_View id)
{
    if (count < 12) return false;
    static const char *const q[SQL_LANG_COUNT] = {
        [SQL_SQLITE]   = "UPDATE orders SET "
                         "order_number = COALESCE(NULLIF(?, ''), order_number), "
                         "org_unit_id = COALESCE(NULLIF(?, ''), org_unit_id), "
                         "staff_id = COALESCE(NULLIF(?, ''), staff_id), "
                         "customer_id = COALESCE(NULLIF(?, ''), customer_id), "
                         "order_status_dict_id = COALESCE(NULLIF(?, ''), "
                         "order_status_dict_id), "
                         "subtotal = COALESCE(NULLIF(?, ''), subtotal), "
                         "discount_amount = COALESCE(NULLIF(?, ''), discount_amount), "
                         "tax_amount = COALESCE(NULLIF(?, ''), tax_amount), "
                         "total_amount = COALESCE(NULLIF(?, ''), total_amount) "
                         "WHERE id = ?;",
        [SQL_MYSQL]    = "UPDATE orders SET "
                         "order_number = COALESCE(NULLIF(?, ''), order_number), "
                         "org_unit_id = COALESCE(NULLIF(?, ''), org_unit_id), "
                         "staff_id = COALESCE(NULLIF(?, ''), staff_id), "
                         "customer_id = COALESCE(NULLIF(?, ''), customer_id), "
                         "order_status_dict_id = COALESCE(NULLIF(?, ''), "
                         "order_status_dict_id), "
                         "subtotal = COALESCE(NULLIF(?, ''), subtotal), "
                         "discount_amount = COALESCE(NULLIF(?, ''), discount_amount), "
                         "tax_amount = COALESCE(NULLIF(?, ''), tax_amount), "
                         "total_amount = COALESCE(NULLIF(?, ''), total_amount) "
                         "WHERE id = ?;",
        [SQL_POSTGRES] = "UPDATE orders SET "
                         "order_number = COALESCE(NULLIF($1, ''), order_number), "
                         "org_unit_id = COALESCE(NULLIF($2, ''), org_unit_id), "
                         "staff_id = COALESCE(NULLIF($3, ''), staff_id), "
                         "customer_id = COALESCE(NULLIF($4, ''), customer_id), "
                         "order_status_dict_id = COALESCE(NULLIF($5, ''), "
                         "order_status_dict_id), "
                         "subtotal = COALESCE(NULLIF($6, ''), subtotal), "
                         "discount_amount = COALESCE(NULLIF($7, ''), discount_amount), "
                         "tax_amount = COALESCE(NULLIF($8, ''), tax_amount), "
                         "total_amount = COALESCE(NULLIF($9, ''), total_amount) "
                         "WHERE id = $10;",
    };
    sql_stmt stmt = {0};
    bool result = true;
    if (!sql_prepare(db, q[db->lang], &stmt)) return_defer(false);
    for (int i = 1; i <= 9; ++i) {
        if (!sql_bind(&stmt, i, SQL_SV(fields[i - 1]))) return_defer(false);
    }
    if (!sql_bind(&stmt, 10, SQL_SV(id))) return_defer(false);
    if (!sql_final_step(&stmt))           return_defer(false);
defer:
    sql_finalize(&stmt);
    return result;
}

static bool soft_delete_order(db_t *db, String_View id) {
    static const char *const q[SQL_LANG_COUNT] = {
        [SQL_SQLITE]   = "UPDATE orders "
                         "SET deleted_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
                         "WHERE id = ?;",
        [SQL_MYSQL]    = "UPDATE orders SET deleted_at = UTC_TIMESTAMP() WHERE id = ?;",
        [SQL_POSTGRES] = "UPDATE orders SET deleted_at = now() WHERE id = $1;",
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

static bool restore_order(db_t *db, String_View id) {
    static const char *const q[SQL_LANG_COUNT] = {
        [SQL_SQLITE]   = "UPDATE orders SET deleted_at = NULL WHERE id = ?;",
        [SQL_MYSQL]    = "UPDATE orders SET deleted_at = NULL WHERE id = ?;",
        [SQL_POSTGRES] = "UPDATE orders SET deleted_at = NULL WHERE id = $1;",
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

void serve_pos_orders(Serve_Context *sc) {
    MD_ChildTab children[] = {
        {
            .table        = "order_items",
            .title        = "Items",
            .fk_column    = "order_id",
            .id_column    = "id",
            .crud_path    = "/pos/order_items",
            .columns      = md_pos_order_item_columns,
            .column_count = md_pos_order_item_columns_count,
            .soft_delete  = 1,
        },
        {
            .table        = "payments",
            .title        = "Payments",
            .fk_column    = "order_id",
            .id_column    = "id",
            .crud_path    = "/pos/payments",
            .columns      = md_pos_payment_columns,
            .column_count = md_pos_payment_columns_count,
            .soft_delete  = 1,
        },
        {
            .table        = "deliveries",
            .title        = "Delivery",
            .fk_column    = "order_id",
            .id_column    = "id",
            .crud_path    = "/pos/deliveries",
            .columns      = md_pos_delivery_columns,
            .column_count = md_pos_delivery_columns_count,
            .soft_delete  = 1,
        },
    };
    MD_MasterConfig config = {
        .table          = "orders",
        .title          = "Orders",
        .id_column      = "id",
        .crud_path      = "/pos/orders",
        .columns        = md_orders_columns,
        .column_count   = md_orders_columns_count,
        .children       = children,
        .children_count = ARRAY_LEN(children),
        .soft_delete    = 1,
        // Orders are never a directly-filled list: rows come from the
        // sale/checkout flow, so the search row is the practical view
        // and the add form stays hidden.
        .no_live        = 1,
        .no_create      = 1,
    };
    serve_master_child(sc, &config);
}

static const char *ord_fields[] = {
    "order_number", "org_unit_id", "staff_id", "customer_id",
    "order_status_dict_id", "subtotal", "discount_amount", "tax_amount",
    "total_amount",
};
static const char *ord_opt_fields[] = {
    "shift_id", "delivery_fee", "order_metadata",
};
SERVE_CREATE(pos_orders, order, ord_fields, ord_opt_fields)
SERVE_UPDATE(pos_orders, order, ord_fields, ord_opt_fields)
SERVE_SOFT_DELETE(pos_orders, order)
