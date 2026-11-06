#ifndef SRC_DB_H_
#define SRC_DB_H_

#include <stdbool.h>
#include "sqlite3.h"
#include "sql.h"

#define LOG_SQLITE3_ERROR(db) fprintf(stderr, "%s:%d: SQLITE3 ERROR: %s\n", __FILE__, __LINE__, sqlite3_errmsg(db))

// Set once in main() (src/webc.c) from the environment and the compiled-in
// WEBC_DEFAULT_DSN; read-only afterwards. There is NO baked $HOME default:
// with nothing configured dsn stays NULL and no connection is ever
// attempted (open_webc_db() answers "no database configured").
typedef struct {
    const char *dsn;          // full DSN, dialect prefix intact; or NULL
    const char *dir;          // parent dir of a sqlite file DSN, "." for
                              // mysql/postgres (not file paths); NULL if no dsn
    const char *default_sql;  // WEBC_DEFAULT_SQL dialect pin; or NULL
} Db_Cfg;
extern Db_Cfg db_cfg;
#define WEBC_DB_PATH     (db_cfg.dsn)
#define WEBC_DIR_PATH    (db_cfg.dir)
#define WEBC_DEFAULT_SQL (db_cfg.default_sql)
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

// Primary database, pooled: checks a connection out of the pool (in-use
// tracking) and db_close() hands it back. Every call site in the
// app does `db = open_webc_db(); ...; db_close(db);` - the pool is invisible
// to them. Returns NULL when the database is unavailable (not configured,
// bad DSN or schema refused on first use); callers already treat NULL as
// "serve 500". Nothing is connected until this is first called.
db_t *open_webc_db(void);

// Opens an ADDITIONAL database, described by `spec`: "mysql:...",
// "postgres:..." or a sqlite file path (an explicit WEBC_DEFAULT_SQL is
// honoured when `spec` carries no prefix). Schema is verified per
// (dialect, DSN) pair once per process. Not pooled - the caller owns the
// handle and must db_close() it. OPT-IN ONLY: the design is one server per
// process; no production code calls this.
db_t *webc_open_db_spec(const char *spec);

void db_close(db_t *db);        // pooled -> release, otherwise destroy

// Connection pool of the primary database. LAZY: nothing connects at
// startup - the first checkout opens connection 1, the pool grows toward
// DB_POOL_SIZE only while every slot is genuinely busy, and a first-ever
// open failure latches the pool (one error, then NULL forever; no retry
// storm). Hand a connection back with db_close() - it is the release API.
// One process talks to exactly one server (the primary DSN);
// webc_open_db_spec() is the opt-in exception and no production code
// calls it.
#define DB_POOL_SIZE 8
db_t *db_pool_get(void);        // checkout: free slot, else grow, else transient

#endif // SRC_DB_H_
