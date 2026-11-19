#!/usr/bin/env python3
"""Storefront + cart + guest checkout + auth suite (Phase 11).

Spawned by test/run.sh LAST (after pos_test.py) so the users/orders it
creates cannot disturb earlier suites' list and pagination counts.
Covers:
  - public catalog at / (search, category filter, ?page=), product detail
  - cookie cart: add / update / remove / buy-now (webc_cart)
  - POST /checkout is the only customer order path: server-computed money
    (forged form amounts ignored), stock decrement + stock_ledger rows,
    deliveries contact row, customers/users reuse by phone, cart cleared
  - over-stock and empty-cart rejects leave orders untouched
  - lockdown: /pos, /dashboard, /reports 303 -> /login for guests (incl.
    a forged POST /pos/orders/create), session login/logout flow, and the
    ?next= open-redirect guard.
"""
import os
import re
import sqlite3
import sys
import time
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testlib import Client, Checker  # noqa: E402

HOST = sys.argv[1]
PORT = int(sys.argv[2])
TMP = sys.argv[3]

DB = os.path.join(TMP, "nested", "deep", "db")

c = Client(HOST, PORT)
t = Checker()

STOCK_SQL = (
    "COALESCE((SELECT SUM(s.quantity) FROM inventory_stocks s "
    "          WHERE s.product_id = p.id AND s.deleted_at IS NULL), 0)"
)
PHONE = "012999001"
URLC = {"Content-Type": "application/x-www-form-urlencoded"}


def db_row(sql, args=()):
    conn = sqlite3.connect(DB, timeout=10)
    try:
        conn.execute("PRAGMA busy_timeout=5000")
        return conn.execute(sql, args).fetchone()
    finally:
        conn.close()


def db_rows(sql, args=()):
    conn = sqlite3.connect(DB, timeout=10)
    try:
        conn.execute("PRAGMA busy_timeout=5000")
        return conn.execute(sql, args).fetchall()
    finally:
        conn.close()


def db_exec(sql, args=()):
    conn = sqlite3.connect(DB, timeout=10)
    try:
        conn.execute("PRAGMA busy_timeout=5000")
        conn.execute(sql, args)
        conn.commit()
    finally:
        conn.close()


def post(path, fields):
    """POST urlencoded through the shared cookie jar -> (status, headers)."""
    st, hdr, _, _ = c.req_full(
        "POST", path,
        body=urllib.parse.urlencode(fields).encode(), headers=URLC)
    return st, hdr


def pick_products():
    """a/b/c/d: ordinary seeded products; e: dedicated over-stock victim."""
    rows = db_rows(
        "SELECT p.id, p.name, p.base_price FROM products p "
        "WHERE p.deleted_at IS NULL AND p.id LIKE 'sd-prod-%' "
        f"AND {STOCK_SQL} >= 4 ORDER BY p.id LIMIT 4"
    )
    a, b, cprod, dprod = rows
    e = db_row(
        "SELECT p.id, p.name, p.base_price FROM products p "
        "WHERE p.deleted_at IS NULL AND p.id NOT IN (?,?,?,?) "
        "AND EXISTS (SELECT 1 FROM inventory_stocks s "
        "            WHERE s.product_id = p.id AND s.deleted_at IS NULL) "
        "ORDER BY p.id LIMIT 1",
        (a[0], b[0], cprod[0], dprod[0]),
    )
    return a, b, cprod, dprod, e


def cart_cookie():
    return c.cookies.get("webc_cart", "")


def main():
    c.wait_ready()
    a, b, cprod, dprod, e = pick_products()

    # ---- public storefront ------------------------------------------------
    st, body, _ = c.get("/")
    html = body.decode()
    t.chk("storefront home", st, 200)
    t.chk("storefront has no admin sidebar", 'class="nav-link' in html, False)
    t.chk("storefront grid present", "grid grid-cols-2" in html, True)
    t.chk("storefront search box", 'name="q"' in html, True)
    t.chk("wide container on home", html.count("max-w-7xl") >= 3, True)

    # ?q= search finds a specific seeded product
    st, body, _ = c.get("/?q=" + urllib.parse.quote(a[1]))
    t.chk("search page", st, 200)
    t.chk("search finds the product by name", a[1] in body.decode(), True)
    st, body, _ = c.get("/?q=zzz-no-such-product-zzz")
    t.chk("search empty state", "មិនមានផលិតផលទេ" in body.decode(), True)

    # ?cat= filter: every rendered sku belongs to that category
    cat_id = db_row(
        "SELECT category_id FROM products WHERE id = ?", (a[0],))[0]
    cat_skus = {
        r[0] for r in db_rows(
            "SELECT sku FROM products WHERE category_id = ? "
            "AND deleted_at IS NULL", (cat_id,))
    }
    st, body, _ = c.get(f"/?cat={cat_id}")
    html = body.decode()
    t.chk("category filter page", st, 200)
    # the card markup wraps onto its own line now: allow the whitespace
    # between the attr and the sku text
    shown = set(re.findall(r'text-overlay0 truncate">\s*([^<]+?)\s*· ',
                           html))
    t.chk("category filter renders rows", len(shown) >= 1, True)
    t.chk("category filter only own skus", shown <= cat_skus, True)

    # ?page= pager: page 1 and 2 windows are disjoint
    st, p1, _ = c.get("/")
    st2, p2, _ = c.get("/?page=2")
    t.chk("page=2 status", st2, 200)
    win1 = set(re.findall(r'href="/product/([^"]+)"', p1.decode()))
    win2 = set(re.findall(r'href="/product/([^"]+)"', p2.decode()))
    t.chk("page 1 has product links", len(win1) >= 1, True)
    t.chk("page1/page2 disjoint", bool(win1) and not (win1 & win2), True)

    # ---- product detail ---------------------------------------------------
    st, body, _ = c.get(f"/product/{a[0]}")
    html = body.decode()
    t.chk("product detail", st, 200)
    t.chk("detail shows name", a[1] in html, True)
    t.chk("detail shows price", f"{a[2]:.2f} $" in html, True)
    t.chk("detail add-to-cart form", 'action="/cart/add"' in html, True)
    t.chk("detail buy-now form", 'action="/cart/buynow"' in html, True)
    t.chk("unknown product id -> 404", c.get("/product/no-such-id")[0], 404)
    t.chk("empty product id -> 404", c.get("/product/")[0], 404)

    # ---- product page: 4 sections (pictures / detail / reviews / related) -
    st, body, _ = c.get("/product/sd-prod-050")
    html = body.decode()
    t.chk("product page has 4 sections",
          sorted(re.findall(r'id="product-([a-z]+)"', html)),
          ["detail", "pictures", "related", "reviews"])
    t.chk("picture tile shows fixed kind badge",
          re.search(r'shrink-0">\s*MAIN\s*</span>', html) is not None, True)
    t.chk("picture caption renders", "រូបមុខផលិតផល" in html, True)
    t.chk("picture image tag served",
          'src="/resource/image/' in html, True)
    t.chk("seed thread renders (2 roots + 1 reply)",
          html.count('data-review-id="'), 3)
    t.chk("rating average renders", "★ 4.5" in html, True)
    t.chk("like count renders", re.search(r"♥ 2", html) is not None, True)
    t.chk("sold count renders on picks",
          re.search(r">\s*\d+\s*បានលក់\s*<", html) is not None, True)
    rel = re.findall(r'href="/product/(sd-prod-\d+)"', html)
    t.chk("related/recommend exclude self", "sd-prod-050" in rel, False)
    t.chk("related+recommend list siblings", len(rel) >= 8, True)

    # AliExpress-style hero: gallery LEFT of detail, zoom furniture, data
    t.chk("hero pairs gallery before detail",
          html.find('id="product-pictures"') < html.find('id="product-detail"')
          and 'data-gallery' in html, True)
    t.chk("gallery thumbnails for every picture",
          html.count('data-gallery-thumb'), 2)
    t.chk("lightbox dialog present", 'id="gallery-lightbox"' in html, True)
    t.chk("gallery script included",
          '<script src="/js/ProductGallery.js"' in html, True)
    t.chk("star distribution bars render",
          all(('data-star-bar="%d"' % s) in html for s in (5, 4, 3, 2, 1)),
          True)
    hero = re.search(r'id="product-detail"(.*?)</section>', html, re.S)
    sold_db = int(db_row("SELECT COALESCE(SUM(quantity), 0) "
                         "FROM order_items WHERE product_id = 'sd-prod-050' "
                         "AND deleted_at IS NULL")[0])
    t.chk("hero sold count equals DB sum",
          hero is not None and
          re.search(r'>\s*' + str(sold_db) + r'\s*បានលក់\s*<',
                    hero.group(1)) is not None, True)
    for star in (5, 4, 3, 2, 1):
        want_c = db_row("SELECT COUNT(*) FROM product_reviews "
                        "WHERE product_id = 'sd-prod-050' "
                        "AND rating = ? AND deleted_at IS NULL",
                        (star,))[0]
        bar = re.search(r'data-star-bar="%d".*?shrink-0">(\d+)<' % star,
                        html, re.S)
        t.chk("star bar %d count" % star,
              bar is not None and int(bar.group(1)) == want_c, True)

    # readability pass: 3-col hero carries a user-stats card beside detail,
    # related/recommend are stacked full-width 3-per-row tall cards
    t.chk("user-stats card beside detail",
          html.find('id="product-detail"') < html.find('id="user-stats"')
          and 'id="user-stats"' in html, True)
    related = re.search(r'id="product-related"(.*)', html, re.S)
    t.chk("related block is 3-col", related is not None and
          related.group(1).count('md:grid-cols-3') == 2, True)
    t.chk("related cards tall images", related is not None and
          related.group(1).count('h-44') >= 8, True)

    # i18n backfill: the 35 formerly-unseeded keys exist in both languages
    # and the en cookie actually switches the storefront off the Khmer
    # C-literal fallback
    t.chk("login_to_comment seeded km+en",
          db_row("SELECT COUNT(*) FROM translations "
                 "WHERE trans_key = 'shop.login_to_comment' "
                 "AND language_id IN ('lang_km', 'lang_en')")[0], 2)
    t.chk("signup keys seeded km+en",
          db_row("SELECT COUNT(*) FROM translations "
                 "WHERE trans_key = 'auth.su_submit' "
                 "AND language_id IN ('lang_km', 'lang_en')")[0], 2)
    st, body, _ = c.get("/product/sd-prod-050")
    t.chk("default language keeps khmer prompt",
          "ចូលគណនីដើម្បីផ្សាយមតិ និង LIKE" in body.decode(), True)
    c.cookies["webc_lang"] = "en"
    st, body, _ = c.get("/product/sd-prod-050")
    t.chk("en cookie switches login prompt",
          "Sign in to post comments and LIKE" in body.decode(), True)
    c.cookies.pop("webc_lang", None)

    # empty-state product still renders (no pictures section, no gallery JS)
    st, body, _ = c.get("/product/sd-prod-003")
    html = body.decode()
    t.chk("empty product hides pictures section",
          'id="product-pictures"' in html, False)
    t.chk("empty product skips gallery script",
          '<script src="/js/ProductGallery.js"' in html, False)
    t.chk("empty product keeps reviews section",
          'id="product-reviews"' in html, True)

    # ---- review gating: guests get /login, nothing is stored --------------
    st, hdr, _, _ = c.req_full(
        "POST", "/product/sd-prod-050/review",
        body=urllib.parse.urlencode({"body": "guest spam"}).encode(),
        headers={"Content-Type": "application/x-www-form-urlencoded"})
    t.chk("guest review 303", st, 303)
    t.chk("guest review -> login with next",
          hdr.get("Location", "").startswith("/login?next=/product/"), True)
    t.chk("guest review stored nothing",
          db_row("SELECT COUNT(*) FROM product_reviews "
                 "WHERE body = 'guest spam'")[0], 0)
    st, hdr, _, _ = c.req_full(
        "POST", "/review/sd-rev-1/like",
        body=b"", headers={"Content-Type": "application/x-www-form-urlencoded"})
    t.chk("guest like 303", st, 303)
    t.chk("guest like stored nothing",
          db_row("SELECT COUNT(*) FROM review_likes "
                 "WHERE review_id = 'sd-rev-1' AND user_id = 'sd-user-1'")[0],
          1)

    # ---- signed-in posting: root / reply / flatten / validation / like ----
    g = Client(HOST, PORT)
    st, _ = g.login_as("sd.staff1", "posadmin1")
    t.chk("staff login for review posting", st, 303)
    st, _, _ = g.post_urlencoded(
        "/product/sd-prod-050/review",
        {"body": "suite root comment", "rating": "4"})
    t.chk("root review posts", st, 303)
    rid = db_row("SELECT id FROM product_reviews "
                 "WHERE body = 'suite root comment'")[0]
    t.chk("root review saved with product+rating",
          db_row("SELECT product_id, rating, parent_id FROM product_reviews "
                 "WHERE id = ?", (rid,)),
          ("sd-prod-050", 4, None))
    st, _, _ = g.post_urlencoded(
        "/product/sd-prod-050/review",
        {"body": "suite reply", "parent_id": rid})
    t.chk("reply posts", st, 303)
    rpid = db_row("SELECT id FROM product_reviews "
                  "WHERE body = 'suite reply'")[0]
    t.chk("reply saved under root",
          db_row("SELECT parent_id FROM product_reviews WHERE id = ?",
                 (rpid,)), (rid,))
    st, _, _ = g.post_urlencoded(
        "/product/sd-prod-050/review",
        {"body": "suite flatten", "parent_id": rpid})
    t.chk("reply-to-reply posts", st, 303)
    t.chk("reply-to-reply flattened to root",
          db_row("SELECT parent_id FROM product_reviews "
                 "WHERE body = 'suite flatten'"), (rid,))

    st, _, _ = g.post_urlencoded("/product/sd-prod-050/review",
                                 {"body": "   "})
    t.chk("blank review 400", st, 400)
    st, _, _ = g.post_urlencoded("/product/sd-prod-050/review",
                                 {"body": "x", "rating": "9"})
    t.chk("rating out of range 400", st, 400)
    st, _, _ = g.post_urlencoded("/product/sd-prod-050/review",
                                 {"body": "x", "parent_id": rid,
                                  "rating": "3"})
    t.chk("rating on reply 400", st, 400)
    st, _, _ = g.post_urlencoded("/product/sd-prod-050/review",
                                 {"body": "x", "parent_id": "sd-rev-4"})
    t.chk("parent of another product 400", st, 400)
    st, _, _ = g.post_urlencoded("/product/no-such-product/review",
                                 {"body": "x"})
    t.chk("unknown product 404", st, 404)

    # like toggle: sd.staff1 IS sd-user-1, who already likes sd-rev-1 via
    # the seed - first click unlikes, second click re-likes (unique pair)
    likes0 = db_row("SELECT COUNT(*) FROM review_likes "
                    "WHERE review_id = 'sd-rev-1' AND user_id = 'sd-user-1'")[0]
    st, _, _ = g.post_urlencoded("/review/sd-rev-1/like", {})
    t.chk("like toggle (unlike first)", st, 303)
    t.chk("like row removed",
          db_row("SELECT COUNT(*) FROM review_likes "
                 "WHERE review_id = 'sd-rev-1' AND user_id = 'sd-user-1'")[0],
          1 - likes0)
    st, _, _ = g.post_urlencoded("/review/sd-rev-1/like", {})
    t.chk("like toggle back", st, 303)
    t.chk("like row re-inserted",
          db_row("SELECT COUNT(*) FROM review_likes "
                 "WHERE review_id = 'sd-rev-1' AND user_id = 'sd-user-1'")[0],
          likes0)
    st, _, _ = g.post_urlencoded("/review/sd-rev-999/like", {})
    t.chk("like unknown review 404", st, 404)

    # signed-in page: thread grew, reply sits under its root, form visible
    st, body, _ = g.get("/product/sd-prod-050")
    html = body.decode()
    t.chk("thread renders new rows", html.count('data-review-id="'), 6)
    t.chk("posted comment renders", "suite root comment" in html, True)
    t.chk("reply renders under page", "suite reply" in html, True)
    t.chk("flatten renders under page", "suite flatten" in html, True)
    t.chk("root before reply in html",
          html.find("suite root comment") < html.find("suite reply"), True)
    t.chk("signed-in review form posts to product",
          'action="/product/sd-prod-050/review"' in html, True)

    # ---- top-6 root cap: 5 more roots push the old ones off the page ----
    for i in range(5):
        st, _, _ = g.post_urlencoded("/product/sd-prod-050/review",
                                     {"body": "cap root %d" % i})
        t.chk("cap root %d posts" % i, st, 303)
    st, body, _ = g.get("/product/sd-prod-050")
    html = body.decode()
    root_rows = re.findall(
        r'<article class="border-t border-surface0 pt-3" data-review-id',
        html)
    t.chk("root cap holds at 6", len(root_rows), 6)
    t.chk("newest root renders first",
          html.find("cap root 4") < html.find("cap root 0"), True)
    t.chk("sixth-newest still renders", "suite root comment" in html, True)
    t.chk("oldest roots drop off the page",
          'data-review-id="sd-rev-1"' in html, False)

    # ---- cookie cart ------------------------------------------------------
    st, _ = post("/cart/add", {"product_id": a[0], "qty": "2",
                               "redirect": "/cart"})
    t.chk("cart add a x2 -> redirect", st, 303)
    t.chk("cart cookie set", f"{a[0]}:2" in cart_cookie(), True)

    post("/cart/add", {"product_id": b[0], "qty": "1"})
    t.chk("cart cookie has both lines",
          f"{a[0]}:2,{b[0]}:1" in cart_cookie(), True)

    st, body, _ = c.get("/cart")
    html = body.decode()
    t.chk("cart page", st, 200)
    t.chk("cart rows", html.count("data-cart-row"), 2)
    sub = 2 * a[2] + 1 * b[2]
    t.chk("cart subtotal (DB prices)", f"{sub:.2f} $" in html, True)

    post("/cart/update", {"product_id": a[0], "qty": "5",
                          "redirect": "/cart"})
    t.chk("cart update qty", f"{a[0]}:5" in cart_cookie(), True)
    st, body, _ = c.get("/cart")
    t.chk("updated subtotal",
          f"{5 * a[2] + b[2]:.2f} $" in body.decode(), True)

    post("/cart/remove", {"product_id": b[0]})
    t.chk("cart remove line", cart_cookie(), f"{a[0]}:5")

    post("/cart/update", {"product_id": a[0], "qty": "0"})
    t.chk("qty 0 clears cart cookie", "webc_cart" in c.cookies, False)
    st, body, _ = c.get("/cart")
    t.chk("empty cart state", "រទេះទំនិញទំនេរ" in body.decode(), True)

    # ---- buy now: cart becomes exactly that one line ----------------------
    post("/cart/buynow", {"product_id": cprod[0], "qty": "1"})
    t.chk("buy-now replaces cart", cart_cookie(), f"{cprod[0]}:1")
    st, body, _ = c.get("/checkout")
    html = body.decode()
    t.chk("checkout page (guest allowed)", st, 200)
    t.chk("checkout summary shows product", cprod[1] in html, True)
    t.chk("checkout contact form", 'name="address"' in html, True)

    # ---- happy path checkout ---------------------------------------------
    orders_before = db_row("SELECT COUNT(*) FROM orders")[0]
    stock_c_before = db_row(
        f"SELECT {STOCK_SQL} FROM products p WHERE p.id = ?", (cprod[0],))[0]

    st, hdr = post("/checkout", {
        "name": "សុខ មាន", "phone": PHONE, "address": "ផ្លូវ 51 ភ្នំពេញ",
        "note": "test", "subtotal": "0.01", "total_amount": "999",
        "unit_price": "0", "discount_amount": "500",
    })
    loc = hdr.get("Location", "")
    t.chk("checkout posts to an order", st, 303)
    t.chk("checkout lands on /order/<id>", loc.startswith("/order/"), True)
    t.chk("cart cookie cleared after order", "webc_cart" in c.cookies, False)

    oid = loc.rsplit("/", 1)[-1]
    row = db_row(
        "SELECT order_number, order_status_dict_id, subtotal, total_amount, "
        "org_unit_id, staff_id, customer_id FROM orders WHERE id = ?", (oid,))
    t.chk("order created", row is not None, True)
    t.chk("order count +1", db_row("SELECT COUNT(*) FROM orders")[0],
          orders_before + 1)
    t.chk("order number WEB-", row[0].startswith("WEB-"), True)
    # WEB-YYMMDD is stamped from the shop's local day (00:00-07:00 local
    # is still "yesterday" in UTC).
    t.chk("order number carries the local shop day",
          row[0].startswith(time.strftime("WEB-%y%m%d-")), True)
    t.chk("order status PENDING", row[1], "PENDING")
    t.chk("order money = DB total (forged 0.01/999 ignored)",
          (row[2], row[3]), (cprod[2], cprod[2]))
    t.chk("order org/staff filled", bool(row[4] and row[5]), True)
    t.chk("order customer linked", bool(row[6]), True)
    customer1 = row[6]

    item = db_row(
        "SELECT unit_price, quantity, total_line FROM order_items "
        "WHERE order_id = ?", (oid,))
    t.chk("order line = DB price", (item[0], item[1], item[2]),
          (cprod[2], 1.0, cprod[2]))

    stock_c_after = db_row(
        f"SELECT {STOCK_SQL} FROM products p WHERE p.id = ?", (cprod[0],))[0]
    t.chk("stock decremented", stock_c_after, stock_c_before - 1)
    led = db_row(
        "SELECT quantity_change, balance_after, reference_type "
        "FROM stock_ledger WHERE reference_id = ?", (oid,))
    t.chk("stock ledger row for the order",
          (led[0], led[1], led[2]), (-1.0, stock_c_after, "ORDER"))

    deliv = db_row(
        "SELECT recipient_name, recipient_phone, delivery_address, "
        "delivery_status FROM deliveries WHERE order_id = ?", (oid,))
    t.chk("delivery row carries guest contact",
          (deliv[1], deliv[2]), (PHONE, "ផ្លូវ 51 ភ្នំពេញ"))
    t.chk("delivery status pending", deliv[3], "pending")

    user = db_row(
        "SELECT username, user_type_dict_id, customer_id FROM users "
        "WHERE phone = ?", (PHONE,))
    t.chk("guest user created from phone",
          (user[0], user[1], user[2]), (PHONE, "CUSTOMER", customer1))

    st, body, _ = c.get(f"/order/{oid}")
    html = body.decode()
    t.chk("order confirmation page", st, 200)
    t.chk("confirmation shows order number", row[0] in html, True)
    t.chk("confirmation shows phone", PHONE in html, True)
    t.chk("unknown order id -> 404", c.get("/order/no-such-id")[0], 404)

    # ---- customer reuse: same phone -> same customer, no second user ------
    post("/cart/add", {"product_id": dprod[0], "qty": "1"})
    orders_before2 = db_row("SELECT COUNT(*) FROM orders")[0]
    st, hdr = post("/checkout", {
        "name": "សុខ មាន", "phone": PHONE, "address": "ផ្លូវ 51 ភ្នំពេញ"})
    t.chk("second checkout", st, 303)
    oid2 = hdr.get("Location", "").rsplit("/", 1)[-1]
    row2 = db_row("SELECT customer_id FROM orders WHERE id = ?", (oid2,))
    t.chk("same phone reuses the customer", row2[0], customer1)
    t.chk("users rows for that phone still 1",
          db_row("SELECT COUNT(*) FROM users WHERE phone = ?", (PHONE,))[0], 1)
    t.chk("second order count +1",
          db_row("SELECT COUNT(*) FROM orders")[0], orders_before2 + 1)

    # ---- over-stock reject: rollback, no order, stock intact --------------
    # Force the victim to exactly 1 unit across its stock rows (single row
    # in the seeds, but be robust), then ask for 3.
    db_exec("UPDATE inventory_stocks SET quantity = 0 "
            "WHERE product_id = ? AND deleted_at IS NULL", (e[0],))
    first_row = db_row(
        "SELECT id FROM inventory_stocks "
        "WHERE product_id = ? AND deleted_at IS NULL ORDER BY id LIMIT 1",
        (e[0],))
    db_exec("UPDATE inventory_stocks SET quantity = 1 WHERE id = ?",
            (first_row[0],))
    stock_e = db_row(
        f"SELECT {STOCK_SQL} FROM products p WHERE p.id = ?", (e[0],))[0]
    post("/cart/add", {"product_id": e[0], "qty": "3"})
    orders_before3 = db_row("SELECT COUNT(*) FROM orders")[0]
    st, _, body, _ = c.req_full(
        "POST", "/checkout",
        body=urllib.parse.urlencode({
            "name": "Over Stock", "phone": "012999002",
            "address": "nowhere"}).encode(),
        headers=URLC)
    t.chk("over-stock checkout rejected", st, 200)
    t.chk("over-stock error marker", b"data-checkout-error" in body, True)
    t.chk("over-stock makes no order",
          db_row("SELECT COUNT(*) FROM orders")[0], orders_before3)
    t.chk("over-stock keeps stock", db_row(
        f"SELECT {STOCK_SQL} FROM products p WHERE p.id = ?", (e[0],))[0],
        stock_e)
    # rollback does not touch the cookie - clear the line manually
    post("/cart/remove", {"product_id": e[0]})

    # ---- empty cart: POST /checkout -> /cart, no order --------------------
    orders_before4 = db_row("SELECT COUNT(*) FROM orders")[0]
    st, hdr = post("/checkout", {"name": "x", "phone": "012999003",
                                 "address": "y"})
    t.chk("empty cart checkout -> /cart", (st, hdr.get("Location")),
          (303, "/cart"))
    t.chk("empty cart makes no order",
          db_row("SELECT COUNT(*) FROM orders")[0], orders_before4)
    st, hdr, _, _ = c.req_full("GET", "/checkout")
    t.chk("GET /checkout without cart -> /cart",
          (st, hdr.get("Location")), (303, "/cart"))

    # ---- lockdown: staff pages require login ------------------------------
    st, hdr, _, _ = c.req_full("GET", "/pos/orders")
    t.chk("guest /pos/orders -> login", st, 303)
    t.chk("guest /pos/orders next=",
          "next=/pos/orders" in hdr.get("Location", ""), True)
    t.chk("guest /dashboard -> login", c.req_full("GET", "/dashboard")[0], 303)
    t.chk("guest /reports -> login", c.req_full("GET", "/reports")[0], 303)
    t.chk("guest /pos exact -> login", c.req_full("GET", "/pos")[0], 303)

    # forged staff order creation from a guest must bounce and change nothing
    orders_before5 = db_row("SELECT COUNT(*) FROM orders")[0]
    st, hdr = post("/pos/orders/create", {
        "order_number": "HACK-1", "org_unit_id": "sd-ou-phm",
        "staff_id": "sd-stf-1", "subtotal": "1", "total_amount": "1"})
    t.chk("guest POST order create -> login", st, 303)
    t.chk("guest POST order create changes nothing",
          db_row("SELECT COUNT(*) FROM orders")[0], orders_before5)

    # login: bad credentials stay on the form
    st, body, _ = c.post_urlencoded(
        "/login", {"username": "sd.staff1", "password": "nope"})
    t.chk("login bad password", st, 200)
    t.chk("login bad password marker", b"data-login-error" in body, True)

    # login: good credentials -> session cookie, default landing /dashboard
    st, hdr = c.login_as("sd.staff1", "posadmin1")
    t.chk("login ok", st, 303)
    t.chk("login sets webc_sid", "webc_sid" in hdr.get("Set-Cookie", ""), True)
    t.chk("login lands on /dashboard", hdr.get("Location"), "/dashboard")
    t.chk("staff /pos/orders", c.get("/pos/orders")[0], 200)
    t.chk("staff /dashboard", c.get("/dashboard")[0], 200)
    t.chk("staff /reports", c.get("/reports")[0], 200)

    # ?next= open-redirect guard: //evil.com falls back to /dashboard
    st, hdr, _, _ = c.req_full(
        "POST", "/login?next=" + urllib.parse.quote("//evil.com", safe=""),
        body=urllib.parse.urlencode(
            {"username": "sd.staff1", "password": "posadmin1"}).encode(),
        headers=URLC)
    t.chk("evil next redirected safely", hdr.get("Location"), "/dashboard")

    # logout kills the session server-side, not just the cookie
    sid = c.cookies.get("webc_sid")
    t.chk("session cookie present before logout", bool(sid), True)
    st, hdr, _, _ = c.req_full("GET", "/logout")
    t.chk("logout redirects", st, 303)
    sc = hdr.get("Set-Cookie", "")
    t.chk("logout clears cookie",
          "webc_sid=" in sc and "Max-Age=0" in sc, True)
    t.chk("logout lands on storefront", hdr.get("Location"), "/")
    # replay the old sid: the session row is gone
    c.cookies["webc_sid"] = sid
    t.chk("old session rejected after logout", c.get("/pos/orders")[0], 303)
    c.cookies.pop("webc_sid", None)

    # storefront stays public for guests
    t.chk("storefront public after logout", c.get("/")[0], 200)

    # ---- Phase 12: theme + language dropdown ------------------------------
    st, body, _ = c.get("/")
    html = body.decode()
    t.chk("storefront default lang", '<html lang="km">' in html, True)
    t.chk("storefront loads themeSwitcher",
          'src="/js/themeSwitcher.js"' in html, True)
    t.chk("storefront theme buttons",
          html.count("data-set-theme="), 3)
    t.chk("storefront theme buttons drive controller",
          html.count("window.themeController.setTheme("), 3)
    t.chk("storefront language form", "data-lang-form" in html, True)
    t.chk("storefront language options km+en",
          html.count('<option value="'), 2)
    # Whitespace-tolerant like the other option pins (pos_test): the
    # selected attribute may sit on its own line in the rendered markup.
    t.chk("storefront active option selected",
          bool(re.search(r'value="km"\s+selected', html)), True)
    t.chk("storefront Khmer default placeholder",
          "ស្វែងរកផលិតផល..." in html, True)
    t.chk("no leftover indigo in storefront",
          "indigo" not in html, True)

    # compiled Tailwind output carries the Catppuccin token bridge
    proj = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    outcss = open(os.path.join(proj, "css", "output.css"),
                  encoding="utf-8").read()
    t.chk("output.css Latte :root tokens",
          ":root{--ctp-base:#eff1f5" in outcss, True)
    t.chk("output.css Mocha .dark tokens",
          ".dark{--ctp-base:#1e1e2e" in outcss, True)
    t.chk("output.css utilities read vars",
          "background-color:var(--ctp-base)" in outcss, True)
    t.chk("output.css nav-active rule", "nav-active" in outcss, True)

    # 0006 seeded both languages for every tr() key (70 keys x 2); other
    # suites add rows via /pos/translations, so assert the seeded floor.
    t.chk("0006 seeded translations",
          db_row("SELECT COUNT(*) FROM translations "
                 "WHERE deleted_at IS NULL")[0] >= 140, True)

    # unknown code: redirect without touching the cookie jar
    st, hdr, _, _ = c.req_full("GET", "/lang?code=zz&back=/")
    t.chk("invalid lang code redirects", st, 303)
    t.chk("invalid lang code sets no cookie",
          "webc_lang" in hdr.get("Set-Cookie", ""), False)
    t.chk("invalid lang code leaves jar empty",
          "webc_lang" in c.cookies, False)

    # open-redirect guard on the return path
    st, hdr, _, _ = c.req_full(
        "GET", "/lang?code=km&back=" + urllib.parse.quote("//evil.com"))
    t.chk("lang back=//evil.com lands on /", hdr.get("Location"), "/")

    # valid switch: cookie set -> English UI + lang attribute
    st, hdr, _, _ = c.req_full("GET", "/lang?code=en&back=/")
    t.chk("lang switch redirects", st, 303)
    sc = hdr.get("Set-Cookie", "")
    t.chk("lang switch sets webc_lang",
          "webc_lang=en" in sc and "Path=/" in sc, True)
    t.chk("lang cookie in jar", c.cookies.get("webc_lang"), "en")
    st, body, _ = c.get("/")
    html = body.decode()
    t.chk("english storefront lang attr", '<html lang="en">' in html, True)
    t.chk("english placeholder", "Search products..." in html, True)
    t.chk("english page title", "<title>Products</title>" in html, True)

    # cookie survives a page reload, then reset to km for the checks below
    st, body, _ = c.get("/")
    t.chk("english persists on reload",
          '<html lang="en">' in body.decode(), True)
    c.cookies.pop("webc_lang", None)
    t.chk("default lang back to km",
          '<html lang="km">' in c.get("/")[1].decode(), True)

    # admin shell (header.h.tt): HTML_LANG + LANG_FORM macros on /dashboard
    c.login_as("sd.staff1", "posadmin1")
    dhtml = c.get("/dashboard")[1].decode()
    t.chk("admin doc lang attr", '<html lang="km">' in dhtml, True)
    t.chk("admin language form", "data-lang-form" in dhtml, True)
    t.chk("admin theme buttons kept", "setTheme('dark')" in dhtml, True)
    c.req_full("GET", "/logout")
    c.cookies.pop("webc_sid", None)

    ok = t.summary()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
