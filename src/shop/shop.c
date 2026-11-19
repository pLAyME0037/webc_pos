#include "shop.h"

#include <stdlib.h>
#include <string.h>

#include "src/auth/auth.h"
#include "src/http/utils.h"
#include "src/i18n/i18n.h"
#include "src/db/db.h"

#define SHOP_PER_PAGE 12
#define SHOP_MAX_ROWS 64

// ---------------------------------------------------------------------------
// Shared chrome / rendering
// ---------------------------------------------------------------------------

const char *shop_money(double v) {
    return temp_sprintf("%.2f $", v);
}

// First UTF-8 glyph of a name, for the placeholder product tile.
static void sb_append_first_glyph(String_Builder *sb, const char *name) {
    if (!name || !name[0]) return;
    size_t len = strlen(name);
    size_t glen = 1;
    unsigned char c = (unsigned char) name[0];
    if ((c & 0xE0) == 0xC0) glen = 2;
    else if ((c & 0xF0) == 0xE0) glen = 3;
    else if ((c & 0xF8) == 0xF0) glen = 4;
    if (glen > len) glen = len;
    char buf[8];
    memcpy(buf, name, glen);
    buf[glen] = '\0';
    sb_append_html_escaped(sb, buf);
}

static size_t cart_entry_count(Serve_Context *sc) {
    String_View raw = {0};
    if (!http_cookie_find(sc, CART_COOKIE, &raw) || raw.count == 0) return 0;
    size_t n = 0;
    String_View rest = raw;
    while (rest.count > 0 && n < CART_MAX) {
        String_View pair = sv_chop_by_delim(&rest, ',');
        if (pair.count > 0) ++n;
    }
    return n;
}

static const char *shop_name(void) {
    const char *name = NULL;
    db_t *db = open_webc_db();
    if (db) {
        sql_stmt stmt = {0};
        if (sql_prepare(db,
                "SELECT config_value FROM system_configs "
                "WHERE config_key = 'pos.shop_name' "
                "AND deleted_at IS NULL;", &stmt)) {
            if (sql_step(&stmt) == SQL_ROW) {
                const char *v = sql_col_text(&stmt, 0);
                if (v && v[0]) name = temp_strdup(v);
            }
            sql_finalize(&stmt);
        }
        db_close(db);
    }
    return name && name[0] ? name : "ហាង POS";
}

// The storefront chrome markup lives in display/shop/chrome_start.h.tt
// (design rule: state-driven HTML belongs to a template); this host feeds it
// the language-form return path and the session state.
void shop_chrome_start(String_Builder *sb, Serve_Context *sc,
                       const char *active) {
    (void) active;  // nav has no active states beyond the home link
    // Return path for the language form: the URI exactly as served.
    const char *back = sc->query_string.count
        ? temp_sprintf("%.*s?%.*s", (int) sc->uri.count, sc->uri.data,
                       (int) sc->query_string.count, sc->query_string.data)
        : temp_sprintf("%.*s", (int) sc->uri.count, sc->uri.data);
    // Phase 16 signup: anonymous = admin entry + sign in + sign up;
    // signed-in = identity + logout, and only ADMIN keeps the staff
    // dashboard link (CUSTOMER sessions belong on the storefront).
    const Auth_User *au = auth_current_user();
    bool signed_in = au && au->id;
    bool is_admin = signed_in && au->user_type
        && strcmp(au->user_type, "ADMIN") == 0;
#include "build/h_to_html/shop/chrome_start.h"
}

void shop_chrome_end(String_Builder *sb) {
#include "build/h_to_html/shop/chrome_end.h"
}

void shop_render(Serve_Context *sc, int status, const char *title,
                 String_Builder *content) {
    sc->body.count = 0;
    String_View t = sv_from_cstr(title ? title : "POS");
    render_page_shell(sc, t, sb_to_sv(*content));
    http_render_response(sc, status, "text/html", sb_to_sv(sc->body));
}

// ---------------------------------------------------------------------------
// GET / - catalog grid with category chips, search and plain ?page= links
// ---------------------------------------------------------------------------

typedef struct {
    char   id[40];
    char   name[160];
    char   sku[64];
    char   cat[96];
    double price;
    double stock;
} Shop_Row;

// Category chip (data only; markup lives in display/shop/index.h.tt).
typedef struct {
    char   id[40];
    char   name[160];
    bool   on;
} Shop_Chip;

static double sql_col_double(sql_stmt *stmt, int col) {
    const char *v = sql_col_text(stmt, col);
    return v && v[0] ? atof(v) : 0.0;
}

// "?cat=&q=&page=" preserving the other active filters (raw values are
// html-escaped by the caller's attribute context).
static const char *shop_list_query(String_View cat, String_View q, int page) {
    return temp_sprintf("/?cat=%.*s&q=%.*s&page=%d",
                        (int) cat.count, cat.data,
                        (int) q.count, q.data, page);
}

void serve_shop_index(Serve_Context *sc) {
    // Non-NULL empty views: sql_bind maps data == NULL to SQL NULL, which
    // would turn the "= ''" tautologies below into NULL and filter out all
    // rows on a bare "/" request.
    String_View q = sv_from_cstr(""), cat = sv_from_cstr("");
    String_View page_sv = {0};
    form_find(sc->query_string, "q", &q);
    form_find(sc->query_string, "cat", &cat);
    form_find(sc->query_string, "page", &page_sv);
    if (q.count > 100) q.count = 100;

    int page = page_sv.count ? atoi(temp_sprintf("%.*s", (int) page_sv.count,
                                                 page_sv.data)) : 1;
    if (page < 1) page = 1;
    int offset = (page - 1) * SHOP_PER_PAGE;

    char pattern[320] = "%";
    if (q.count > 0) {
        snprintf(pattern, sizeof(pattern), "%%%.*s%%", (int) q.count, q.data);
    }

    static const char cat_sql[] =
        "SELECT id, name FROM categories "
        "WHERE deleted_at IS NULL ORDER BY name LIMIT 50;";
    static const char count_sql[] =
        "SELECT COUNT(*) FROM products p "
        "JOIN categories c ON c.id = p.category_id "
        "WHERE p.deleted_at IS NULL AND c.deleted_at IS NULL "
        "AND (? = '' OR c.id = ?) "
        "AND (? = '' OR p.name LIKE ? OR p.sku LIKE ?);";
    static const char rows_sql[] =
        "SELECT p.id, p.name, p.sku, p.base_price, c.name, "
        "COALESCE((SELECT SUM(s.quantity) FROM inventory_stocks s "
        "          WHERE s.product_id = p.id AND s.deleted_at IS NULL), 0) "
        "FROM products p "
        "JOIN categories c ON c.id = p.category_id "
        "WHERE p.deleted_at IS NULL AND c.deleted_at IS NULL "
        "AND (? = '' OR c.id = ?) "
        "AND (? = '' OR p.name LIKE ? OR p.sku LIKE ?) "
        "ORDER BY p.created_at DESC, p.id DESC "
        "LIMIT ? OFFSET ?;";
    static const char rows_pg[] =
        "SELECT p.id, p.name, p.sku, p.base_price, c.name, "
        "COALESCE((SELECT SUM(s.quantity) FROM inventory_stocks s "
        "          WHERE s.product_id = p.id AND s.deleted_at IS NULL), 0) "
        "FROM products p "
        "JOIN categories c ON c.id = p.category_id "
        "WHERE p.deleted_at IS NULL AND c.deleted_at IS NULL "
        "AND ($1 = '' OR c.id = $2) "
        "AND ($3 = '' OR p.name LIKE $4 OR p.sku LIKE $5) "
        "ORDER BY p.created_at DESC, p.id DESC "
        "LIMIT $6 OFFSET $7;";
    static const char *const q_rows[SQL_LANG_COUNT] = {
        [SQL_SQLITE]   = rows_sql,
        [SQL_MYSQL]    = rows_sql,
        [SQL_POSTGRES] = rows_pg,
    };
    static const char count_pg[] =
        "SELECT COUNT(*) FROM products p "
        "JOIN categories c ON c.id = p.category_id "
        "WHERE p.deleted_at IS NULL AND c.deleted_at IS NULL "
        "AND ($1 = '' OR c.id = $2) "
        "AND ($3 = '' OR p.name LIKE $4 OR p.sku LIKE $5);";
    static const char *const q_count[SQL_LANG_COUNT] = {
        [SQL_SQLITE]   = count_sql,
        [SQL_MYSQL]    = count_sql,
        [SQL_POSTGRES] = count_pg,
    };

    db_t *db = open_webc_db();
    if (!db) { serve_error(sc, 500); return; }

    // Category chips (data only; the markup lives in the template).
    struct { Shop_Chip *items; size_t count, capacity; } chips = {0};
    {
        sql_stmt stmt = {0};
        if (sql_prepare(db, cat_sql, &stmt)) {
            while (sql_step(&stmt) == SQL_ROW) {
                const char *cid = sql_col_text(&stmt, 0);
                const char *cname = sql_col_text(&stmt, 1);
                if (!cid || !cname) continue;
                Shop_Chip ch = {0};
                snprintf(ch.id, sizeof(ch.id), "%s", cid);
                snprintf(ch.name, sizeof(ch.name), "%s", cname);
                ch.on = cat.count == strlen(cid)
                    && memcmp(cat.data, cid, cat.count) == 0;
                da_append(&chips, ch);
            }
            sql_finalize(&stmt);
        }
    }

    // Total pages + page rows.
    long long total = 0;
    {
        sql_stmt stmt = {0};
        if (sql_prepare(db, q_count[db->lang], &stmt)
            && sql_bind(&stmt, 1, SQL_SV(cat))
            && sql_bind(&stmt, 2, SQL_SV(cat))
            && sql_bind(&stmt, 3, SQL_SV(sv_from_cstr(pattern)))
            && sql_bind(&stmt, 4, SQL_SV(sv_from_cstr(pattern)))
            && sql_bind(&stmt, 5, SQL_SV(sv_from_cstr(pattern)))
            && sql_step(&stmt) == SQL_ROW) {
            total = sql_column_int64(&stmt, 0);
        }
        sql_finalize(&stmt);
    }
    int pages = (int) ((total + SHOP_PER_PAGE - 1) / SHOP_PER_PAGE);
    if (pages < 1) pages = 1;
    if (page > pages) { page = pages; offset = (page - 1) * SHOP_PER_PAGE; }

    Shop_Row rows[SHOP_PER_PAGE];
    size_t row_count = 0;
    {
        sql_stmt stmt = {0};
        if (sql_prepare(db, q_rows[db->lang], &stmt)
            && sql_bind(&stmt, 1, SQL_SV(cat))
            && sql_bind(&stmt, 2, SQL_SV(cat))
            && sql_bind(&stmt, 3, SQL_SV(sv_from_cstr(pattern)))
            && sql_bind(&stmt, 4, SQL_SV(sv_from_cstr(pattern)))
            && sql_bind(&stmt, 5, SQL_SV(sv_from_cstr(pattern)))
            && sql_bind(&stmt, 6, SQL_I(SHOP_PER_PAGE))
            && sql_bind(&stmt, 7, SQL_I(offset))) {
            while (sql_step(&stmt) == SQL_ROW
                   && row_count < SHOP_PER_PAGE) {
                Shop_Row *r = &rows[row_count];
                memset(r, 0, sizeof(*r));
                snprintf(r->id, sizeof(r->id), "%s",
                         sql_col_text(&stmt, 0) ? sql_col_text(&stmt, 0) : "");
                snprintf(r->name, sizeof(r->name), "%s",
                         sql_col_text(&stmt, 1) ? sql_col_text(&stmt, 1) : "");
                snprintf(r->sku, sizeof(r->sku), "%s",
                         sql_col_text(&stmt, 2) ? sql_col_text(&stmt, 2) : "");
                r->price = sql_col_double(&stmt, 3);
                snprintf(r->cat, sizeof(r->cat), "%s",
                         sql_col_text(&stmt, 4) ? sql_col_text(&stmt, 4) : "");
                r->stock = sql_col_double(&stmt, 5);
                ++row_count;
            }
        }
        sql_finalize(&stmt);
    }
    db_close(db);

    String_Builder content = {0};
    shop_chrome_start(&content, sc, "/");

    // Filter bar, product grid and pager: display/shop/index.h.tt (design
    // rule: state-driven HTML belongs to a template).
String_Builder *sb = &content;
#include "build/h_to_html/shop/index.h"

    shop_chrome_end(&content);
    shop_render(sc, 200, tr("shop.title_products", "ផលិតផល"), &content);
    sb_free(content);
    da_free(chips);
}

// ---------------------------------------------------------------------------
// GET /product/<id> - four sections: pictures (kind + name) / detail +
// transaction / reviews (thread + likes) / related + recommendation.
// Every list is bounded (fixed stack arrays, indexed WHERE, LIMIT) - a page
// render never grows the heap and never scans a whole table.
// ---------------------------------------------------------------------------

#define SHOP_MAX_PICS    24
#define SHOP_MAX_ROOTS   6
#define SHOP_MAX_REPLIES 100
#define SHOP_MAX_CARDS    8
#define SHOP_REVIEW_MAX  1000   // POST body cap; the display buffer adds slack

typedef struct {
    char kind[16];
    char name[128];
    char image[256];
} Shop_Picture;

typedef struct {
    char   id[48];
    char   parent[48];   // replies: owning root id ("" on roots)
    char   user[96];
    char   when[16];     // YYYY-MM-DD slice of created_at
    char   body[SHOP_REVIEW_MAX + 24];
    double rating;       // 0 = no rating (always 0 on replies)
    int    likes;
    int    liked;        // 1 when the signed-in viewer liked it
} Shop_Review;

typedef struct {
    char   id[48];
    char   name[160];
    double price;
    char   image[256];
    int    sold;         // recommendation cards only
} Shop_Card;

void serve_shop_product(Serve_Context *sc) {
    Route_Id id = sc->route_id;
    if (id.kind == ID_NONE) { serve_error(sc, 404); return; }

    db_t *db = open_webc_db();
    if (!db) { serve_error(sc, 500); return; }

    // Product row + rating summary + comment count (scalar subqueries keep
    // it one round trip over the product's own index).
    static const char sql[] =
        "SELECT p.name, p.sku, p.base_price, c.name, "
        "COALESCE((SELECT SUM(s.quantity) FROM inventory_stocks s "
        "          WHERE s.product_id = p.id AND s.deleted_at IS NULL), 0), "
        "p.category_id, "
        "(SELECT ROUND(AVG(r.rating), 1) FROM product_reviews r "
        "  WHERE r.product_id = p.id AND r.deleted_at IS NULL "
        "    AND r.rating IS NOT NULL), "
        "(SELECT COUNT(*) FROM product_reviews r "
        "  WHERE r.product_id = p.id AND r.deleted_at IS NULL "
        "    AND r.rating IS NOT NULL), "
        "(SELECT COUNT(*) FROM product_reviews r "
        "  WHERE r.product_id = p.id AND r.deleted_at IS NULL), "
        "COALESCE((SELECT SUM(oi.quantity) FROM order_items oi "
        "  WHERE oi.product_id = p.id AND oi.deleted_at IS NULL), 0) "
        "FROM products p JOIN categories c ON c.id = p.category_id "
        "WHERE p.id = ? AND p.deleted_at IS NULL;";
    char name[160] = {0}, sku[64] = {0}, cat[96] = {0}, cat_id[48] = {0};
    double price = 0, stock = 0, avg_rating = 0;
    long rating_count = 0, comment_count = 0, sold = 0;
    bool found = false;
    {
        sql_stmt stmt = {0};
        if (sql_prepare(db, sql, &stmt)
            && sql_bind(&stmt, 1, SQL_SV(id.raw))
            && sql_step(&stmt) == SQL_ROW) {
            snprintf(name, sizeof(name), "%s",
                     sql_col_text(&stmt, 0) ? sql_col_text(&stmt, 0) : "");
            snprintf(sku, sizeof(sku), "%s",
                     sql_col_text(&stmt, 1) ? sql_col_text(&stmt, 1) : "");
            price = sql_col_double(&stmt, 2);
            snprintf(cat, sizeof(cat), "%s",
                     sql_col_text(&stmt, 3) ? sql_col_text(&stmt, 3) : "");
            stock = sql_col_double(&stmt, 4);
            snprintf(cat_id, sizeof(cat_id), "%s",
                     sql_col_text(&stmt, 5) ? sql_col_text(&stmt, 5) : "");
            avg_rating = sql_col_double(&stmt, 6);
            rating_count = (long) sql_column_int64(&stmt, 7);
            comment_count = (long) sql_column_int64(&stmt, 8);
            sold = (long) sql_column_int64(&stmt, 9);
            found = true;
        }
        sql_finalize(&stmt);
    }
    if (!found) { db_close(db); serve_error(sc, 404); return; }

    // Star distribution for the reviews summary: at most 5 rows (GROUP BY
    // rating), parked in a fixed 6-slot array (index 1..5, 0 unused).
    int star_count[6] = {0};
    {
        static const char q[] =
            "SELECT rating, COUNT(*) FROM product_reviews "
            "WHERE product_id = ? AND deleted_at IS NULL "
            "  AND rating IS NOT NULL "
            "GROUP BY rating;";
        sql_stmt stmt = {0};
        if (sql_prepare(db, q, &stmt) && sql_bind(&stmt, 1, SQL_SV(id.raw))) {
            while (sql_step(&stmt) == SQL_ROW) {
                int bucket = (int) sql_column_int64(&stmt, 0);
                if (bucket >= 1 && bucket <= 5) {
                    star_count[bucket] = (int) sql_column_int64(&stmt, 1);
                }
            }
        }
        sql_finalize(&stmt);
    }

    // Viewer identity: the storefront is public; anonymous pages simply
    // carry empty ids (0 likes, login prompts on the write forms).
    const Auth_User *viewer = auth_current_user();
    String_View viewer_id = {0};
    if (viewer != NULL && viewer->id != NULL && viewer->id[0] != '\0') {
        viewer_id = sv_from_cstr(viewer->id);
    }

    // -- section 1: pictures, in display order -----------------------------
    Shop_Picture pics[SHOP_MAX_PICS];
    size_t pic_count = 0;
    {
        static const char q[] =
            "SELECT kind, name, image FROM product_images "
            "WHERE product_id = ? AND deleted_at IS NULL "
            "ORDER BY sort_order, created_at LIMIT 24;";
        sql_stmt stmt = {0};
        if (sql_prepare(db, q, &stmt) && sql_bind(&stmt, 1, SQL_SV(id.raw))) {
            while (sql_step(&stmt) == SQL_ROW && pic_count < SHOP_MAX_PICS) {
                Shop_Picture *p = &pics[pic_count];
                memset(p, 0, sizeof(*p));
                snprintf(p->kind, sizeof(p->kind), "%s",
                         sql_col_text(&stmt, 0) ? sql_col_text(&stmt, 0) : "");
                snprintf(p->name, sizeof(p->name), "%s",
                         sql_col_text(&stmt, 1) ? sql_col_text(&stmt, 1) : "");
                snprintf(p->image, sizeof(p->image), "%s",
                         sql_col_text(&stmt, 2) ? sql_col_text(&stmt, 2) : "");
                ++pic_count;
            }
        }
        sql_finalize(&stmt);
    }

    // -- section 3a: root reviews (newest first) ---------------------------
    Shop_Review roots[SHOP_MAX_ROOTS];
    size_t root_count = 0;
    {
        static const char q[] =
            "SELECT r.id, u.name, r.created_at, r.body, r.rating, "
            "(SELECT COUNT(*) FROM review_likes l "
            "  WHERE l.review_id = r.id), "
            "(SELECT COUNT(*) FROM review_likes l "
            "  WHERE l.review_id = r.id AND l.user_id = ?) "
            "FROM product_reviews r JOIN users u ON u.id = r.user_id "
            "WHERE r.product_id = ? AND r.deleted_at IS NULL "
            "  AND r.parent_id IS NULL "
            "ORDER BY r.created_at DESC LIMIT 6;";
        sql_stmt stmt = {0};
        if (sql_prepare(db, q, &stmt)
            && sql_bind(&stmt, 1, SQL_SV(viewer_id))
            && sql_bind(&stmt, 2, SQL_SV(id.raw))) {
            while (sql_step(&stmt) == SQL_ROW && root_count < SHOP_MAX_ROOTS) {
                Shop_Review *r = &roots[root_count];
                memset(r, 0, sizeof(*r));
                snprintf(r->id, sizeof(r->id), "%s",
                         sql_col_text(&stmt, 0) ? sql_col_text(&stmt, 0) : "");
                snprintf(r->user, sizeof(r->user), "%s",
                         sql_col_text(&stmt, 1) ? sql_col_text(&stmt, 1) : "");
                const char *created = sql_col_text(&stmt, 2);
                if (created) {
                    snprintf(r->when, sizeof(r->when), "%.10s", created);
                }
                snprintf(r->body, sizeof(r->body), "%s",
                         sql_col_text(&stmt, 3) ? sql_col_text(&stmt, 3) : "");
                r->rating = sql_col_double(&stmt, 4);
                r->likes = (int) sql_column_int64(&stmt, 5);
                r->liked = 0;
                if (sql_column_int64(&stmt, 6) > 0) r->liked = 1;
                ++root_count;
            }
        }
        sql_finalize(&stmt);
    }

    // -- section 3b: replies to the loaded roots (placeholders, never a
    //    built-in id list). The thread renders two levels, so a reply to a
    //    reply is stored against its root (see the POST handler). ---------
    Shop_Review replies[SHOP_MAX_REPLIES];
    size_t reply_count = 0;
    if (root_count > 0) {
        char ph[2 * SHOP_MAX_ROOTS + 1];
        size_t pos = 0;
        for (size_t i = 0; i < root_count; ++i) {
            if (i > 0) ph[pos++] = ',';
            ph[pos++] = '?';
        }
        ph[pos] = '\0';
        char q[640];
        snprintf(q, sizeof(q),
            "SELECT r.id, r.parent_id, u.name, r.created_at, r.body, "
            "(SELECT COUNT(*) FROM review_likes l "
            "  WHERE l.review_id = r.id), "
            "(SELECT COUNT(*) FROM review_likes l "
            "  WHERE l.review_id = r.id AND l.user_id = ?) "
            "FROM product_reviews r JOIN users u ON u.id = r.user_id "
            "WHERE r.deleted_at IS NULL AND r.parent_id IN (%s) "
            "ORDER BY r.created_at ASC LIMIT 100;", ph);
        sql_stmt stmt = {0};
        bool bound = sql_prepare(db, q, &stmt)
            && sql_bind(&stmt, 1, SQL_SV(viewer_id));
        for (size_t i = 0; bound && i < root_count; ++i) {
            bound = sql_bind(&stmt, (int) (i + 2),
                             SQL_SV(sv_from_cstr(roots[i].id)));
        }
        if (bound) {
            while (sql_step(&stmt) == SQL_ROW && reply_count < SHOP_MAX_REPLIES) {
                Shop_Review *r = &replies[reply_count];
                memset(r, 0, sizeof(*r));
                snprintf(r->id, sizeof(r->id), "%s",
                         sql_col_text(&stmt, 0) ? sql_col_text(&stmt, 0) : "");
                snprintf(r->parent, sizeof(r->parent), "%s",
                         sql_col_text(&stmt, 1) ? sql_col_text(&stmt, 1) : "");
                snprintf(r->user, sizeof(r->user), "%s",
                         sql_col_text(&stmt, 2) ? sql_col_text(&stmt, 2) : "");
                const char *created = sql_col_text(&stmt, 3);
                if (created) {
                    snprintf(r->when, sizeof(r->when), "%.10s", created);
                }
                snprintf(r->body, sizeof(r->body), "%s",
                         sql_col_text(&stmt, 4) ? sql_col_text(&stmt, 4) : "");
                r->likes = (int) sql_column_int64(&stmt, 5);
                r->liked = 0;
                if (sql_column_int64(&stmt, 6) > 0) r->liked = 1;
                ++reply_count;
            }
        }
        sql_finalize(&stmt);
    }

    // -- section 4a: related = other products of the same category --------
    Shop_Card related[SHOP_MAX_CARDS];
    size_t related_count = 0;
    {
        static const char q[] =
            "SELECT p.id, p.name, p.base_price, "
            "(SELECT i.image FROM product_images i "
            "  WHERE i.product_id = p.id AND i.deleted_at IS NULL "
            "  ORDER BY i.sort_order, i.created_at LIMIT 1) "
            "FROM products p "
            "WHERE p.category_id = ? AND p.id <> ? AND p.deleted_at IS NULL "
            "ORDER BY p.created_at DESC LIMIT 8;";
        sql_stmt stmt = {0};
        if (sql_prepare(db, q, &stmt)
            && sql_bind(&stmt, 1, SQL_SV(sv_from_cstr(cat_id)))
            && sql_bind(&stmt, 2, SQL_SV(id.raw))) {
            while (sql_step(&stmt) == SQL_ROW && related_count < SHOP_MAX_CARDS) {
                Shop_Card *c = &related[related_count];
                memset(c, 0, sizeof(*c));
                snprintf(c->id, sizeof(c->id), "%s",
                         sql_col_text(&stmt, 0) ? sql_col_text(&stmt, 0) : "");
                snprintf(c->name, sizeof(c->name), "%s",
                         sql_col_text(&stmt, 1) ? sql_col_text(&stmt, 1) : "");
                c->price = sql_col_double(&stmt, 2);
                snprintf(c->image, sizeof(c->image), "%s",
                         sql_col_text(&stmt, 3) ? sql_col_text(&stmt, 3) : "");
                ++related_count;
            }
        }
        sql_finalize(&stmt);
    }

    // -- section 4b: recommendation = best sellers (order_items), excluding
    //    self and the related cards so no product shows twice; newest-first
    //    fallback when the product has never sold. ------------------------
    Shop_Card recommend[SHOP_MAX_CARDS];
    size_t rec_count = 0;
    char excl[256] = {0};   // " AND p.id NOT IN (?,?,..)" or empty
    if (related_count > 0) {
        size_t pos = (size_t) snprintf(excl, sizeof(excl), " AND p.id NOT IN (");
        for (size_t i = 0; i < related_count; ++i) {
            if (i > 0) {
                pos += (size_t) snprintf(excl + pos, sizeof(excl) - pos, ",");
            }
            pos += (size_t) snprintf(excl + pos, sizeof(excl) - pos, "?");
        }
        snprintf(excl + pos, sizeof(excl) - pos, ")");
    }
    {
        char q[768];
        snprintf(q, sizeof(q),
            "SELECT p.id, p.name, p.base_price, "
            "COALESCE(SUM(oi.quantity), 0), "
            "(SELECT i.image FROM product_images i "
            "  WHERE i.product_id = p.id AND i.deleted_at IS NULL "
            "  ORDER BY i.sort_order, i.created_at LIMIT 1) "
            "FROM order_items oi JOIN products p ON p.id = oi.product_id "
            "WHERE oi.deleted_at IS NULL "
            "  AND p.deleted_at IS NULL AND p.id <> ?%s "
            "GROUP BY p.id, p.name, p.base_price "
            "ORDER BY COALESCE(SUM(oi.quantity), 0) DESC, "
            "         p.created_at DESC LIMIT 8;", excl);
        sql_stmt stmt = {0};
        bool bound = sql_prepare(db, q, &stmt)
            && sql_bind(&stmt, 1, SQL_SV(id.raw));
        for (size_t i = 0; bound && i < related_count; ++i) {
            bound = sql_bind(&stmt, (int) (i + 2),
                             SQL_SV(sv_from_cstr(related[i].id)));
        }
        if (bound) {
            while (sql_step(&stmt) == SQL_ROW && rec_count < SHOP_MAX_CARDS) {
                Shop_Card *c = &recommend[rec_count];
                memset(c, 0, sizeof(*c));
                snprintf(c->id, sizeof(c->id), "%s",
                         sql_col_text(&stmt, 0) ? sql_col_text(&stmt, 0) : "");
                snprintf(c->name, sizeof(c->name), "%s",
                         sql_col_text(&stmt, 1) ? sql_col_text(&stmt, 1) : "");
                c->price = sql_col_double(&stmt, 2);
                c->sold = (int) sql_column_int64(&stmt, 3);
                snprintf(c->image, sizeof(c->image), "%s",
                         sql_col_text(&stmt, 4) ? sql_col_text(&stmt, 4) : "");
                ++rec_count;
            }
        }
        sql_finalize(&stmt);
    }
    if (rec_count == 0) {
        char q[640];
        snprintf(q, sizeof(q),
            "SELECT p.id, p.name, p.base_price, "
            "(SELECT i.image FROM product_images i "
            "  WHERE i.product_id = p.id AND i.deleted_at IS NULL "
            "  ORDER BY i.sort_order, i.created_at LIMIT 1) "
            "FROM products p "
            "WHERE p.deleted_at IS NULL AND p.id <> ?%s "
            "ORDER BY p.created_at DESC LIMIT 8;", excl);
        sql_stmt stmt = {0};
        bool bound = sql_prepare(db, q, &stmt)
            && sql_bind(&stmt, 1, SQL_SV(id.raw));
        for (size_t i = 0; bound && i < related_count; ++i) {
            bound = sql_bind(&stmt, (int) (i + 2),
                             SQL_SV(sv_from_cstr(related[i].id)));
        }
        if (bound) {
            while (sql_step(&stmt) == SQL_ROW && rec_count < SHOP_MAX_CARDS) {
                Shop_Card *c = &recommend[rec_count];
                memset(c, 0, sizeof(*c));
                snprintf(c->id, sizeof(c->id), "%s",
                         sql_col_text(&stmt, 0) ? sql_col_text(&stmt, 0) : "");
                snprintf(c->name, sizeof(c->name), "%s",
                         sql_col_text(&stmt, 1) ? sql_col_text(&stmt, 1) : "");
                c->price = sql_col_double(&stmt, 2);
                snprintf(c->image, sizeof(c->image), "%s",
                         sql_col_text(&stmt, 3) ? sql_col_text(&stmt, 3) : "");
                ++rec_count;
            }
        }
        sql_finalize(&stmt);
    }

    db_close(db);

    int stock_i = (int) stock;
    int sold_i = (int) sold;
    bool out = stock_i <= 0;
    const char *id_s = temp_sprintf("%.*s", (int) id.raw.count, id.raw.data);
    const char *avg_s = "";
    if (rating_count > 0) avg_s = temp_sprintf("%.1f", avg_rating);

    String_Builder content = {0};
    shop_chrome_start(&content, sc, "/");

    // Four-section page: display/shop/product.h.tt (design rule: state-
    // driven HTML belongs to a template).
String_Builder *sb = &content;
#include "build/h_to_html/shop/product.h"

    shop_chrome_end(&content);
    shop_render(sc, 200, name, &content);
    sb_free(content);
}

// ---------------------------------------------------------------------------
// Shared gate for the storefront's write endpoints: the product (or review)
// resolved first - so the ?next= target is a real, safe path - then a
// logged-in user. Anonymous visitors land on /login and come back via ?next=.
// Returns true after writing the response.
// ---------------------------------------------------------------------------
static bool shop_review_login_gate(Serve_Context *sc, const char *product_id) {
    const Auth_User *viewer = auth_current_user();
    if (viewer != NULL && viewer->id != NULL && viewer->id[0] != '\0') {
        return false;
    }
    http_render_redirect(sc, 303,
                         temp_sprintf("/login?next=/product/%s", product_id));
    return true;
}

// POST /product/<id>/review - comment or reply from a signed-in user.
// Root posts may carry a rating (single digit 1..5); replies attach to the
// root (a reply to a reply is stored against its root) so the page renders
// a bounded two-level thread.
void serve_shop_product_review(Serve_Context *sc) {
    Route_Id id = sc->route_id;
    if (id.kind == ID_NONE) { serve_error(sc, 404); return; }

    db_t *db = open_webc_db();
    if (!db) { serve_error(sc, 500); return; }

    bool product_ok = false;
    {
        static const char q[] =
            "SELECT 1 FROM products WHERE id = ? AND deleted_at IS NULL;";
        sql_stmt stmt = {0};
        if (sql_prepare(db, q, &stmt)
            && sql_bind(&stmt, 1, SQL_SV(id.raw))
            && sql_step(&stmt) == SQL_ROW) {
            product_ok = true;
        }
        sql_finalize(&stmt);
    }
    if (!product_ok) { db_close(db); serve_error(sc, 404); return; }

    const char *pid_s = temp_sprintf("%.*s", (int) id.raw.count, id.raw.data);
    if (shop_review_login_gate(sc, pid_s)) { db_close(db); return; }
    const Auth_User *viewer = auth_current_user();

    String_View body = form_text(sb_to_sv(sc->request), sb_to_sv(sc->body),
                                 "body");
    String_View parent = form_text(sb_to_sv(sc->request), sb_to_sv(sc->body),
                                   "parent_id");
    String_View rating_sv = form_text(sb_to_sv(sc->request),
                                      sb_to_sv(sc->body), "rating");

    // Body: 1..SHOP_REVIEW_MAX chars with at least one non-space.
    if (body.count == 0 || body.count > SHOP_REVIEW_MAX) {
        db_close(db);
        serve_error(sc, 400);
        return;
    }
    bool has_text = false;
    for (size_t i = 0; i < body.count; ++i) {
        if (body.data[i] != ' ' && body.data[i] != '\t'
            && body.data[i] != '\r' && body.data[i] != '\n') {
            has_text = true;
            break;
        }
    }
    if (!has_text) { db_close(db); serve_error(sc, 400); return; }

    // Resolve the parent: it must be a live review of THIS product, and a
    // reply to a reply is flattened to its root (two-level display).
    char parent_id[48] = {0};
    if (parent.count > 0) {
        static const char q[] =
            "SELECT parent_id FROM product_reviews "
            "WHERE id = ? AND product_id = ? AND deleted_at IS NULL;";
        bool parent_ok = false;
        char grand[48] = {0};   // copied inside the step: column pointers
                                // die with sql_finalize
        sql_stmt stmt = {0};
        if (sql_prepare(db, q, &stmt)
            && sql_bind(&stmt, 1, SQL_SV(parent))
            && sql_bind(&stmt, 2, SQL_SV(id.raw))
            && sql_step(&stmt) == SQL_ROW) {
            parent_ok = true;
            const char *g = sql_col_text(&stmt, 0);
            if (g != NULL) snprintf(grand, sizeof(grand), "%s", g);
        }
        sql_finalize(&stmt);
        if (!parent_ok) { db_close(db); serve_error(sc, 400); return; }
        if (grand[0] != '\0') {
            snprintf(parent_id, sizeof(parent_id), "%s", grand);
        } else {
            snprintf(parent_id, sizeof(parent_id), "%.*s",
                     (int) parent.count, parent.data);
        }
    }

    // Rating: roots only, exactly one digit 1..5 when present.
    bool has_rating = false;
    int rating = 0;
    if (rating_sv.count > 0) {
        if (parent_id[0] != '\0' || rating_sv.count != 1
            || rating_sv.data[0] < '1' || rating_sv.data[0] > '5') {
            db_close(db);
            serve_error(sc, 400);
            return;
        }
        has_rating = true;
        rating = rating_sv.data[0] - '0';
    }

    static const char q[] =
        "INSERT INTO product_reviews "
        "(product_id, user_id, parent_id, rating, body) "
        "VALUES (?, ?, ?, ?, ?);";
    sql_val parent_bind = SQL_NIL();
    if (parent_id[0] != '\0') parent_bind = SQL_SV(sv_from_cstr(parent_id));
    sql_val rating_bind = SQL_NIL();
    if (has_rating) rating_bind = SQL_I(rating);
    bool ok = false;
    {
        sql_stmt stmt = {0};
        if (sql_prepare(db, q, &stmt)
            && sql_bind(&stmt, 1, SQL_SV(id.raw))
            && sql_bind(&stmt, 2, SQL_SV(sv_from_cstr(viewer->id)))
            && sql_bind(&stmt, 3, parent_bind)
            && sql_bind(&stmt, 4, rating_bind)
            && sql_bind(&stmt, 5, SQL_SV(body))
            && sql_final_step(&stmt)) {
            ok = true;
        }
        sql_finalize(&stmt);
    }
    db_close(db);
    if (!ok) { serve_error(sc, 500); return; }
    http_render_redirect(sc, 303, temp_sprintf("/product/%s", pid_s));
}

// POST /review/<id>/like - toggle the viewer's like on one comment
// (INSERT-or-DELETE; the unique pair keeps double clicks idempotent).
void serve_shop_review_like(Serve_Context *sc) {
    Route_Id id = sc->route_id;
    if (id.kind == ID_NONE) { serve_error(sc, 404); return; }

    db_t *db = open_webc_db();
    if (!db) { serve_error(sc, 500); return; }

    char pid[48] = {0};
    {
        static const char q[] =
            "SELECT product_id FROM product_reviews "
            "WHERE id = ? AND deleted_at IS NULL;";
        sql_stmt stmt = {0};
        if (sql_prepare(db, q, &stmt)
            && sql_bind(&stmt, 1, SQL_SV(id.raw))
            && sql_step(&stmt) == SQL_ROW) {
            snprintf(pid, sizeof(pid), "%s",
                     sql_col_text(&stmt, 0) ? sql_col_text(&stmt, 0) : "");
        }
        sql_finalize(&stmt);
    }
    if (pid[0] == '\0') { db_close(db); serve_error(sc, 404); return; }

    if (shop_review_login_gate(sc, pid)) { db_close(db); return; }
    const Auth_User *viewer = auth_current_user();

    bool liked = false;
    {
        static const char q[] =
            "SELECT 1 FROM review_likes "
            "WHERE review_id = ? AND user_id = ?;";
        sql_stmt stmt = {0};
        if (sql_prepare(db, q, &stmt)
            && sql_bind(&stmt, 1, SQL_SV(id.raw))
            && sql_bind(&stmt, 2, SQL_SV(sv_from_cstr(viewer->id)))
            && sql_step(&stmt) == SQL_ROW) {
            liked = true;
        }
        sql_finalize(&stmt);
    }

    if (liked) {
        static const char q[] =
            "DELETE FROM review_likes "
            "WHERE review_id = ? AND user_id = ?;";
        sql_stmt stmt = {0};
        if (sql_prepare(db, q, &stmt)
            && sql_bind(&stmt, 1, SQL_SV(id.raw))
            && sql_bind(&stmt, 2, SQL_SV(sv_from_cstr(viewer->id)))) {
            sql_final_step(&stmt);
        }
        sql_finalize(&stmt);
    } else {
        static const char *const q[SQL_LANG_COUNT] = {
            [SQL_SQLITE]   = "INSERT OR IGNORE INTO review_likes "
                             "(review_id, user_id) VALUES (?, ?);",
            [SQL_MYSQL]    = "INSERT IGNORE INTO review_likes "
                             "(review_id, user_id) VALUES (?, ?);",
            [SQL_POSTGRES] = "INSERT INTO review_likes "
                             "(review_id, user_id) VALUES ($1, $2) "
                             "ON CONFLICT DO NOTHING;",
        };
        sql_stmt stmt = {0};
        if (sql_prepare(db, q[db->lang], &stmt)
            && sql_bind(&stmt, 1, SQL_SV(id.raw))
            && sql_bind(&stmt, 2, SQL_SV(sv_from_cstr(viewer->id)))) {
            sql_final_step(&stmt);
        }
        sql_finalize(&stmt);
    }
    db_close(db);
    http_render_redirect(sc, 303, temp_sprintf("/product/%s", pid));
}
