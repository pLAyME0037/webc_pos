#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOB_STRIP_PREFIX
#include "third_party/nob/nob.h"

#include "sqlite3.h"
#include "db.h"
#include "sql.h"

Db_Cfg db_cfg = {0};  // set once in main() (src/webc.c); see db.h
bool WEBC_TRACE_MIGRATION_QUERIES = false;

bool txn_begin(sqlite3 *db) {
    if (sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return false;
    }
    return true;
}

bool txn_commit(sqlite3 *db) {
    if (sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return false;
    }
    return true;
}

bool txn_rollback(sqlite3 *db) {
    if (sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return false;
    }
    return true;
}

/* Migrations live as per-dialect SQL files under migrations/ (no inline
 * strings): every entry names a directory with sqlite3.sql / mysql.sql /
 * postgres.sql. All migrations are applied inside one transaction, so these
 * files must not contain PRAGMAs (SQLite rejects `journal_mode`/`synchronous`
 * changes inside a transaction) - those run in open_webc_db() before the
 * transaction starts.
 *
 * Existing databases store the exact file contents in their Migrations
 * history, so the sqlite3.sql files must keep matching what the old inline
 * migrations[] held byte for byte (pinned by test/golden/). */
typedef struct {
    const char *name;
    const char *paths[SQL_LANG_COUNT];
} Migration;

static const Migration migrations[] = {
    { "0001_notes", {
        [SQL_SQLITE]   = "migrations/0001_notes/sqlite3.sql",
        [SQL_MYSQL]    = "migrations/0001_notes/mysql.sql",
        [SQL_POSTGRES] = "migrations/0001_notes/postgres.sql",
    }},
    { "0002_users", {
        [SQL_SQLITE]   = "migrations/0002_users/sqlite3.sql",
        [SQL_MYSQL]    = "migrations/0002_users/mysql.sql",
        [SQL_POSTGRES] = "migrations/0002_users/postgres.sql",
    }},
    { "0003_pos_eshop", {
        [SQL_SQLITE]   = "migrations/0003_pos_eshop/sqlite3.sql",
        [SQL_MYSQL]    = "migrations/0003_pos_eshop/mysql.sql",
        [SQL_POSTGRES] = "migrations/0003_pos_eshop/postgres.sql",
    }},
    { "0004_seed_pos", {
        [SQL_SQLITE]   = "migrations/0004_seed_pos/sqlite3.sql",
        [SQL_MYSQL]    = "migrations/0004_seed_pos/mysql.sql",
        [SQL_POSTGRES] = "migrations/0004_seed_pos/postgres.sql",
    }},
    { "0005_auth", {
        [SQL_SQLITE]   = "migrations/0005_auth/sqlite3.sql",
        [SQL_MYSQL]    = "migrations/0005_auth/mysql.sql",
        [SQL_POSTGRES] = "migrations/0005_auth/postgres.sql",
    }},
    { "0006_i18n", {
        [SQL_SQLITE]   = "migrations/0006_i18n/sqlite3.sql",
        [SQL_MYSQL]    = "migrations/0006_i18n/mysql.sql",
        [SQL_POSTGRES] = "migrations/0006_i18n/postgres.sql",
    }},
    { "0007_users_contact", {
        [SQL_SQLITE]   = "migrations/0007_users_contact/sqlite3.sql",
        [SQL_MYSQL]    = "migrations/0007_users_contact/mysql.sql",
        [SQL_POSTGRES] = "migrations/0007_users_contact/postgres.sql",
    }},
    { "0008_role_workspace", {
        [SQL_SQLITE]   = "migrations/0008_role_workspace/sqlite3.sql",
        [SQL_MYSQL]    = "migrations/0008_role_workspace/mysql.sql",
        [SQL_POSTGRES] = "migrations/0008_role_workspace/postgres.sql",
    }},
    { "0009_geo_admin", {
        [SQL_SQLITE]   = "migrations/0009_geo_admin/sqlite3.sql",
        [SQL_MYSQL]    = "migrations/0009_geo_admin/mysql.sql",
        [SQL_POSTGRES] = "migrations/0009_geo_admin/postgres.sql",
    }},
    { "0010_account_switch", {
        [SQL_SQLITE]   = "migrations/0010_account_switch/sqlite3.sql",
        [SQL_MYSQL]    = "migrations/0010_account_switch/mysql.sql",
        [SQL_POSTGRES] = "migrations/0010_account_switch/postgres.sql",
    }},
    { "0011_seed_config", {
        [SQL_SQLITE]   = "migrations/0011_seed_config/sqlite3.sql",
        [SQL_MYSQL]    = "migrations/0011_seed_config/mysql.sql",
        [SQL_POSTGRES] = "migrations/0011_seed_config/postgres.sql",
    }},
};

/* Bootstrap table recording which migrations are already applied. It must
 * exist before the history can be read, so it cannot itself live in a
 * migration file - and its DDL differs per dialect (DATETIME vs TIMESTAMP).
 *
 * `query` is LONGTEXT on MySQL: the column holds whole migration files, and
 * TEXT stops at 64 KiB - a migration over that limit would be rejected
 * (strict mode) or truncated (non-strict), and a truncated history row can
 * never match the file again, which is a permanent mismatch on every open.
 * Databases created while this said TEXT are widened in create_schema_status()
 * with a best-effort ALTER right after the CREATE. */
static const char *const migrations_table_sql[SQL_LANG_COUNT] = {
    [SQL_SQLITE]   = "CREATE TABLE IF NOT EXISTS Migrations (\n"
                     "    applied_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,\n"
                     "    query TEXT NOT NULL\n"
                     ");\n",
    [SQL_MYSQL]    = "CREATE TABLE IF NOT EXISTS Migrations (\n"
                     "    applied_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,\n"
                     "    query LONGTEXT NOT NULL\n"
                     ");\n",
    [SQL_POSTGRES] = "CREATE TABLE IF NOT EXISTS Migrations (\n"
                     "    applied_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,\n"
                     "    query TEXT NOT NULL\n"
                     ");\n",
};

// Existing MySQL databases were created with `query TEXT`; widen them once.
// Idempotent (MODIFY to the same type succeeds), best-effort: a failure
// leaves the old limit in place and is logged by exec_script, never fatal.
static const char *const migrations_table_widen_sql[SQL_LANG_COUNT] = {
    [SQL_SQLITE]   = NULL,
    [SQL_MYSQL]    = "ALTER TABLE Migrations MODIFY query LONGTEXT NOT NULL;",
    [SQL_POSTGRES] = NULL,
};

/* Migration file contents for the active dialect, read from disk once per
 * process - the same lifetime the inline strings used to have.
 *
 * Indexed BY DIALECT, not by "the current dialect": a process may hold two
 * databases of different dialects open at the same time (webc_open_db_spec),
 * so one shared buffer that got reloaded whenever the dialect changed would
 * free text another connection is about to read. */
static String_Builder migration_bufs[SQL_LANG_COUNT][ARRAY_LEN(migrations)];
static bool migration_files_loaded[SQL_LANG_COUNT];

static bool migration_files_load(sql_lang_t lang) {
    if (lang >= SQL_LANG_COUNT) return false;
    if (migration_files_loaded[lang]) return true;
    for (size_t i = 0; i < ARRAY_LEN(migrations); ++i) {
        const char *path = migrations[i].paths[lang];
        if (!path) {
            fprintf(stderr, "ERROR: migration %s has no file for SQL dialect %d\n",
                    migrations[i].name, (int) lang);
            return false;
        }
        if (!read_entire_file(path, &migration_bufs[lang][i])) return false;
        sb_append_null(&migration_bufs[lang][i]);  // callers treat items as a C string
    }
    migration_files_loaded[lang] = true;
    return true;
}

static const char *migration_sql(sql_lang_t lang, size_t index) {
    return migration_bufs[lang][index].items;
}

// Result of checking/applying the migrations history.
typedef enum {
    SCHEMA_OK = 0,            // history matches and all pending migrations are applied
    SCHEMA_ERROR,             // SQL/internal error (nothing was committed)
    SCHEMA_HISTORY_MISMATCH,  // an already-applied migration differs from the
                              // migration files (the history was rewritten)
                              // -> caller may rebuild
} Schema_Status;

static Schema_Status create_schema_status(db_t *db, const char *webc_path);

static Schema_Status create_schema_status(db_t *db, const char *webc_path) {
    Schema_Status result = SCHEMA_OK;
    sql_stmt stmt = {0};
    bool began = false;
    bool fk_off = false;
    char **stored = NULL;          // history rows, copied out of the result
    size_t stored_count = 0;
    size_t stored_capacity = 0;
    bool *stored_used = NULL;      // which stored row matched a file
    bool *matched = NULL;          // which migrations[] entry is applied

    if (!migration_files_load(db->lang)) return_defer(SCHEMA_ERROR);

    // Migrations may swap a table other tables point at (0003 rebuilds the
    // demo `users` into the POS shape while notes.user_id references it).
    // SQLite enforces DROP/RENAME against live FKs and PRAGMA foreign_keys
    // is a no-op inside a transaction, so toggle it around the whole apply -
    // the same pattern the restore path below uses. Only the sqlite dialect
    // needs this: the ported mysql/postgres files carry no REFERENCES clauses
    // that could block a DROP.
    if (db->lang == SQL_SQLITE) {
        if (sqlite3_exec((sqlite3 *) db->conn, "PRAGMA foreign_keys=OFF;",
                         NULL, NULL, NULL) != SQLITE_OK) {
            nob_log(NOB_ERROR, "disabling foreign keys for migrations: %s",
                    db_errmsg(db));
            return_defer(SCHEMA_ERROR);
        }
        fk_off = true;
    }

    if (!sql_txn_begin(db)) return_defer(SCHEMA_ERROR);
    began = true;

    if (!sql_exec_script(db, migrations_table_sql[db->lang])) {
        return_defer(SCHEMA_ERROR);
    }
    if (migrations_table_widen_sql[db->lang]) {
        // Best effort on purpose: databases created with the old `query TEXT`
        // keep working while it fails, and sql_exec_script() logs the reason.
        sql_exec_script(db, migrations_table_widen_sql[db->lang]);
    }

    if (!sql_prepare(db, "SELECT query FROM Migrations;", &stmt)) {
        return_defer(SCHEMA_ERROR);
    }

    // Read the whole history before judging it. The table carries no ORDER BY
    // guarantee - InnoDB invents a hidden key when none is declared, Postgres
    // promises nothing - so rows are matched BY CONTENT below instead of by
    // position. Content matching keeps D5's property (an edited file can
    // never silently pass) without relying on an order no backend promises.
    Sql_Step rc = sql_step(&stmt);
    for (; rc == SQL_ROW; rc = sql_step(&stmt)) {
        if (stored_count >= stored_capacity) {
            size_t next = stored_capacity ? stored_capacity * 2 : 16;
            char **grown = realloc(stored, next * sizeof(*stored));
            if (!grown) return_defer(SCHEMA_ERROR);
            stored = grown;
            stored_capacity = next;
        }
        const char *query = sql_column_text(&stmt, 0);
        stored[stored_count] = strdup(query ? query : "");
        if (!stored[stored_count]) return_defer(SCHEMA_ERROR);
        stored_count += 1;
    }
    if (rc != SQL_DONE) {
        nob_log(NOB_ERROR, "reading migration history: %s", db_errmsg(db));
        return_defer(SCHEMA_ERROR);
    }
    sql_finalize(&stmt);

    if (stored_count > ARRAY_LEN(migrations)) {
        fprintf(stderr, "ERROR: %s: Database scheme is too new. Contains "
                "more migrations applied than expected. Update your "
                "application.\n", webc_path);
        return_defer(SCHEMA_ERROR);
    }

    stored_used = calloc(stored_count ? stored_count : 1, sizeof(*stored_used));
    matched = calloc(ARRAY_LEN(migrations), sizeof(*matched));
    if (!stored_used || !matched) return_defer(SCHEMA_ERROR);

    // Every stored row must be exactly some file's text, and every file
    // consumed at most one row. First-fit is enough: identical texts are
    // interchangeable, and a permutation of the real history still matches.
    for (size_t i = 0; i < ARRAY_LEN(migrations); ++i) {
        const char *expected = migration_sql(db->lang, i);
        if (!expected) continue;
        for (size_t j = 0; j < stored_count; ++j) {
            if (stored_used[j]) continue;
            if (strcmp(stored[j], expected) == 0) {
                stored_used[j] = true;
                matched[i] = true;
                break;
            }
        }
    }

    // A row no migration file accounts for = history this checkout does not
    // own (the history was rewritten, or a shipped migration file was edited).
    // Same verdict as the old position-based compare: rebuild, or reject.
    for (size_t j = 0; j < stored_count; ++j) {
        if (stored_used[j]) continue;
        size_t i = 0;
        while (i < ARRAY_LEN(migrations) && matched[i]) ++i;
        const char *name     = i < ARRAY_LEN(migrations) ? migrations[i].name : "?";
        const char *expected = i < ARRAY_LEN(migrations)
                             ? migration_sql(db->lang, i) : "";
        fprintf(stderr, "ERROR: %s: Invalid database scheme. Mismatch in "
                "migration %s:\n", webc_path, name);
        fprintf(stderr, "EXPECTED: %s\n", expected);
        fprintf(stderr, "FOUND: %s\n", stored[j][0] ? stored[j] : "(null)");
        return_defer(SCHEMA_HISTORY_MISMATCH);
    }

    // Unmatched files are pending migrations (fewer rows than files is the
    // normal "some migrations not applied yet" state).
    for (size_t i = 0; i < ARRAY_LEN(migrations); ++i) {
        if (matched[i]) continue;
        printf("INFO: %s: applying migration %s\n", webc_path,
               migrations[i].name);
        const char *mig = migration_sql(db->lang, i);
        if (WEBC_TRACE_MIGRATION_QUERIES) printf("%s\n", mig);
        if (!sql_exec_script(db, mig)) return_defer(SCHEMA_ERROR);

        if (!sql_prepare(db, "INSERT INTO Migrations (query) VALUES (?)",
                         &stmt)) {
            return_defer(SCHEMA_ERROR);
        }
        bool ok = sql_bind(&stmt, 1, SQL_SV(sv_from_cstr(mig)))
                  && sql_final_step(&stmt);
        sql_finalize(&stmt);
        if (!ok) return_defer(SCHEMA_ERROR);
    }

defer:
    sql_finalize(&stmt);
    for (size_t j = 0; j < stored_count; ++j) free(stored[j]);
    free(stored);
    free(stored_used);
    free(matched);
    if (result == SCHEMA_OK) {
        if (!sql_txn_commit(db)) result = SCHEMA_ERROR;
    } else if (began) {
        // Nothing is committed on error/mismatch; close the open transaction
        // explicitly so the connection is in a clean state. (ROLLBACK outside
        // of a transaction just reports an error, which we skip.)
        sql_txn_rollback(db);
    }
    // Always restore FK enforcement for the request-serving connection,
    // whether the apply succeeded or bailed out half way.
    if (fk_off) {
        sqlite3_exec((sqlite3 *) db->conn, "PRAGMA foreign_keys=ON;",
                     NULL, NULL, NULL);
    }
    return result;
}

/* ---------------------------------------------------------------------------
 * Backup/restore support: when migrations[] history was rewritten we rebuild
 * the database from scratch, but keep the user data:
 *   1. db            -> db.bak     (old file, untouched)
 *   2. fresh db is created and migrated from migrations[]
 *   3. every table/column both databases share is copied from db.bak into
 *      the new schema (db_restore_from_backup)
 * ---------------------------------------------------------------------------
 */
static const char *db_backup_path(const char *path) {
    return temp_sprintf("%s.bak", path);
}

// Moves the current database file out of the way so a fresh one can be
// created. The connection MUST be closed before calling this (so SQLite has
// checkpointed and removed the -wal/-shm files). `path` is the sqlite file of
// the connection being rebuilt - not the process-wide global, so a database
// opened through webc_open_db_spec() rebuilds itself and not the primary one.
static bool db_backup_and_reset(const char *path) {
    const char *bak     = db_backup_path(path);
    const char *wal     = temp_sprintf("%s-wal", path);
    const char *shm     = temp_sprintf("%s-shm", path);
    const char *bak_wal = temp_sprintf("%s.bak-wal", path);
    const char *bak_shm = temp_sprintf("%s.bak-shm", path);

    if (file_exists(bak) > 0 && !delete_file(bak)) return false;
    if (file_exists(bak_wal) > 0 && !delete_file(bak_wal)) return false;
    if (file_exists(bak_shm) > 0 && !delete_file(bak_shm)) return false;

    if (!nob_rename(path, bak)) return false;

    // Leftovers of the renamed file must not be picked up by the new database
    if (file_exists(wal) > 0 && !delete_file(wal)) return false;
    if (file_exists(shm) > 0 && !delete_file(shm)) return false;
    return true;
}

static bool str_ieq(const char *a, const char *b) {
    size_t n = strlen(a);
    return n == strlen(b) && sqlite3_strnicmp(a, b, (int)n) == 0;
}

static void sb_append_quoted_ident(String_Builder *sb, const char *s) {
    sb_append_cstr(sb, "\"");
    for (; *s; ++s) {
        if (*s == '"') sb_append_cstr(sb, "\"\"");
        else sb_append_buf(sb, s, 1);
    }
    sb_append_cstr(sb, "\"");
}

#define MAX_TABLE_COLS 64
typedef struct {
    char name[128];
} Col_Name;
typedef struct {
    Col_Name items[MAX_TABLE_COLS];
    size_t count;
} Column_List;

// PRAGMA <schema>.table_info(<table>) -> column names of that table.
// A table that does not exist simply yields count == 0.
static bool get_table_columns(sqlite3     *db,
                              const char  *schema,
                              const char  *table,
                              Column_List *out)
{
    bool result = true;
    sqlite3_stmt *stmt = NULL;
    String_Builder sql = {0};

    out->count = 0;
    sb_append_cstr(&sql, "PRAGMA ");
    sb_append_cstr(&sql, schema);
    sb_append_cstr(&sql, ".table_info(");
    sb_append_quoted_ident(&sql, table);
    sb_append_cstr(&sql, ");");
    sb_append_null(&sql);

    if (sqlite3_prepare_v2(db, sql.items, -1, &stmt, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        if (out->count >= MAX_TABLE_COLS) {
            fprintf(stderr, "ERROR: table `%s` has more than %d columns, "
                    "cannot restore it automatically\n", table, MAX_TABLE_COLS);
            return_defer(false);
        }
        snprintf(out->items[out->count].name, sizeof(out->items[0].name), "%s", name);
        out->count += 1;
    }

defer:
    if (stmt) sqlite3_finalize(stmt);
    sb_free(sql);
    return result;
}

// Copies all tables that exist in both databases (matching columns by name,
// case-insensitively) from the backup into the freshly migrated database.
// Foreign key checks are toggled off inside (the connection now comes up
// with them on) so rows can be inserted in any order.
static bool db_restore_from_backup(sqlite3 *db, const char *bak_path) {
    bool result = true;
    bool attached = false;
    sqlite3_stmt *tables_stmt = NULL;
    String_Builder sql = {0};
    char tables[64][128] = {0};
    size_t tables_count = 0;

    sb_append_cstr(&sql, "ATTACH DATABASE '");
    for (const char *p = bak_path; *p; ++p) {
        if (*p == '\'') sb_append_cstr(&sql, "''");
        else sb_append_buf(&sql, p, 1);
    }
    sb_append_cstr(&sql, "' AS bak");
    sb_append_null(&sql);

    if (sqlite3_exec(db, sql.items, NULL, NULL, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }
    attached = true;

    // Run the copy with foreign key checks off: a note may reference a user
    // that is restored later. Re-enabled in the defer below, which always
    // runs outside the transaction.
    if (sqlite3_exec(db, "PRAGMA foreign_keys=OFF;", NULL, NULL, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    if (!txn_begin(db)) return_defer(false);

    // Materialize the table list first so we do not hold a statement open
    // while inserting into the main database.
    if (sqlite3_prepare_v2(db,
            "SELECT name FROM bak.sqlite_master WHERE type='table' "
            "AND name NOT LIKE 'sqlite_%' AND name <> 'Migrations' ORDER BY name;",
            -1, &tables_stmt, NULL) != SQLITE_OK) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }

    int step = SQLITE_ROW;
    while ((step = sqlite3_step(tables_stmt)) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(tables_stmt, 0);
        if (tables_count >= ARRAY_LEN(tables)) {
            fprintf(stderr, "ERROR: backup contains more than %zu tables, "
                    "cannot restore it automatically\n", ARRAY_LEN(tables));
            return_defer(false);
        }
        snprintf(tables[tables_count], sizeof(tables[0]), "%s", name);
        tables_count += 1;
    }
    if (step != SQLITE_DONE) {
        LOG_SQLITE3_ERROR(db);
        return_defer(false);
    }
    sqlite3_finalize(tables_stmt);
    tables_stmt = NULL;

    for (size_t i = 0; i < tables_count; ++i) {
        const char *old_name = tables[i];
        Column_List old_cols = {0}, new_cols = {0};

        if (!get_table_columns(db, "bak", old_name, &old_cols)) return_defer(false);
        if (!get_table_columns(db, "main", old_name, &new_cols)) return_defer(false);

        if (new_cols.count == 0) {
            printf("INFO: backup table `%s` has no counterpart in the new "
                    "schema, skipped\n", old_name);
            continue;
        }

        String_Builder cols = {0};
        String_Builder sel = {0};
        size_t shared = 0;
        for (size_t j = 0; j < new_cols.count; ++j) {
            const char *col = new_cols.items[j].name;
            bool exists = false;
            for (size_t k = 0; k < old_cols.count; ++k) {
                if (str_ieq(col, old_cols.items[k].name)) {
                    exists = true;
                    break;
                }
            }
            if (!exists) continue;
            if (shared > 0) {
                sb_append_cstr(&cols, ", ");
                sb_append_cstr(&sel, ", ");
            }
            sb_append_quoted_ident(&cols, col);
            sb_append_quoted_ident(&sel, col);
            shared += 1;
        }

        if (shared == 0) {
            printf("INFO: backup table `%s` shares no columns with the new "
                    "schema, skipped\n", old_name);
            sb_free(cols);
            sb_free(sel);
            continue;
        }

        sql.count = 0;
        sb_append_cstr(&sql, "INSERT INTO ");
        sb_append_quoted_ident(&sql, old_name);
        sb_append_cstr(&sql, " (");
        sb_append_buf(&sql, cols.items, cols.count);
        sb_append_cstr(&sql, ") SELECT ");
        sb_append_buf(&sql, sel.items, sel.count);
        sb_append_cstr(&sql, " FROM bak.");
        sb_append_quoted_ident(&sql, old_name);
        sb_append_cstr(&sql, ";");
        sb_append_null(&sql);
        sb_free(cols);
        sb_free(sel);

        if (sqlite3_exec(db, sql.items, NULL, NULL, NULL) != SQLITE_OK) {
            fprintf(stderr, "ERROR: restoring table `%s` failed: %s\n",
                    old_name, sqlite3_errmsg(db));
            return_defer(false);
        }
        printf("INFO: restored %d row(s) into `%s`\n", sqlite3_changes(db), old_name);
    }

    if (!txn_commit(db)) return_defer(false);

defer:
    if (tables_stmt) sqlite3_finalize(tables_stmt);
    if (!result) sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    if (attached) sqlite3_exec(db, "DETACH DATABASE bak;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
    sb_free(sql);
    return result;
}

/* -- dialect + connection string ------------------------------------------
 * Resolution order:
 *   1. WEBC_DEFAULT_SQL - "SQLITE3"/"MYSQL"/"POSTGRES" (case-insensitive),
 *      the explicit choice; the prefix is then only used to find the DSN.
 *   2. `mysql:` / `postgres:` prefix on WEBC_DB_PATH - one variable that
 *      carries both dialect and connection string.
 *   3. SQLite - the default, and also the answer when WEBC_DB_PATH is still
 *      NULL (this is reachable: the globals are read by code that can run
 *      before main() has set them, and a NULL must not segfault).
 */
sql_lang_t webc_db_lang(void) {
    if (WEBC_DEFAULT_SQL && WEBC_DEFAULT_SQL[0]) {
        if (str_ieq(WEBC_DEFAULT_SQL, "SQLITE") ||
            str_ieq(WEBC_DEFAULT_SQL, "SQLITE3")) return SQL_SQLITE;
        if (str_ieq(WEBC_DEFAULT_SQL, "MYSQL")) return SQL_MYSQL;
        if (str_ieq(WEBC_DEFAULT_SQL, "POSTGRES") ||
            str_ieq(WEBC_DEFAULT_SQL, "POSTGRESQL")) return SQL_POSTGRES;
        nob_log(NOB_ERROR, "WEBC_DEFAULT_SQL: unknown dialect `%s` "
                "(want SQLITE3, MYSQL or POSTGRES); falling back to the "
                "WEBC_DB prefix", WEBC_DEFAULT_SQL);
    }
    if (!WEBC_DB_PATH) return SQL_SQLITE;
    if (strncmp(WEBC_DB_PATH, "mysql:", 6) == 0) return SQL_MYSQL;
    if (strncmp(WEBC_DB_PATH, "postgres:", 9) == 0) return SQL_POSTGRES;
    return SQL_SQLITE;
}

const char *webc_db_dsn(void) {
    if (!WEBC_DB_PATH) return "";  // db_open() rejects an empty dsn
    sql_lang_t lang = webc_db_lang();
    if (lang == SQL_MYSQL && strncmp(WEBC_DB_PATH, "mysql:", 6) == 0) {
        return WEBC_DB_PATH + 6;
    }
    if (lang == SQL_POSTGRES && strncmp(WEBC_DB_PATH, "postgres:", 9) == 0) {
        return WEBC_DB_PATH + 9;
    }
    return WEBC_DB_PATH;
}

/* -- schema verification cache --------------------------------------------
 * create_schema_status() reads the whole migration history (2.4 MB of
 * 0009_geo_admin) and runs DDL. Its verdict depends only on (dialect, dsn),
 * so it is computed once per pair per process; every later open of the same
 * database just gets a fresh connection. Without this the pool would check
 * the same history 8 times at startup, and the old per-request open checked
 * it on EVERY request. A database modified by another process mid-run is not
 * re-detected - the same trade-off every migration tool makes. */
typedef struct {
    sql_lang_t lang;
    char      *dsn;
} Verified_Db;
static Verified_Db *verified_items = NULL;
static size_t verified_count = 0;

static bool schema_verified_has(sql_lang_t lang, const char *dsn) {
    for (size_t i = 0; i < verified_count; ++i) {
        if (verified_items[i].lang == lang &&
            strcmp(verified_items[i].dsn, dsn) == 0) {
            return true;
        }
    }
    return false;
}

static void schema_verified_add(sql_lang_t lang, const char *dsn) {
    // A cache: losing an entry only costs a repeat of the check, so an
    // allocation failure is silently tolerated.
    Verified_Db *grown = realloc(verified_items,
                                 (verified_count + 1) * sizeof(*verified_items));
    if (!grown) return;
    verified_items = grown;
    char *copy = strdup(dsn);
    if (!copy) return;
    verified_items[verified_count].lang = lang;
    verified_items[verified_count].dsn = copy;
    verified_count += 1;
}

// Opens one connection and proves its schema (once per process per pair).
// `label` is what messages name (the spec the caller asked for); `dsn` is
// what the driver gets and, for sqlite, what the rebuild path renames.
// `symlink_hint` explains a failed sqlite open when WEBC_DIR_PATH is a
// symlink (primary-database legacy behaviour).
static void print_symlink_note(void);
static db_t *db_open_checked(sql_lang_t lang, const char *dsn,
                             const char *label, bool symlink_hint)
{
    bool open_failed = false;

    if (schema_verified_has(lang, dsn)) {
        db_t *db = db_open(lang, dsn);
        if (!db) open_failed = true;
        if (open_failed && symlink_hint) print_symlink_note();
        return open_failed ? NULL : db;
    }

    bool rebuilt = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        // db_open creates parent directories and applies the connection
        // PRAGMAs (WAL, synchronous, foreign_keys, cache/temp/mmap) -
        // everything SQLite refuses to run inside a transaction, so it all
        // happens before create_schema_status() opens one.
        db_t *db = db_open(lang, dsn);
        if (!db) {
            if (symlink_hint) print_symlink_note();
            return NULL;
        }

        Schema_Status status = create_schema_status(db, label);
        if (status == SCHEMA_OK) {
            schema_verified_add(lang, dsn);
            if (rebuilt) {
                const char *bak = db_backup_path(dsn);
                if (!db_restore_from_backup((sqlite3 *) db->conn, bak)) {
                    fprintf(stderr, "ERROR: could not copy the data back from "
                            "%s. Your previous data is still safe there; the "
                            "application continues with an empty schema.\n", bak);
                }
            }
            return db;
        }

        if (status == SCHEMA_HISTORY_MISMATCH && attempt == 0
            && lang == SQL_SQLITE) {
            // migrations[] history was rewritten: move the old database aside,
            // rebuild from scratch and copy the data over afterwards.
            fprintf(stderr, "NOTE: migrations[] history changed. Backing up "
                    "%s to %s and rebuilding the schema...\n",
                    label, db_backup_path(dsn));
            db_close(db);
            if (!db_backup_and_reset(dsn)) return NULL;
            rebuilt = true;
            continue;
        }

        if (status == SCHEMA_HISTORY_MISMATCH) {
            fprintf(stderr, "ERROR: %s: migration history does not match the "
                    "migration files; the automatic rebuild exists only for "
                    "the sqlite dialect - fix or recreate the database.\n",
                    label);
        }
        db_close(db);
        return NULL;
    }
    return NULL;  // attempt 1 either returned above or failed open/verify
}

// Legacy file->directory layout dance for the PRIMARY sqlite database (extra
// databases come from webc_open_db_spec(); the sqlite driver creates their
// parent directories itself). Sets *symlink_hint when WEBC_DIR_PATH is a
// symlink, so a later failed open can explain it.
static bool webc_prepare_primary_layout(bool *symlink_hint) {
    int exists = file_exists(WEBC_DIR_PATH);
    if (exists < 0) return false;

    if (!exists) {
        if (!db_mkdir_recursive(WEBC_DIR_PATH)) return false;
        return true;
    }

    File_Type type = get_file_type(WEBC_DIR_PATH);
    if (type < 0) return false;
    switch (type) {
    case FILE_DIRECTORY: break;
    case FILE_REGULAR: {
        nob_log(INFO, "%s is a file! Migrating it to a directory...", WEBC_DIR_PATH);
        const char *webc_tmp_db_path = temp_sprintf("%s.tmp", WEBC_DIR_PATH);
        if (!nob_rename(WEBC_DIR_PATH, webc_tmp_db_path)) return false;
        if (!db_mkdir_recursive(WEBC_DIR_PATH)) return false;
        if (!nob_rename(webc_tmp_db_path, WEBC_DB_PATH)) return false;
    } break;
    case FILE_SYMLINK: {
        *symlink_hint = true;
    } break;
    case FILE_OTHER: {
        fprintf(stderr, "ERROR: %s is a weird file! We expect it to be a "
                "directory or a regular file in case of a legacy database...\n",
                WEBC_DIR_PATH);
        return false;
    } break;
    }
    return true;
}

static void print_symlink_note(void) {
    if (!WEBC_DIR_PATH || !WEBC_DB_PATH) return;
    fprintf(stderr, "NOTE: Your %s is a symlink! We used to expect this "
            "path to lead to an sqlite3 database file, but at some point "
            "we changed it to a directory. And now the database file is "
            "expected to be at %s. If you are using some clever symlink "
            "setup, please update it accordingly so we could open %s as "
            "the sqlite3 database.\n", WEBC_DIR_PATH, WEBC_DB_PATH, WEBC_DB_PATH);
}

// Primary database, opened directly (this is what the pool is filled with).
static db_t *open_webc_db_raw(void) {
    if (!WEBC_DB_PATH || !WEBC_DB_PATH[0]) {
        nob_log(NOB_ERROR, "no database configured - set WEBC_DB or "
                "WEBC_CONNECTION_STRING, or edit WEBC_DEFAULT_DSN in "
                "src/webc.c");
        return NULL;
    }
    sql_lang_t lang = webc_db_lang();
    const char *dsn = webc_db_dsn();
    bool symlink_hint = false;

    if (lang == SQL_SQLITE && !webc_prepare_primary_layout(&symlink_hint)) {
        return NULL;
    }
    return db_open_checked(lang, dsn, WEBC_DB_PATH, symlink_hint);
}

db_t *webc_open_db_spec(const char *spec) {
    if (!spec || !spec[0]) return NULL;

    sql_lang_t lang;
    const char *dsn;
    if (strncmp(spec, "mysql:", 6) == 0) {
        lang = SQL_MYSQL;
        dsn = spec + 6;
    } else if (strncmp(spec, "postgres:", 9) == 0) {
        lang = SQL_POSTGRES;
        dsn = spec + 9;
    } else {
        // No prefix on the spec: an explicit WEBC_DEFAULT_SQL decides,
        // otherwise a bare path/DSN means SQLite (the WEBC_DB prefix is a
        // property of the PRIMARY database and must not leak in here).
        lang = SQL_SQLITE;
        if (WEBC_DEFAULT_SQL && WEBC_DEFAULT_SQL[0]) lang = webc_db_lang();
        dsn = spec;
    }

    db_t *db = db_open_checked(lang, dsn, spec, false);
    if (db) db->pool_slot = -1;  // caller owns it; db_close() destroys it
    return db;
}

/* -- connection pool (primary database) -----------------------------------
 * LAZY + grow-on-demand: nothing connects at startup (serve.c does not
 * warm the pool - a static file server or `webc help` never touches a
 * database). The first checkout opens connection 1; later checkouts open
 * another slot only when every existing one is genuinely in use, up to
 * DB_POOL_SIZE. Every slot tracks in_use, so a caller never holds two
 * handles, and db_close() releases it (pooled handles carry pool_slot >= 0,
 * so every existing `open_webc_db(); ...; db_close();` call site keeps
 * working unchanged).
 *
 * Release undoes what a request left behind before the next one sees it:
 *   - an open transaction is rolled back (a handler that returned between
 *     BEGIN and COMMIT must not leak its txn onto a shared handle);
 *   - a sqlite connection with a leaked statement is recycled, because a
 *     leaked statement pins a read transaction on a handle others share.
 *
 * The FIRST open failure LATCHES the pool: db_pool_get() returns NULL
 * forever after (one error, no per-request retry storm). A failed GROW
 * later on does not latch - it degrades to a transient, unpooled
 * connection so a burst costs "one connection per request" instead of a
 * 500. One process talks to exactly one server: every slot opens the same
 * primary DSN. */
typedef struct {
    db_t *db;
    bool  in_use;
} Pool_Slot;

static Pool_Slot db_pool[DB_POOL_SIZE];
static int db_pool_index = 0;
static int  db_pool_count = 0;        // slots open so far (lazy: 0 at start)
static bool db_pool_latched = false;  // first open failed -> refuse forever

// The "not pooled" teardown: close the connection and free the handle.
static void db_destroy(db_t *db) {
    if (!db) return;
    if (db->drv && db->drv->close) db->drv->close(db);
    free(db->dsn);
    free(db);
}

// Open one more slot at the next index. False = open failed; the caller
// decides what that means (first ever open -> latch, later grow -> degrade
// to a transient connection).
static bool pool_grow(void) {
    db_t *db = open_webc_db_raw();
    if (!db) {
        fprintf(stderr, "ERROR: Failed to initialize DB pool connection %d\n",
                db_pool_count);
        return false;
    }
    db->pool_slot = db_pool_count;
    db_pool[db_pool_count].db = db;
    db_pool[db_pool_count].in_use = false;
    db_pool_count++;
    if (db_pool_count == 1)
        nob_log(NOB_INFO, "DB pool: first connection open (lazy)");
    return true;
}

// Hand a pooled connection back. Idempotent: releasing a slot that is already
// free (double close) is ignored rather than double-freed.
static void db_pool_release(db_t *db) {
    if (!db || db->pool_slot < 0) return;
    int slot = db->pool_slot;
    if (slot >= DB_POOL_SIZE || db_pool[slot].db != db
        || !db_pool[slot].in_use) {
        return;
    }

    if (db->txn_depth > 0) {
        nob_log(NOB_ERROR, "db_close: %s returned with %d open transaction(s); "
                "rolling back", db->dsn ? db->dsn : "connection", db->txn_depth);
        if (db->drv && db->drv->txn_rollback) db->drv->txn_rollback(db);
        db->txn_depth = 0;
    }

    if (db->lang == SQL_SQLITE && db->conn
        && sqlite3_next_stmt((sqlite3 *) db->conn, NULL)) {
        // Recycle: sqlite3_close() below becomes close_v2() (a zombie kept
        // alive by the leaked statement), and the slot gets a clean handle
        // with all connection PRAGMAs applied again.
        nob_log(NOB_ERROR, "db_close: leaked statement on %s; recycling the "
                "pooled connection", db->dsn ? db->dsn : "connection");
        db->drv->close(db);
        db->conn = NULL;
        if (db->drv->open(db, db->dsn)) {
            db->txn_depth = 0;
        } else {
            nob_log(NOB_ERROR, "db_close: could not recycle %s (%s); dropping "
                    "the pool slot", db->dsn ? db->dsn : "connection",
                    db->err[0] ? db->err : db->drv->errmsg(db));
            db_destroy(db);
            db_pool[slot] = (Pool_Slot) {0};
            return;
        }
    }

    db_pool[slot].in_use = false;
}

db_t *db_pool_get(void) {
    if (db_pool_latched) return NULL;  // one clean refusal, no storm

    // First use opens connection 1; its failure latches the pool (bad DSN
    // or refused schema: fail once, then every request gets NULL).
    if (db_pool_count == 0 && !pool_grow()) {
        db_pool_latched = true;
        return NULL;
    }

    for (int n = 0; n < db_pool_count; ++n) {
        int i = (db_pool_index + n) % db_pool_count;
        if (!db_pool[i].db || db_pool[i].in_use) continue;
        db_pool[i].in_use = true;
        db_pool_index = (i + 1) % db_pool_count;
        return db_pool[i].db;
    }

    // Every slot busy: grow toward DB_POOL_SIZE. A failed grow does not
    // latch (the pool itself is serving) - fall through to transient.
    if (db_pool_count < DB_POOL_SIZE && pool_grow()) {
        int i = db_pool_count - 1;
        db_pool[i].in_use = true;
        db_pool_index = (i + 1) % db_pool_count;
        return db_pool[i].db;
    }

    // Degraded: a transient connection (pool_slot -1, destroyed by
    // db_close) so a burst costs one connection per request instead of a 500.
    db_t *db = open_webc_db_raw();
    if (db) db->pool_slot = -1;
    return db;
}

db_t *open_webc_db(void) {
    // Pool checkout: call sites keep their `open_webc_db(); ...; db_close();`
    // shape, the pool and the once-per-database schema check are invisible
    // to them. NULL means "the database is unavailable" (serve 500), which
    // every call site already handles.
    return db_pool_get();
}

void db_close(db_t *db) {
    if (!db) return;
    if (db->pool_slot >= 0) {
        db_pool_release(db);
        return;
    }
    db_destroy(db);
}
