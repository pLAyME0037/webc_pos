# Database

How three SQL dialects share one schema, one pool and one migration history.

## Choosing a dialect and a database

Environment resolution happens once in `main()` (`src/webc.c`) before
anything touches the database. Four variables, no config file:

| Variable | Role | Precedence |
|---|---|---|
| `WEBC_DEFAULT_SQL` | pin the dialect: `SQLITE3` / `MYSQL` / `POSTGRES` (case-insensitive). Overrides prefix detection | wins over the prefix, dialect only |
| `WEBC_DB` | full DSN or file path (tests, alternate instances) | wins for the location |
| `WEBC_CONNECTION_STRING` | raw DSN, read only when `WEBC_DB` is unset | fallback |
| *(none)* | `~/sqlite3/webc_c/db` | default |

`webc_db_lang()` (`src/db/db.c:599`) picks the dialect in three steps:

1. `WEBC_DEFAULT_SQL` is set → that dialect (an unknown value logs an error
   and falls through to step 2).
2. `mysql:` / `postgres:` prefix on `WEBC_DB_PATH`.
3. Anything else (including `WEBC_DB_PATH == NULL`) → SQLite.

`webc_db_dsn()` then hands the driver the string with the prefix stripped, so
the driver never sees the selector.

| `WEBC_DB` | Dialect | Driver |
|---|---|---|
| unset → `~/sqlite3/webc_c/db` | SQLite | bundled amalgamation (3.46.1) |
| `/tmp/x.db` | SQLite | bundled amalgamation |
| `mysql:user=root;password=;database=webc;socket=/run/mysqld/mysqld.sock` | MySQL/MariaDB | libmariadb |
| `postgres:...` | PostgreSQL | **stub — fails with a clear message** |
| `WEBC_DEFAULT_SQL=MYSQL` + any path | MySQL/MariaDB | pin wins over the path |

For a SQLite file DSN the parent directory becomes `WEBC_DIR_PATH`, which is
where the runtime creates what it needs (including the recursive
`nested/deep/db` the test harness deliberately exercises). A
`mysql:`/`postgres:` DSN is not a file path, so `WEBC_DIR_PATH` becomes `.`
and the legacy file→directory layout dance is skipped (it only runs for the
SQLite dialect anyway).

### More than one database per process

`webc_open_db_spec(spec)` (`src/db/db.h`) opens an **additional** database —
`"mysql:..."`, `"postgres:..."` or a SQLite path — independent of the primary
global. An explicit prefix on `spec` wins even when `WEBC_DEFAULT_SQL` pins
the primary; a prefix-less `spec` honours the pin. Not pooled: the caller owns
the handle and must `db_close()` it. Schema verification is cached per
`(dialect, DSN)` pair, so opening the same second database twice does not
re-read the migration history.

### Why SQLite is the default

It needs no server, no credentials and no daemon, so a clone builds and runs
with nothing configured. Everything else is opt-in through one variable.

### Postgres: honest stub

`src/db/driver_postgres.c` is a complete `Sql_Driver` vtable in which every
function fails:

```c
static bool pg_drv_open(db_t *db, const char *dsn) {
    (void) dsn;
    if (db) snprintf(db->err, sizeof(db->err), "postgres driver is not implemented yet");
    return false;
}
```

The dialect is **first-class everywhere else** — `q[]` query tables have a
`[SQL_POSTGRES]` entry, `migrations_table_sql[SQL_POSTGRES]` exists, every
migration directory has a `postgres.sql`. Only the wire protocol is missing.

This is deliberate, and the alternative was worse: dropping Postgres from the
schema layer would mean rewriting the whole query table when it is implemented,
and pretending it connects would fail somewhere obscure. Failing in
`db_open()` with a sentence naming the problem is the cheapest possible
mistake. See [decisions.md](decisions.md) D4.

## The driver interface — `src/db/sql.h`

One vtable, four functions, plus column access:

```c
open / close / prepare / step / finalize / column_text
```

`sql.h` gives callers a dialect-neutral statement API — `db_t`, `sql_stmt`,
`sql_step()` returning `SQL_ROW`/`SQL_DONE`/`SQL_ERROR` — so feature code never
branches on dialect for reads. Dialect differences are confined to:

- `driver_*.c`
- `migrations_table_sql[]`
- `migrations/<name>/{sqlite3,mysql,postgres}.sql`
- the `[SQL_*]` entries in per-file query tables

| Driver | Lines | Notes |
|---|---|---|
| `driver_sqlite.c` | 172 | bundled, `third_party/sqlite-amalgamation-3460100` |
| `driver_mysql.c` | 481 | libmariadb, found via `pkg-config` at build time |
| `driver_postgres.c` | 101 | stub |

## The pool

```c
#define DB_POOL_SIZE 8          // src/db/db.h
db_t *open_webc_db(void);       // checkout from the pool (or a transient)
void  db_close(db_t *db);       // pooled -> release, otherwise destroy
void  db_pool_init(void);       // opens all 8 up front, prints the count
```

`db_pool_init()` runs inside `coroutine_server_run()` before `listen()`, so a
bad DSN fails at startup rather than on the first request. It is idempotent,
and `db_pool_get()` will initialise it if called first.

`db_pool_get()` is a real **checkout**: it scans for the next slot whose
`in_use` flag is clear, marks it taken, and `db_close()` hands it back
(`db_pool_release()` rolls back any open transaction and recycles a SQLite
connection that leaked prepared statements). Handlers keep the unchanged
`db = open_webc_db(); ...; db_close(db);` shape — the pool is invisible to
them.

Two safety valves:

- **Pool exhausted** (all 8 checked out): `db_pool_get()` opens a transient
  connection instead of returning `NULL`, so concurrency costs a connection,
  not a 500.
- **Init failure latches**: a bad DSN or a refused schema marks the pool dead
  for the process. `db_pool_get()` then returns `NULL` immediately with one
  log line — no per-request retry storm re-running the 2.4 MB history check.
  Callers already treat `NULL` as "serve 500".

Schema verification itself runs **once per process per `(dialect, DSN)`**
(`verified_items` in `db.c`); every later open of the same database just gets
a fresh connection.

## Migrations

### Layout

```
migrations/
  0001_notes/
    sqlite3.sql    mysql.sql    postgres.sql
  0002_users/
    sqlite3.sql    mysql.sql    postgres.sql
```

Numbered directory = migration name; three dialect files inside. Registered
in a **compile-time array** in `db.c`, not discovered at runtime:

```c
static const Migration migrations[] = {
    { "0001_notes", { [SQL_SQLITE] = "migrations/0001_notes/sqlite3.sql",
                      [SQL_MYSQL]  = "migrations/0001_notes/mysql.sql",
                      [SQL_POSTGRES] = "..." } },
    { "0002_users", { ... } },
};
```

A migration that exists on disk but is not in `migrations[]` **does not run**.
Adding a migration means adding the directory *and* an array entry.

### The history table

```sql
CREATE TABLE IF NOT EXISTS Migrations (
    applied_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    query      TEXT NOT NULL
);
```

(`TIMESTAMP` instead of `DATETIME` on Postgres; `query LONGTEXT` on MySQL —
this is why the DDL is a per-dialect array rather than one string. Databases
created when MySQL still said `TEXT` (64 KiB cap) are widened in place with a
best-effort `ALTER TABLE Migrations MODIFY query LONGTEXT NOT NULL;` on every
open — a migration file larger than 64 KiB would otherwise fail to store.)

`query` stores the **exact text of the migration that was applied**, not a
version number. On every open the code compares stored strings against the
current files.

Why not a version number: it cannot detect that an already-applied migration
was edited. Storing the text turns "someone rewrote history" from silent data
corruption into a detectable state.

The compare is **order-independent**: the `Migrations` table has no key and no
`ORDER BY` can be relied on (MySQL's hidden PK, Postgres row order), so every
stored row must equal some `migrations[]` entry and vice versa — the counts
bound it. A row that matches nothing is `SCHEMA_HISTORY_MISMATCH`.

### The open sequence

`create_schema_status()`:

```
schema verified already for (lang, DSN)?  -- yes: skip straight to a fresh conn
migration_files_load(lang)     -- read the three files, cache per dialect
sql_txn_begin()
exec migrations_table_sql[]    -- ensure Migrations exists (+ LONGTEXT widen)
SELECT query FROM Migrations
compare stored vs current files, order-independent
  more in DB than in code   → ERROR "Database scheme is too new..."
  a stored string differs   → SCHEMA_HISTORY_MISMATCH
  pending                   → apply them all now
sql_txn_commit()
```

**Everything runs in one transaction.** Two consequences, both encoded as
comments in `db.c`:

1. Migration files must not contain `PRAGMA` statements — SQLite rejects
   `journal_mode`/`synchronous` changes inside a transaction. Connection
   PRAGMAs (`cache_size`, `temp_store`, `mmap_size`, `foreign_keys`, ...)
   run in `sqlite_drv_open()` — at open time, before any transaction.
2. A failure commits nothing; there is no partially-migrated state.

Set `WEBC_TRACE_MIGRATION_QUERIES=1` to print each migration's SQL as it
applies.

### When history mismatches: rebuild, keep the data

```c
if (status == SCHEMA_HISTORY_MISMATCH && attempt == 0) {
    db_backup_and_reset(dsn);       // dsn = this connection's own DSN,
                                    // db → db.bak (+ wal/shm), close first
    // fresh db created and migrated from migrations[]
    db_restore_from_backup(...)     // copy shared tables/columns
}
```

The recipe, verbatim from the comment at `db.c:349`:

1. `db` → `db.bak` (old file, untouched)
2. fresh db is created and migrated from `migrations[]`
3. every table/column **both** databases share is copied from `db.bak` into the
   new schema

What it tolerates, each logged as `INFO:`:

- backup table with no counterpart → skipped
- table sharing no columns → skipped
- more than `MAX_TABLE_COLS` columns, or more tables than the array holds →
  refused with an explicit error rather than a partial copy

What it does **not** cover:

- **SQLite only.** `db_restore_from_backup()` takes a `sqlite3*`, so a
  history mismatch on MySQL/MariaDB is not auto-repaired.
- `db.bak` is overwritten on the next mismatch — keep a copy if it matters.

### Migration ordering note

`0001_notes` deliberately has **no `REFERENCES` clause** on `notes.user_id`:
the `users` table does not exist until `0002_users`. The comment in the file
says so. Cross-table integrity between the two is therefore not enforced by
the schema.

### IDs

```sql
id VARCHAR(36) PRIMARY KEY DEFAULT (UUID())          -- sqlite/mysql
id TEXT PRIMARY KEY DEFAULT (gen_random_uuid()::text) -- postgres
```

IDs are **text**, not integers. `parse_uri_id()` in `route.c` tags an
all-digits segment as `ID_INT` so a legacy integer-keyed table still routes;
everything else flows through as text. See [architecture.md](architecture.md).

### Golden files

`test/golden/` pins the `sqlite3.sql` files **byte for byte**. Existing
databases hold those exact strings in `Migrations.query`, so editing a
`sqlite3.sql` that has already shipped is a history mismatch for every user of
that database — which is precisely why the golden test exists. See
[testing.md](testing.md).

The rule when a shipped migration *must* be edited: amend the golden to the
new bytes **in the same commit** — the alternative (moving the change into a
new numbered migration) is only correct when no deployed database carries the
old bytes. `0003_pos_eshop` was amended this way on 2026-11-05: the
`v_geo_hierarchy` view added by `ff4fc0ed` already sits in the live database's
history, so a new `0012` migration would have forced a D5 rebuild of every
existing database instead of a golden re-pin.

## Things that will bite you

| | |
|---|---|
| Editing a shipped migration file (without re-pinning the golden) | history mismatch on every existing SQLite DB |
| Adding a file without a `migrations[]` entry | silently never applied |
| `PRAGMA` inside a migration file | fails — it runs inside a transaction |
| `WEBC_DB` with only a `mysql:` prefix change | no directory is derived (`WEBC_DIR_PATH` = `.`); only the DSN moves |
| Pool failure | latched for the process — one log line, then `NULL` → 500, no retry storm |
| Postgres | `db_open()` fails with "postgres driver is not implemented yet" |
