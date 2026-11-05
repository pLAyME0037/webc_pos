#ifndef SRC_DB_H_
#define SRC_DB_H_

#include <stdbool.h>
#include "sqlite3.h"
#include "sql.h"

#define LOG_SQLITE3_ERROR(db) fprintf(stderr, "%s:%d: SQLITE3 ERROR: %s\n", __FILE__, __LINE__, sqlite3_errmsg(db))

extern const char *WEBC_DIR_PATH;
extern const char *WEBC_DB_PATH;
// Dialect override from the WEBC_DEFAULT_SQL environment variable
// ("SQLITE3" / "MYSQL" / "POSTGRES", case-insensitive). When it is set it
// wins over the `mysql:`/`postgres:` prefix convention; when it is NULL the
// prefix decides, and no prefix means SQLite. Set in main() before anything
// touches the database.
extern const char *WEBC_DEFAULT_SQL;
extern bool WEBC_TRACE_MIGRATION_QUERIES;

bool txn_begin(sqlite3 *db);
bool txn_commit(sqlite3 *db);
bool txn_rollback(sqlite3 *db);
// Dialect + connection string. Resolution order (both in webc_db_lang()):
//   1. WEBC_DEFAULT_SQL            - explicit dialect, prefix ignored
//   2. `mysql:` / `postgres:` prefix on WEBC_DB_PATH
//   3. anything else (incl. NULL)  - SQLite file path
sql_lang_t webc_db_lang(void);
const char *webc_db_dsn(void);  // WEBC_DB_PATH without the dialect prefix

// Primary database, pooled: checks a connection out of the pool (round-robin
// with in-use tracking) and db_close() hands it back. Every call site in the
// app does `db = open_webc_db(); ...; db_close(db);` - the pool is invisible
// to them. Returns NULL when the pool is unavailable (bad DSN / schema
// refused at startup); callers already treat NULL as "serve 500".
db_t *open_webc_db(void);

// Opens an ADDITIONAL database, described by `spec`, independent of the
// primary one: "mysql:...", "postgres:..." or a sqlite file path (an explicit
// WEBC_DEFAULT_SQL is honoured when `spec` carries no prefix). The schema is
// verified for that (dialect, DSN) pair once per process, like the primary.
// Not pooled - the caller owns the handle and must db_close() it. This is the
// supported way to talk to two databases (even two dialects) at once.
db_t *webc_open_db_spec(const char *spec);

void db_close(db_t *db);        // pooled -> release, otherwise destroy

// Connection pool of the primary database
#define DB_POOL_SIZE 8
db_t *db_pool_get(void);        // checkout: next free slot, else a transient
void  db_pool_put(db_t *db);    // release (db_close() does this for you)
void  db_pool_init(void);       // opens all 8 up front, once; failure latches
void  db_pool_cleanup(void);

#endif // SRC_DB_H_
