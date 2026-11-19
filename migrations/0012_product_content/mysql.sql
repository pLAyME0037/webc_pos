-- ============================================================================
-- 0012_product_content -- MySQL port of the storefront content tables
-- (product_images / product_reviews / review_likes) backing the 4-section
-- /product/<id> page. Follows the 0003 MySQL conventions: no inline FKs,
-- no triggers (the naive semicolon splitter cannot carry them - product and
-- user soft-delete cascades are therefore sqlite-only and ride
-- migrations/0012_product_content/sqlite3.sql). kind keeps its CHECK:
-- MySQL 8 enforces it, MariaDB parses and ignores it, and the C create
-- handler validates the fixed set on every dialect.
-- Seeds mirror sqlite3.sql (sd-prod-*/sd-user-* rows exist in
-- 0004_seed_pos/mysql.sql) - timestamps use the DATE_SUB/UTC_TIMESTAMP
-- idiom from 0004/0011 so demo rows stay older than test-created rows.
-- ============================================================================

CREATE TABLE IF NOT EXISTS product_images (
    id VARCHAR(36) PRIMARY KEY DEFAULT ((UUID())),
    product_id VARCHAR(36) NOT NULL,
    kind TEXT NOT NULL CHECK (kind IN ('MAIN', 'GALLERY', 'DETAIL', 'PACKAGE')),
    name TEXT NOT NULL DEFAULT '',
    image TEXT NOT NULL,
    sort_order INT NOT NULL DEFAULT 0,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    deleted_at DATETIME NULL
);

CREATE INDEX IF NOT EXISTS idx_product_images_product ON product_images(product_id, deleted_at);

CREATE TABLE IF NOT EXISTS product_reviews (
    id VARCHAR(36) PRIMARY KEY DEFAULT ((UUID())),
    product_id VARCHAR(36) NOT NULL,
    user_id VARCHAR(36) NOT NULL,
    parent_id VARCHAR(36) NULL,
    rating INT NULL CHECK (rating >= 1 AND rating <= 5),
    body TEXT NOT NULL,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    deleted_at DATETIME NULL
);

CREATE INDEX IF NOT EXISTS idx_product_reviews_product ON product_reviews(product_id, deleted_at, created_at);
CREATE INDEX IF NOT EXISTS idx_product_reviews_parent ON product_reviews(parent_id, deleted_at);

CREATE TABLE IF NOT EXISTS review_likes (
    id VARCHAR(36) PRIMARY KEY DEFAULT ((UUID())),
    review_id VARCHAR(36) NOT NULL,
    user_id VARCHAR(36) NOT NULL,
    created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE (review_id, user_id)
);

INSERT IGNORE INTO product_images (id, product_id, kind, name, image, sort_order, created_at, updated_at) VALUES
    ('sd-pic-001', 'sd-prod-001', 'MAIN',    'រូបមុខផលិតផល',   '/resource/image/user1.png',       0, DATE_SUB(UTC_TIMESTAMP(), INTERVAL 80 DAY), DATE_SUB(UTC_TIMESTAMP(), INTERVAL 80 DAY)),
    ('sd-pic-002', 'sd-prod-001', 'DETAIL',  'លម្អិតជិត',       '/resource/image/company_icon.png', 1, DATE_SUB(UTC_TIMESTAMP(), INTERVAL 80 DAY), DATE_SUB(UTC_TIMESTAMP(), INTERVAL 80 DAY)),
    ('sd-pic-003', 'sd-prod-002', 'GALLERY', 'បណ្ដុំរូបភាព',     '/resource/image/user1.png',       0, DATE_SUB(UTC_TIMESTAMP(), INTERVAL 75 DAY), DATE_SUB(UTC_TIMESTAMP(), INTERVAL 75 DAY)),
    ('sd-pic-004', 'sd-prod-050', 'MAIN',    'រូបមុខផលិតផល',   '/resource/image/know_me.png',     0, DATE_SUB(UTC_TIMESTAMP(), INTERVAL 70 DAY), DATE_SUB(UTC_TIMESTAMP(), INTERVAL 70 DAY)),
    ('sd-pic-005', 'sd-prod-050', 'PACKAGE', 'កញ្ចប់ផលិតផល',   '/resource/image/company_icon.png', 1, DATE_SUB(UTC_TIMESTAMP(), INTERVAL 70 DAY), DATE_SUB(UTC_TIMESTAMP(), INTERVAL 70 DAY));

INSERT IGNORE INTO product_reviews (id, product_id, user_id, parent_id, rating, body, created_at, updated_at) VALUES
    ('sd-rev-1', 'sd-prod-050', 'sd-user-4', NULL,       5, 'ឆ្ងាញ់ខ្លាំងណាស់ សាកម្តងទៀតជាមិនខាន', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 60 DAY), DATE_SUB(UTC_TIMESTAMP(), INTERVAL 60 DAY)),
    ('sd-rev-2', 'sd-prod-050', 'sd-user-5', 'sd-rev-1', NULL, 'យល់ស្រប! ខ្ញុំក៏ដូចគ្នា',             DATE_SUB(UTC_TIMESTAMP(), INTERVAL 59 DAY), DATE_SUB(UTC_TIMESTAMP(), INTERVAL 59 DAY)),
    ('sd-rev-3', 'sd-prod-050', 'sd-user-5', NULL,       4, 'សេវាលឿន តម្លៃសមរម្យ',                 DATE_SUB(UTC_TIMESTAMP(), INTERVAL 55 DAY), DATE_SUB(UTC_TIMESTAMP(), INTERVAL 55 DAY)),
    ('sd-rev-4', 'sd-prod-001', 'sd-user-6', NULL,       3, 'ប្រើបានល្អ ប៉ុន្តែចង់ឃើញពណ៌ផ្សេងទៀត', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 50 DAY), DATE_SUB(UTC_TIMESTAMP(), INTERVAL 50 DAY));

INSERT IGNORE INTO review_likes (id, review_id, user_id, created_at) VALUES
    ('sd-like-1', 'sd-rev-1', 'sd-user-1', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 58 DAY)),
    ('sd-like-2', 'sd-rev-1', 'sd-user-5', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 58 DAY)),
    ('sd-like-3', 'sd-rev-2', 'sd-user-1', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 57 DAY)),
    ('sd-like-4', 'sd-rev-3', 'sd-user-2', DATE_SUB(UTC_TIMESTAMP(), INTERVAL 54 DAY));
