-- ============================================================================
-- 0012_product_content -- POSTGRES PORT: the three storefront content tables
-- (product_images / product_reviews / review_likes) with inline REFERENCES
-- per the 0003 postgres convention. Seed data is intentionally NOT ported
-- (same reason as 0004_seed_pos/postgres.sql: sqlite/mysql carry the demo
-- rows, the postgres driver exec is a stub anyway - this file is a
-- specification placeholder).
-- Triggers (soft-delete cascades) are sqlite-only in this schema family.
-- ============================================================================

CREATE TABLE IF NOT EXISTS product_images (
    id TEXT PRIMARY KEY DEFAULT ((gen_random_uuid()::text)),
    product_id TEXT NOT NULL REFERENCES products(id) ON DELETE RESTRICT,
    kind TEXT NOT NULL CHECK (kind IN ('MAIN', 'GALLERY', 'DETAIL', 'PACKAGE')),
    name TEXT NOT NULL DEFAULT '',
    image TEXT NOT NULL,
    sort_order INT NOT NULL DEFAULT 0,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    deleted_at TIMESTAMP NULL
);

CREATE INDEX IF NOT EXISTS idx_product_images_product ON product_images(product_id, deleted_at);

CREATE TABLE IF NOT EXISTS product_reviews (
    id TEXT PRIMARY KEY DEFAULT ((gen_random_uuid()::text)),
    product_id TEXT NOT NULL REFERENCES products(id) ON DELETE RESTRICT,
    user_id TEXT NOT NULL REFERENCES users(id) ON DELETE RESTRICT,
    parent_id TEXT REFERENCES product_reviews(id) ON DELETE RESTRICT,
    rating INT NULL CHECK (rating >= 1 AND rating <= 5),
    body TEXT NOT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    deleted_at TIMESTAMP NULL
);

CREATE INDEX IF NOT EXISTS idx_product_reviews_product ON product_reviews(product_id, deleted_at, created_at);
CREATE INDEX IF NOT EXISTS idx_product_reviews_parent ON product_reviews(parent_id, deleted_at);

CREATE TABLE IF NOT EXISTS review_likes (
    id TEXT PRIMARY KEY DEFAULT ((gen_random_uuid()::text)),
    review_id TEXT NOT NULL REFERENCES product_reviews(id) ON DELETE RESTRICT,
    user_id TEXT NOT NULL REFERENCES users(id) ON DELETE RESTRICT,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE (review_id, user_id)
);
