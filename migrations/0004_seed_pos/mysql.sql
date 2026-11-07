-- ============================================================================
-- 0004_seed_pos -- seeded for MySQL too (ported from sqlite3.sql 2026-11-06).
-- The old header claimed this file was skipped because pos_test is
-- sqlite-only. Missing test coverage must not strip data from a dialect.
-- MySQL idioms used here: INSERT IGNORE, DATE_SUB/UTC_TIMESTAMP,
-- LPAD, WITH RECURSIVE (MariaDB), and `||` as concatenation - the mysql
-- driver sets sql_mode PIPES_AS_CONCAT on every connection.
-- ============================================================================
-- ---------------------------------------------------------------------------
-- 1. One org unit (branch) -- tests create their own 'pos-ou-1'
-- ---------------------------------------------------------------------------
INSERT IGNORE INTO org_units (id, parent_id, ou_code, ou_name, ou_type_dict_id, created_at) VALUES
    ('sd-ou-phm', NULL, 'SD-MAIN', 'ហាងកណ្តាលភ្នំពេញ', 'BRANCH',
     DATE_SUB(UTC_TIMESTAMP(), INTERVAL 300 DAY));

-- ---------------------------------------------------------------------------
-- 2. Categories
-- ---------------------------------------------------------------------------
INSERT IGNORE INTO categories (id, parent_id, cat_code, name, created_at) VALUES
    ('sd-cat-1', NULL, 'SD-CAT-01', 'ទូរស័ព្ទ', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 280 DAY)),
    ('sd-cat-2', NULL, 'SD-CAT-02', 'កុំព្យូទ័រ', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 280 DAY)),
    ('sd-cat-3', NULL, 'SD-CAT-03', 'គ្រឿងបន្លាស់', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 280 DAY)),
    ('sd-cat-4', NULL, 'SD-CAT-04', 'អាគុយ និងថ្ម', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 280 DAY)),
    ('sd-cat-5', NULL, 'SD-CAT-05', 'ឧបករណ៍ការពារ', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 280 DAY));

-- ---------------------------------------------------------------------------
-- 3. Products: 50 rows = 10 Khmer nouns x 5 adjectives (unique names via
--    the row number), category cycles, price = 15 + n*37 % 900.
-- ---------------------------------------------------------------------------
INSERT IGNORE INTO products (id, category_id, product_type_dict_id, sku, name,
                                base_price, cost_price, tax_rate, created_at)
WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < 50)
SELECT 'sd-prod-' || LPAD(n, 3, '0'),
       'sd-cat-' || ((n - 1) % 5 + 1),
       'PHYSICAL',
       'SD-P-' || LPAD(n, 3, '0'),
       (SELECT w.label FROM
            (SELECT 1 AS i, 'ស្តង់ដារ' AS label UNION ALL
             SELECT 2, 'ល្បឿនលឿន' UNION ALL
             SELECT 3, 'ស៊េរីថ្មី' UNION ALL
             SELECT 4, 'រឹងមាំ' UNION ALL
             SELECT 5, 'តូចល្មម') w
        WHERE w.i = (n % 5) + 1)
       || ' ' ||
       (SELECT u.label FROM
            (SELECT 1 AS i, 'ទូរស័ព្ទឆ្លាត' AS label UNION ALL
             SELECT 2, 'កុំព្យូទ័រលើតុ' UNION ALL
             SELECT 3, 'កាសស៊ីស' UNION ALL
             SELECT 4, 'ខ្សែសាក USB' UNION ALL
             SELECT 5, 'ថ្មសាកពាក់លើ' UNION ALL
             SELECT 6, 'គំរបការពារ' UNION ALL
             SELECT 7, 'កញ្ចក់ការពារ' UNION ALL
             SELECT 8, 'ម៉ោនឆ្លុះ' UNION ALL
             SELECT 9, 'បន្ទះឥវ៉ាន់' UNION ALL
             SELECT 10, 'ថាសផ្ទុក') u
        WHERE u.i = (n % 10) + 1)
       || ' ' || n,
       ROUND(15.0 + ((n * 37) % 900), 2),
       ROUND((15.0 + ((n * 37) % 900)) * 0.6, 2),
       CASE WHEN n % 4 = 0 THEN 0.1 ELSE 0.0 END,
       DATE_SUB(UTC_TIMESTAMP(), INTERVAL 200 DAY)
FROM seq;

-- ---------------------------------------------------------------------------
-- 4. One variant per product (black/white, some with a price bump)
-- ---------------------------------------------------------------------------
INSERT IGNORE INTO product_variants (id, product_id, sku, variant_name,
                                        price_delta, created_at)
WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < 50)
SELECT 'sd-var-' || LPAD(n, 3, '0'),
       'sd-prod-' || LPAD(n, 3, '0'),
       'SD-V-' || LPAD(n, 3, '0'),
       CASE WHEN n % 2 = 0 THEN 'ពណ៌ខ្មៅ' ELSE 'ពណ៌ស' END || ' · ' || LPAD(n, 3, '0'),
       CASE WHEN n % 4 = 0 THEN 20000.0 ELSE 0.0 END,
       DATE_SUB(UTC_TIMESTAMP(), INTERVAL 200 DAY)
FROM seq;

-- ---------------------------------------------------------------------------
-- 5. Stock for every product. The first six sit at/below their min so the
--    low-stock report has rows (quantity <= min_threshold).
-- ---------------------------------------------------------------------------
INSERT IGNORE INTO inventory_stocks (id, org_unit_id, product_id, variant_id,
                                        quantity, min_threshold, max_threshold, created_at)
WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < 50)
SELECT 'sd-stock-' || LPAD(n, 3, '0'),
       'sd-ou-phm',
       'sd-prod-' || LPAD(n, 3, '0'),
       'sd-var-' || LPAD(n, 3, '0'),
       CASE WHEN n <= 6 THEN 2 + n ELSE 10 + ((n * 7) % 120) END,
       CASE WHEN n <= 6 THEN 10.0 ELSE 5.0 END,
       200.0,
       DATE_SUB(UTC_TIMESTAMP(), INTERVAL 190 DAY)
FROM seq;

-- ---------------------------------------------------------------------------
-- 6. Customers (loyalty varies)
-- ---------------------------------------------------------------------------
INSERT IGNORE INTO customers (id, customer_type_dict_id, loyalty_points, metadata, created_at) VALUES
    ('sd-cus-1', 'TEIR_2', 240, NULL, DATE_SUB(UTC_TIMESTAMP(), INTERVAL 120 DAY)),
    ('sd-cus-2', 'TEIR_1', 35,  NULL, DATE_SUB(UTC_TIMESTAMP(), INTERVAL 120 DAY)),
    ('sd-cus-3', 'TEIR_3', 860, NULL, DATE_SUB(UTC_TIMESTAMP(), INTERVAL 115 DAY)),
    ('sd-cus-4', 'TEIR_1', 10,  NULL, DATE_SUB(UTC_TIMESTAMP(), INTERVAL 115 DAY)),
    ('sd-cus-5', 'TEIR_2', 415, NULL, DATE_SUB(UTC_TIMESTAMP(), INTERVAL 110 DAY)),
    ('sd-cus-6', 'TEIR_3', 720, NULL, DATE_SUB(UTC_TIMESTAMP(), INTERVAL 110 DAY)),
    ('sd-cus-7', 'TEIR_1', 55,  NULL, DATE_SUB(UTC_TIMESTAMP(), INTERVAL 105 DAY)),
    ('sd-cus-8', 'TEIR_2', 330, NULL, DATE_SUB(UTC_TIMESTAMP(), INTERVAL 105 DAY));

-- ---------------------------------------------------------------------------
-- 7. Users: 3 staff accounts + 5 customer accounts (users.customer_id is
--    what feeds customer_name in the sales report view). All old enough
--    to stay below demo rows in created_at DESC lists.
-- ---------------------------------------------------------------------------
INSERT IGNORE INTO users (id, org_unit_id, name, username, email, phone,
                             password_hash, user_type_dict_id, customer_id,
                             village_id, status, created_at) VALUES
    ('sd-user-1', 'sd-ou-phm', 'សុខ ដារា',   'sd.staff1', 'sd.staff1@pos.kh', '0121000001', '', 'ADMIN',   NULL, '7426', 'ACTIVE', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 150 DAY)),
    ('sd-user-2', 'sd-ou-phm', 'ចាន់ថា ស្រីពៅ', 'sd.staff2', 'sd.staff2@pos.kh', '0121000002', '', 'ADMIN',   NULL, '941', 'ACTIVE', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 150 DAY)),
    ('sd-user-3', 'sd-ou-phm', 'វណ្ណារ៉ា សុខ', 'sd.staff3', 'sd.staff3@pos.kh', '0121000003', '', 'ADMIN',   NULL, '10433', 'ACTIVE', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 150 DAY)),
    ('sd-user-4', NULL,        'ឡាច វិចិត្រ',   'sd.cust1',  'sd.cust1@pos.kh',  '0122000001', '', 'CUSTOMER', 'sd-cus-1', NULL, 'ACTIVE', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 100 DAY)),
    ('sd-user-5', NULL,        'សែស ម៉ាលី',   'sd.cust2',  'sd.cust2@pos.kh',  '0122000002', '', 'CUSTOMER', 'sd-cus-2', NULL, 'ACTIVE', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 100 DAY)),
    ('sd-user-6', NULL,        'រតនៈ ស្រីណែត','sd.cust3',  'sd.cust3@pos.kh',  '0122000003', '', 'CUSTOMER', 'sd-cus-3', NULL, 'ACTIVE', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 95 DAY)),
    ('sd-user-7', NULL,        'គង់ សុផល',    'sd.cust4',  'sd.cust4@pos.kh',  '0122000004', '', 'CUSTOMER', 'sd-cus-5', NULL, 'ACTIVE', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 95 DAY)),
    ('sd-user-8', NULL,        'ប៉ែន សុជាតា',  'sd.cust5',  'sd.cust5@pos.kh',  '0122000005', '', 'CUSTOMER', 'sd-cus-6', NULL, 'ACTIVE', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 90 DAY));

-- ---------------------------------------------------------------------------
-- 8. Staff (tests create their own 'pos-stf-1')
-- ---------------------------------------------------------------------------
INSERT IGNORE INTO staff (id, user_id, org_unit_id, staff_code, staff_type_dict_id,
                             first_name, last_name, phone, village_id, hire_date, created_at) VALUES
    ('sd-stf-1', 'sd-user-1', 'sd-ou-phm', 'SD-STF-01', 'MANAGER',   'សុខ',   'ដារា',   '0121000001', '7426', '2025-01-06', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 150 DAY)),
    ('sd-stf-2', 'sd-user-2', 'sd-ou-phm', 'SD-STF-02', 'CASHIER',   'ចាន់ថា', 'ស្រីពៅ', '0121000002', '941', '2025-03-03', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 150 DAY)),
    ('sd-stf-3', 'sd-user-3', 'sd-ou-phm', 'SD-STF-03', 'DRIVER',    'វណ្ណារ៉ា','សុខ',    '0121000003', '10433', '2025-06-02', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 150 DAY));

-- ---------------------------------------------------------------------------
-- 9. Roles + permissions + links (tests create their own 'pos-role-*')
-- ---------------------------------------------------------------------------
INSERT IGNORE INTO roles (id, org_unit_id, role_code, role_name, description, created_at) VALUES
    ('sd-role-manager',  'sd-ou-phm', 'SD-MANAGER', 'អ្នកគ្រប់គ្រងហាង', 'គ្រប់គ្រងទាំងអស់',       DATE_SUB(UTC_TIMESTAMP(), INTERVAL 140 DAY)),
    ('sd-role-cashier',  'sd-ou-phm', 'SD-CASHIER', 'អ្នកគិតលុយ',       'លក់ និងទទួលប្រាក់',       DATE_SUB(UTC_TIMESTAMP(), INTERVAL 140 DAY)),
    ('sd-role-driver',   'sd-ou-phm', 'SD-DRIVER',  'អ្នកដឹកជញ្ជូន',     'ដឹកជញ្ជូនកម្មង់',         DATE_SUB(UTC_TIMESTAMP(), INTERVAL 140 DAY));

INSERT IGNORE INTO permissions (id, perm_code, perm_name, module_name, created_at) VALUES
    ('sd-perm-1', 'SD.SELL',         'លក់ទំនិញ',       'orders',    DATE_SUB(UTC_TIMESTAMP(), INTERVAL 140 DAY)),
    ('sd-perm-2', 'SD.PRODUCT.MGMT', 'គ្រប់គ្រងផលិតផល', 'products',  DATE_SUB(UTC_TIMESTAMP(), INTERVAL 140 DAY)),
    ('sd-perm-3', 'SD.STOCK.ADJUST', 'កែប្រែស្តុក',     'stocks',    DATE_SUB(UTC_TIMESTAMP(), INTERVAL 140 DAY)),
    ('sd-perm-4', 'SD.REPORT.VIEW',  'មើលរបាយការណ៍',   'reports',   DATE_SUB(UTC_TIMESTAMP(), INTERVAL 140 DAY)),
    ('sd-perm-5', 'SD.USER.MGMT',    'គ្រប់គ្រងអ្នកប្រើ', 'users',     DATE_SUB(UTC_TIMESTAMP(), INTERVAL 140 DAY)),
    ('sd-perm-6', 'SD.DISCOUNT',     'ផ្តល់បញ្ចុតតម្លៃ', 'orders',    DATE_SUB(UTC_TIMESTAMP(), INTERVAL 140 DAY)),
    ('sd-perm-7', 'SD.REFUND',       'សងប្រាក់វិញ',     'payments',  DATE_SUB(UTC_TIMESTAMP(), INTERVAL 140 DAY)),
    ('sd-perm-8', 'SD.SETTINGS',     'ការកំណត់ប្រព័ន្ធ', 'config',    DATE_SUB(UTC_TIMESTAMP(), INTERVAL 140 DAY));

INSERT IGNORE INTO role_permissions (id, role_id, permission_id, created_at) VALUES
    ('sd-rp-m1', 'sd-role-manager', 'sd-perm-1', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 135 DAY)),
    ('sd-rp-m2', 'sd-role-manager', 'sd-perm-2', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 135 DAY)),
    ('sd-rp-m3', 'sd-role-manager', 'sd-perm-3', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 135 DAY)),
    ('sd-rp-m4', 'sd-role-manager', 'sd-perm-4', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 135 DAY)),
    ('sd-rp-m5', 'sd-role-manager', 'sd-perm-5', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 135 DAY)),
    ('sd-rp-m6', 'sd-role-manager', 'sd-perm-6', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 135 DAY)),
    ('sd-rp-m7', 'sd-role-manager', 'sd-perm-7', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 135 DAY)),
    ('sd-rp-m8', 'sd-role-manager', 'sd-perm-8', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 135 DAY)),
    ('sd-rp-c1', 'sd-role-cashier', 'sd-perm-1', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 135 DAY)),
    ('sd-rp-c2', 'sd-role-cashier', 'sd-perm-6', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 135 DAY)),
    ('sd-rp-c3', 'sd-role-cashier', 'sd-perm-4', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 135 DAY));

INSERT IGNORE INTO user_roles (id, user_id, role_id, created_at) VALUES
    ('sd-ur-1', 'sd-user-1', 'sd-role-manager', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 135 DAY)),
    ('sd-ur-2', 'sd-user-2', 'sd-role-cashier', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 135 DAY)),
    ('sd-ur-3', 'sd-user-3', 'sd-role-driver',  DATE_SUB(UTC_TIMESTAMP(), INTERVAL 135 DAY));

-- ---------------------------------------------------------------------------
-- 10. Cash shifts (one closed, one still open)
-- ---------------------------------------------------------------------------
INSERT IGNORE INTO cash (id, org_unit_id, staff_id, opened_at, closed_at,
                                   opening_cash, closing_cash, expected_cash,
                                   status, notes, created_at) VALUES
    ('sd-shift-1', 'sd-ou-phm', 'sd-stf-1',
     DATE_ADD(DATE_SUB(UTC_TIMESTAMP(), INTERVAL 5 DAY), INTERVAL 8 HOUR),
     DATE_ADD(DATE_SUB(UTC_TIMESTAMP(), INTERVAL 5 DAY), INTERVAL 17 HOUR),
     200.0, 512.5, 512.5, 'CLOSED', 'សុតសិល',
     DATE_SUB(UTC_TIMESTAMP(), INTERVAL 5 DAY)),
    ('sd-shift-2', 'sd-ou-phm', 'sd-stf-2',
     DATE_ADD(DATE_SUB(UTC_TIMESTAMP(), INTERVAL 1 DAY), INTERVAL 8 HOUR),
     NULL, 150.0, NULL, NULL, 'OPEN', NULL,
     DATE_SUB(UTC_TIMESTAMP(), INTERVAL 1 DAY));

-- ---------------------------------------------------------------------------
-- 11. Order lines first (2 per order, 42 orders): FKs are off, so the
--     orders can then carry subtotals that equal SUM(total_line).
-- ---------------------------------------------------------------------------
INSERT IGNORE INTO order_items (id, order_id, product_id, variant_id,
                                   unit_price, unit_cost, quantity,
                                   discount_amount, tax_amount, total_line, created_at)
WITH RECURSIVE o(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM o WHERE n < 42),
               k(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM k WHERE i < 2)
SELECT 'sd-item-' || LPAD(o.n, 3, '0') || '-' || k.i,
       'sd-ord-' || LPAD(o.n, 3, '0'),
       'sd-prod-' || LPAD(((o.n * 3 + k.i * 7) % 50) + 1, 3, '0'),
       'sd-var-' || LPAD(((o.n * 3 + k.i * 7) % 50) + 1, 3, '0'),
       p.base_price,
       p.cost_price,
       ((o.n + k.i) % 4) + 1,
       0.0,
       0.0,
       ROUND(p.base_price * (((o.n + k.i) % 4) + 1), 2),
       DATE_ADD(DATE_SUB(UTC_TIMESTAMP(), INTERVAL (((42 - o.n) DIV 3)) DAY), INTERVAL (((o.n % 9) + 8)) HOUR)
FROM o, k, products p
WHERE p.id = 'sd-prod-' || LPAD(((o.n * 3 + k.i * 7) % 50) + 1, 3, '0');

-- ---------------------------------------------------------------------------
-- 12. Orders across the last 14 days. subtotal/total = sum of the lines
--     (+ delivery fee every third order).
-- ---------------------------------------------------------------------------
INSERT IGNORE INTO orders (id, org_unit_id, shift_id, customer_id, staff_id,
                              order_number, order_status_dict_id,
                              subtotal, discount_amount, tax_amount,
                              delivery_fee, total_amount,
                              created_at, updated_at)
WITH RECURSIVE n(k) AS (SELECT 1 UNION ALL SELECT k + 1 FROM n WHERE k < 42)
SELECT 'sd-ord-' || LPAD(k, 3, '0'),
       'sd-ou-phm',
       CASE WHEN k % 5 = 0 THEN NULL
            WHEN k % 2 = 0 THEN 'sd-shift-1'
            ELSE 'sd-shift-2' END,
       CASE WHEN k % 5 = 0 THEN NULL
            ELSE 'sd-cus-' || ((k % 8) + 1) END,
       'sd-stf-' || ((k % 3) + 1),
       'SD-ORD-' || LPAD(k, 3, '0'),
       (SELECT s.id FROM
            (SELECT 1 AS i, 'PENDING' AS id UNION ALL
             SELECT 2, 'PAID' UNION ALL
             SELECT 3, 'PACKED' UNION ALL
             SELECT 4, 'SHIPPED' UNION ALL
             SELECT 5, 'DELIVERED' UNION ALL
             SELECT 6, 'CANCELLED') s
        WHERE s.i = (k % 6) + 1),
       (SELECT ROUND(SUM(i.total_line), 2)
          FROM order_items i
         WHERE i.order_id = 'sd-ord-' || LPAD(k, 3, '0')),
       0.0, 0.0,
       CASE WHEN k % 3 = 0 THEN 3.5 ELSE 0.0 END,
       (SELECT ROUND(SUM(i.total_line), 2)
          FROM order_items i
         WHERE i.order_id = 'sd-ord-' || LPAD(k, 3, '0'))
       + CASE WHEN k % 3 = 0 THEN 3.5 ELSE 0.0 END,
       DATE_ADD(DATE_SUB(UTC_TIMESTAMP(), INTERVAL (((42 - k) DIV 3)) DAY), INTERVAL (((k % 9) + 8)) HOUR),
       DATE_ADD(DATE_SUB(UTC_TIMESTAMP(), INTERVAL (((42 - k) DIV 3)) DAY), INTERVAL (((k % 9) + 8)) HOUR)
FROM n;

-- ---------------------------------------------------------------------------
-- 13. Payments. one per order, every 4th pays half (total_paid < total),
--     every 7th still PENDING (excluded from the report's paid sum).
-- ---------------------------------------------------------------------------
INSERT IGNORE INTO payments (id, order_id, payment_method_dict_id, amount,
                                payment_status, transaction_ref, created_at)
WITH RECURSIVE n(k) AS (SELECT 1 UNION ALL SELECT k + 1 FROM n WHERE k < 42)
SELECT 'sd-pay-' || LPAD(k, 3, '0'),
       'sd-ord-' || LPAD(k, 3, '0'),
       (SELECT m.id FROM
            (SELECT 1 AS i, 'CASH' AS id UNION ALL
             SELECT 2, 'CARD' UNION ALL
             SELECT 3, 'KHQR' UNION ALL
             SELECT 4, 'TRANSFER') m
        WHERE m.i = (k % 4) + 1),
       CASE WHEN k % 4 = 0 THEN ROUND(o.total_amount * 0.5, 2)
            ELSE o.total_amount END,
       CASE WHEN k % 7 = 0 THEN 'PENDING' ELSE 'COMPLETED' END,
       'TXN-SD-' || LPAD(k, 4, '0'),
       o.created_at
FROM n
JOIN orders o ON o.id = 'sd-ord-' || LPAD(k, 3, '0');

-- ---------------------------------------------------------------------------
-- 14. Deliveries: every third order is shipped (statuses cycle).
-- ---------------------------------------------------------------------------
INSERT IGNORE INTO deliveries (id, order_id, driver_staff_id, delivery_status,
                                  recipient_name, recipient_phone, delivery_address,
                                  delivery_cost, dispatched_at, delivered_at, created_at)
WITH RECURSIVE n(k) AS (SELECT 3 UNION ALL SELECT k + 3 FROM n WHERE k < 42)
SELECT 'sd-del-' || LPAD(k, 3, '0'),
       'sd-ord-' || LPAD(k, 3, '0'),
       'sd-stf-3',
       (SELECT s.id FROM
            (SELECT 1 AS i, 'pending' AS id UNION ALL
             SELECT 2, 'in_transit' UNION ALL
             SELECT 3, 'delivered' UNION ALL
             SELECT 4, 'cancelled') s
        WHERE s.i = ((k / 3) % 4) + 1),
       (SELECT r.name FROM
            (SELECT 1 AS i, 'សុខ គឹមហុង' AS name UNION ALL
             SELECT 2, 'ចាន់ ស្រីនាថ' UNION ALL
             SELECT 3, 'វណ្ណ រតនៈ') r
        WHERE r.i = ((k / 3) % 3) + 1),
       '012' || LPAD(k, 7, '0'),
       (SELECT a.name FROM
            (SELECT 1 AS i, 'ផ្លូវ២៧១ ភ្នំពេញ' AS name UNION ALL
             SELECT 2, 'ផ្លូវសម្តេចជុំ ភ្នំពេញ' UNION ALL
             SELECT 3, 'ផ្លូវ៦០៦ ភ្នំពេញ') a
        WHERE a.i = ((k / 3) % 3) + 1),
       2.0 + (k % 4),
       o.created_at,
       CASE WHEN ((k / 3) % 4) + 1 = 3
            THEN DATE_ADD(DATE_SUB(UTC_TIMESTAMP(), INTERVAL (((42 - k) DIV 3)) DAY), INTERVAL (((k % 9) + 10)) HOUR)
            ELSE NULL END,
       o.created_at
FROM n
JOIN orders o ON o.id = 'sd-ord-' || LPAD(k, 3, '0');

-- ---------------------------------------------------------------------------
-- 15. Ledger: one CREDIT row per completed payment with a running balance,
--     plus a handful of EXPENSE debits.
-- ---------------------------------------------------------------------------
INSERT IGNORE INTO financial_ledgers (id, org_unit_id, reference_type, reference_id,
                                         account_code, entry_type, amount, balance,
                                         description, created_at)
SELECT 'sd-led-' || substr(p.id, 8),
       'sd-ou-phm', 'ORDER_PAYMENT', p.id,
       'CASH', 'CREDIT', p.amount,
       ROUND(SUM(p.amount) OVER (ORDER BY p.created_at, p.id), 2),
       'ប្រាក់ចំណូលពីកម្មង់ ' ||
           (SELECT o.order_number FROM orders o WHERE o.id = p.order_id),
       p.created_at
FROM payments p
WHERE p.payment_status = 'COMPLETED';

INSERT IGNORE INTO financial_ledgers (id, org_unit_id, reference_type, reference_id,
                                         account_code, entry_type, amount, balance,
                                         description, created_at)
VALUES
    ('sd-led-exp-1', 'sd-ou-phm', 'EXPENSE', NULL, 'CASH', 'DEBIT', 45.0, -45.0,
     'ថ្នែបផ្ទាល់ខ្លួន', DATE_ADD(DATE_SUB(UTC_TIMESTAMP(), INTERVAL 12 DAY), INTERVAL 9 HOUR)),
    ('sd-led-exp-2', 'sd-ou-phm', 'EXPENSE', NULL, 'CASH', 'DEBIT', 18.75, -18.75,
     'ទឹក និងភេសជ្ជៈ', DATE_ADD(DATE_SUB(UTC_TIMESTAMP(), INTERVAL 10 DAY), INTERVAL 11 HOUR)),
    ('sd-led-exp-3', 'sd-ou-phm', 'EXPENSE', NULL, 'CASH', 'DEBIT', 120.0, -120.0,
     'ថ្លែផ្គត់ផ្គង់ការិយាល័យ', DATE_ADD(DATE_SUB(UTC_TIMESTAMP(), INTERVAL 8 DAY), INTERVAL 15 HOUR)),
    ('sd-led-exp-4', 'sd-ou-phm', 'EXPENSE', NULL, 'CASH', 'DEBIT', 65.0, -65.0,
     'ថ្លែដឹកជញ្ជូន', DATE_ADD(DATE_SUB(UTC_TIMESTAMP(), INTERVAL 6 DAY), INTERVAL 10 HOUR)),
    ('sd-led-exp-5', 'sd-ou-phm', 'EXPENSE', NULL, 'CASH', 'DEBIT', 30.0, -30.0,
     'សាប៊ូ និងការសម្អាត', DATE_ADD(DATE_SUB(UTC_TIMESTAMP(), INTERVAL 4 DAY), INTERVAL 14 HOUR)),
    ('sd-led-exp-6', 'sd-ou-phm', 'EXPENSE', NULL, 'CASH', 'DEBIT', 88.2, -88.2,
     'ជួសជុលឧបករណ៍', DATE_ADD(DATE_SUB(UTC_TIMESTAMP(), INTERVAL 2 DAY), INTERVAL 16 HOUR));

-- ---------------------------------------------------------------------------
-- 16. Sample alerts (mixed seen/sent flags)
-- ---------------------------------------------------------------------------
INSERT IGNORE INTO system_alerts (id, user_id, order_id, alert_type, raw_payload,
                                     is_seen, is_sent, created_at) VALUES
    ('sd-alert-1', 'sd-user-1', 'sd-ord-003', 'MAIL',
     '[{"Alert Mail":"កម្មង់ SD-ORD-003 រង់ចាំការបញ្ជាក់"}]', 1, 1,
     DATE_ADD(DATE_SUB(UTC_TIMESTAMP(), INTERVAL 3 DAY), INTERVAL 9 HOUR)),
    ('sd-alert-2', 'sd-user-4', NULL, 'POPUP',
     '[{"Popup_seen":false,"Message":"សុំផ្លាស់ប្តូរទំនិញ"}]', 0, 0,
     DATE_ADD(DATE_SUB(UTC_TIMESTAMP(), INTERVAL 2 DAY), INTERVAL 13 HOUR)),
    ('sd-alert-3', 'sd-user-2', 'sd-ord-015', 'ORDER_DELAY',
     '[{"Message":"យឺតជាង ២០ នាទី"}]', 1, 0,
     DATE_ADD(DATE_SUB(UTC_TIMESTAMP(), INTERVAL 1 DAY), INTERVAL 10 HOUR)),
    ('sd-alert-4', 'sd-user-3', NULL, 'MAIL',
     '[{"Alert Mail":"ជូនដំណឹងផ្សារថ្មី"}]', 0, 1,
     DATE_ADD(DATE_SUB(UTC_TIMESTAMP(), INTERVAL 1 DAY), INTERVAL 16 HOUR));

-- ---------------------------------------------------------------------------
-- 17. Sample audit rows
-- ---------------------------------------------------------------------------
INSERT IGNORE INTO audit_logs (id, user_id, table_name, record_id, action,
                                  old_values, new_values, created_at) VALUES
    ('sd-aud-1', 'sd-user-2', 'products', 'sd-prod-001', 'UPDATE',
     '{"base_price": 45.0}', '{"base_price": 52.0}',
     DATE_ADD(DATE_SUB(UTC_TIMESTAMP(), INTERVAL 4 DAY), INTERVAL 10 HOUR)),
    ('sd-aud-2', 'sd-user-1', 'orders', 'sd-ord-001', 'INSERT',
     NULL, '{"order_number": "SD-ORD-001", "total_amount": 96.5}',
     DATE_ADD(DATE_SUB(UTC_TIMESTAMP(), INTERVAL 14 DAY), INTERVAL 8 HOUR)),
    ('sd-aud-3', 'sd-user-3', 'inventory_stocks', 'sd-stock-001', 'UPDATE',
     '{"quantity": 20}', '{"quantity": 3}',
     DATE_ADD(DATE_SUB(UTC_TIMESTAMP(), INTERVAL 2 DAY), INTERVAL 11 HOUR)),
    ('sd-aud-4', 'sd-user-2', 'customers', 'sd-cus-3', 'UPDATE',
     '{"loyalty_points": 700}', '{"loyalty_points": 860}',
     DATE_ADD(DATE_SUB(UTC_TIMESTAMP(), INTERVAL 1 DAY), INTERVAL 14 HOUR));
-- and assert against these exact rows. Applying the same volumes to MySQL
