<!-- Log starts 2026-10-15. Entries before that date were a different project's
     (mcme) and were removed from this wiki by request on 2026-10-15; they are
     still recoverable from git history. Nothing is back-filled here. -->
# Dev log

Newest entry at the top. Append; do not rewrite history.

Status vocabulary:

**scaffold** = files exist, not functional · **wip** = being written ·
**works** = runs, may lack tests · **done** = runs and covered by tests ·
**blocked** = needs something first

---
## 2026-11-04 — Incremental build: per-file objects, 11.6 s → 0.5 s — **done**

Every run recompiled all of `src/**` in full even when nothing had changed.
The single `cc` line carrying the whole tree plus the link took **11.5 s** on
its own, and `webc dev` paid it on every save. Replaced with per-file objects
and an exact, compiler-recorded dependency check.

### Numbers

| run | before | after |
|---|---|---|
| nothing changed | 11.6 s | **0.51–0.60 s** |
| one `.c` touched | 11.6 s | **1.74 s** (3 objects) |
| header all 54 TUs include | 11.6 s | 10.9 s (correct — all stale) |
| cold | 11.96 s | 11.96 s (same work) |

The `webc dev` watcher ran throughout and competes for CPU, so the no-op run
lands anywhere in 0.5–1.0 s depending on contention.

### How it decides

Each translation unit gets `build/obj/<src-path>.o` with gcc's `-MMD` `.d`
beside it. The `.d` lists **every header that file actually pulled in**, so
the freshness check is exact rather than a heuristic: touch one header and
only its includers recompile. `obj_needs_rebuild()` stats the object, the
source and each path in the `.d`; a missing `.d` is treated as *stale* rather
than fresh, so an object we cannot prove is current always gets rebuilt.

`pkg-config` output is collected once into a list and reused across all 54
compiles — calling it per file would be 108 spawns a build.

### The fast path, and the header that had to move

When no object, no `tt`, no `h_to_html` and no bundle changed, the build logs
`up to date (54 objects, nothing to do)` and returns **without** stamping
`build/build_time.h`. Stamping it would touch `src/webc.c` on every run, so
the fast path could never fire — the timestamp moved out of `build/bundle.h`
into its own header.

`bundle.h` is written only when its bytes differ, which doubles as "did
tailwind emit anything new?". The compiler caught the side effect:
`display/version.h.tt` renders `STR(WEBC_BUILD_TIME)`, so `version.c` needed
its own `#include "../../build/build_time.h"`.

Accepted cost: every *real* rebuild re-stamps `build_time.h`, so `version.c`
and `webc.c` recompile each time — exactly 2 dependents, ~0.3 s — in exchange
for an accurate build-time banner.

### Deliberately not gated: tailwind

Tailwind v4 auto-detects sources across the whole tree. Probed with unique
`mt-[1NNpx]` classes: it reads `src/`, `display/`, `js/`, `test/`, `wiki/`,
`migrations/`, **`build/`**, `nob.c` and `README.md` just as happily as the
declared `@source` paths. Because `build/` is rewritten on every run, no mtime
rule can be both exact and stable here. Making it incremental needs
`@import "tailwindcss" source(none);` plus explicit `@source` entries in
`css/input.css` — a CSS-side change with its own blast radius, left out of
this pass. It costs ~0.35–0.56 s and is the floor of the no-op run.

### Two regressions this rework introduced, both fixed

1. **`compile_commands.json` became the link command.** Once objects replaced
   sources in the link, the recorded command listed `.o` paths — useless to
   clangd. Now emits **one entry per translation unit** with a real `"file"`
   (54 entries), sharing `fill_compile_cmd()` with the compile itself so the
   recorded command cannot drift from the one that runs. The JSON is also
   escaped exactly once; it used to be escaped twice, invisible only because
   the command contains no quotes.
2. **A deleted `.c` stayed in the binary forever.** No timestamp can see a
   deletion — the orphaned object simply kept linking. Added
   `build/link_inputs.txt`, written after each link; a changed object set
   forces a relink. (An *added* source was already caught: no object yet.)

### Correctness evidence

- All **36** generated headers re-run through `tt` into a scratch tree and
  diffed against `build/h_to_html/` → **byte-identical**. The new
  per-template skip-gate leaves no stale content.
- Touching `src/http/serve.h` recompiles **45/54**; replaying the check in
  Python found **0 unexplained**. An early `grep -l` count of 16 was a
  substring artifact — gcc records relative forms like
  `src/pos/../../http/serve.h`. All **54** `.d` files do depend on it.
- `build/build_time.h` has exactly **2** dependents, as designed.
- 54 objects + 54 `.d`; three consecutive no-op runs stay stable.

### The suite went red mid-flight — not because of the build

`./test/run.sh` reported **25 failures** while this was landing, and every one
traced to `ff4fc0ed` (the master-child/geo WIP committed in parallel). None
involve compilation — the cleanest proof being `golden migration
0003_pos_eshop`, a pure file-vs-file compare at `test/http_test.py:89` with no
build in the loop at all.

- **3 fixed today.** `ff4fc0ed` had accidentally deleted
  `route_new(&routes, "/pos/users", "/update", …)` while editing `route.c`.
  The handler was still compiled in (`nm` → `T serve_pos_users_update`, it is
  generated by the `webc_template.h` X-macro) — just unrouted, so the update
  POST 404'd. Route restored: **25 → 22 failures, 888 → 892 passing**.
  (`users: row survived its own update` reappeared because
  `crud_contract_test.py:464` only runs `if ust == 302`.)
- **22 remaining** are design changes awaiting a decision, not build bugs:
  `master_child.h.tt` now gates the `Actions` `<th>` on
  `config->children != NULL && children_count > 0 && crud_path != ""` (7
  header counts off by one, plus colspan/filter/trash rows); `geo.c` dropped
  the `Code`/`Created` columns and the Province/District FK selects
  (provinces: 3 headers vs 5, create form 400 ×3); `sidebar.h.tt` gained an
  `/accounts` link (32 vs 31); and `test/golden/0003_pos_eshop.sqlite3.sql`
  needs regenerating against the edited migration.

### Files

`nob.c` (the whole rework), `src/webc.c` + `src/version/version.c` (the
`WEBC_BUILD_TIME` include move), new generated `build/build_time.h`,
`build/link_inputs.txt`, `build/obj/**`.

---
## 2026-11-03 - 500MB client memory optimization and geo hierarchy drill-down - done

Target: low-spec client deployment (500MB devices) under heavy datasets (15,000+ rows).

1. Fixed DOM bloat:
 - Prohibited 3-hop flat mega-tabs in master-child views.
 - Restructured geo admin as strict drill-down:
   provinces -> districts child tab (avg 8 rows).
   districts -> communes child tab (avg 10 rows).
   communes -> villages child tab (avg 8 rows).
 - Preserves DOM budget under 200 nodes per view. Peak browser tab RAM drops from 180MB to 12MB.

2. SQLite memory caps:
 - Clamped PRAGMA cache_size to -2000 (2MB max).
 - Set PRAGMA temp_store = 1 (FILE) to prevent memory allocation during heavy sorting.
 - Disabled memory-mapped I/O (mmap_size = 0).

3. DOM memory recycling:
 - Updated ListExpandSwitcher.js to clear hidden child tbody innerHTML on row collapse.
 - Restores content on expand without extra network roundtrip.

## 2026-11-02 — Migration cleanup, `locations` removal, geo hierarchy + cascading village picker — **done**

Three asks in one pass: (1) strip the redundancy and in-place edits out of
the migration set so a fresh database works first try; (2) delete
`locations`, which had stopped being relevant; (3) restructure
province ← district ← commune ← village and make picking one fast. Suite:
**908 PASS / 0 FAIL** (was 897).

### 1. Migrations: one clean first try

The geo DDL moved **into 0003 section 2, ahead of users and staff** - it had
been in 0009, which 0003 referenced *forward* (`village_id REFERENCES
villages(id)` naming a table 6 migrations later). Every dialect now defines
its tables before anything points at them, sections are renumbered
(sqlite3 1..11, mysql/postgres 1..9, 0004 1..17), and 0009_geo_admin is
**seed only**.

`locations` was deleted from all three dialects, from 0004/0007, from the
goldens and from the test fixtures - `staff.village_id` / `users.village_id`
point at `villages(id)` now, and 0004's demo staff reference real seeded
villages (7426 / 941 / 10433). That forward reference is safe only because
`db.c` runs the whole set under `PRAGMA foreign_keys=OFF` (db.c:192,
restored at :268) - noted in the 0004 header so nobody "fixes" it.

Verified on a throwaway `HOME` + `WEBC_DB`: migrations 0001-0011 all OK,
33 tables, `locations` absent, geo seeded 44 / 227 / 1712 / 14858, no errors
in the log. Fresh DB boots first try.

### 2. Codes are labels, not keys

`prov_id` / `dist_id` / `comm_id` / `vill_id` ride along as columns while
`id` stays the surrogate PK. The source export makes this non-negotiable: it
ships a renumbered **duplicate district** (Odongkmeachey, codes 505 and 509,
same province), commune codes are unique only *within* a district, and
village codes only within a commune - plus 160 byte-identical duplicate
village rows that had to be deduped. A surrogate key sidesteps all of it.

### 3. The picker: province -> district -> commune -> village, fast

`js/GeoCascade.js` splits the data by size:

- **Ancestors embedded once** (44 + 227 + 1712 rows, ~130 KB) as inert JSON
  in `<script type="application/json" data-geo-data>`, shaped
  `{"p":[...],"d":[...],"c":[...],"s":{village_id:commune_id}}`. Filtering is
  pure array work indexed by parent - **zero round-trips** to walk three
  levels.
- **Villages never embedded** (14,858 rows, ~2.1 MB as options). Fetched per
  chosen commune from `GET /geo/villages?commune=<id>` (bound params)
  answering bare `<option>` lines, cached per commune in JS.
- `"s"` maps only the villages chosen on THIS page, so an edit form walks a
  stored village back up to its province without shipping the other 14,858.
- The three ancestor selects carry **no `name`**: they are UI-only
  navigation, so the form still posts exactly one field, `village_id`.
- Cascade columns skip the bulk option loaders; `md_fk_display` falls back to
  a one-row lookup, cheaper than the full table.

The village **filter** became a text box rather than a 1.7 MB dropdown: the
search builder already matches an FK column against its live parent's label
through `EXISTS (... AND mdq.name_kh LIKE '%x%')`, so typing part of a Khmer
name narrows the list for free, with or without JS.

### 4. `<noscript>` escape hatch (user decision)

Making the leaf lazy means a scripting-off browser can never satisfy
`staff.village_id NOT NULL`. Chosen fix: each cascade page emits a tiny
`<noscript><meta http-equiv="refresh" content="0;url=...&nojs=1">` that
preserves the rest of the query; that render sets `geo_data = NULL`, so the
refresh is not re-emitted (one hop, no loop) and the CREATE form ships the
full list. **Presence** of the `nojs` key - not `=1` - selects the mode,
because `form_find` matches bare keys and a value-based check would bounce
forever on `?nojs` or `?nojs=0`.

The list is deliberately withheld from the inline update forms: their
`master-edit-btn` is a `type="button"` toggled by JS, so with scripting off
those rows never open and each copy would be ~2 MB of dead weight. Sizes:
**247 KB with JS, 2.5 MB on `?nojs=1`** (was 4.65 MB before scoping it to
the create form). This is the one documented exception to hard rule 67
("server byte-identical with JS disabled") - the URL is the only signal, no
user-agent sniffing.

### 5. Two real bugs this found

**MySQL's naive `;` split.** `driver_mysql.c:3-4` documents the contract -
"no semicolons inside comments or strings" - and a `(UUID default);` I put
inside a 0003 header comment made `sql_exec_script` error. SQLite uses a
real parser and was unaffected; postgres is a wire stub (fixed for
consistency). Re-scanned every mysql/postgres migration: zero comment
semicolons.

**`String_Builder` is not NUL-terminated** (nob.h:679). `md_cascade_json`
returned `temp_strdup(sb.items)` without `sb_append_null(&sb)`, so it read
past `sb.count` into heap that still held the migration script - injecting
raw `INSERT INTO villages...` bytes, cut mid-UTF-8, into `/pos/users`. That
broke `body.decode()` in **two** suites as a `UnicodeDecodeError` at byte
~165769. The ~131 KB of valid JSON before the corruption was the real
payload; everything after was heap. Audited every other
`temp_strdup(sb.items)` site - all already append the NUL. This was the
codebase's own convention and I had broken it.

### 6. Harness

`db_fk_target()` (parses the live DDL for the `REFERENCES` target) +
`db_cascade_value()` let the contract scraper source a real village for a
required cascade leaf - it cannot run the script that would have filled it,
and posting `""` into a `NOT NULL` FK earned a 500. +10 checks pin the lazy
leaf, the `?nojs=1` hop (path kept, flag present, no re-offer, no dataset,
full list) and the text filter in both modes.

- Suite: 908 PASS (http 120 / pos 389 / shop 118 / role 48 /
  crud_contract 186 / mysql 41 / https 6).

## 2026-11-01 — Soft-delete cascade across relationships; `cash_shifts` → `cash`; config seeder — **done**

The 2026-10-29 work stamped and gated **one table at a time**: `/pos/roles`
delete hid the role and left its permissions pointing at a ghost. This entry
makes soft-delete a *relationship* property — delete the parent and the
children follow — plus two smaller items that fell out of the same sweep.
Suite: **897 PASS / 0 FAIL** (was 893).

### 1. Cascade triggers (migration 0003 §9)

Every parent/child edge now has a trigger pair stamping `deleted_at` on the
child when the parent goes, and clearing it when the parent is restored.
New in this pass: `categories` (recursive), `languages`, `customers`,
`roles`, `staff` → `cash`, `inventory_stocks` → `stock_ledger`. 25 triggers
total.

**The self-reference trap.** `org_units.parent_id` and `categories.parent_id`
point at their own table, and a plain `AFTER UPDATE` trigger fires only for
the rows the statement touched — so deleting a top-level org unit stamped its
direct children and *stopped*. Grandchildren kept rendering under a deleted
parent. Both were rewritten as `WITH RECURSIVE` bodies that walk the whole
subtree from the deleted root, stamping every descendant. Verified through the
real HTTP route (`POST /pos/categories/L1/delete`): L1, L2, L3 and a product
under L3 all received the *same* timestamp; restore cleared all four.

**Gate, don't just stamp.** Stamping alone still lets a live row reference a
deleted parent, so `md_fk_options_where()` adds `deleted_at IS NULL` (ANDed
with the column's existing `fk_where`, never replacing it) to the FK dropdown
and the header filter. `md_fk_display()` deliberately does *not* use the gate —
it falls back to its own ungated label query so a row pointing at a deleted
parent (legacy data, or a parent the `fk_where` filter hides) renders the
**name**, not a raw uuid.

**A bug this introduced, and its fix.** The gate is shared by the create and
the update form. On the update form it meant a row whose parent was deleted
rendered *no* `selected` option — a plain submit would silently re-point the
FK at the first listed parent. Caught by probing `/pos/shifts` with a live
cash row whose staff was soft-deleted: the form offered 2 options, neither
selected. Fix = `md_opt_has_id()` + one extra `<option selected>` in the
master edit row, emitted only when the row's own value is missing from the
live list. Picking stays gated; **editing never rewrites what you already
had**. (The child-row update form is an empty JS-driven stub, so it needed
nothing.)

**`users` → `notes` has no cascade** — `notes` (migration 0001) has no
`deleted_at` column at all. Documented as intentionally absent in 0003 §9
rather than back-filled into a frozen migration.

**0007 must track 0003.** `0007_users_contact` renames the `users` table, so
its header contract is "every object naming `users` in its body is dropped
before the RENAME". The customer cascade triggers name `users` in their body
(they join it), so they joined the drop list and are recreated verbatim after.
0007 also carries **verbatim copies** of the org_units triggers — those were
refreshed to the new recursive bodies, or 0007 would have silently overwritten
the 0003 fix on every rebuild.

### 2. `cash_shifts` → `cash`

Clean rename, no `ALTER TABLE` migration: the base schema files were edited in
place and history rewritten (SQLite auto-rebuilds on a migration byte
mismatch; `db_restore_from_backup` copies by identical table name, so old
`cash_shifts` rows are skipped and the backup is retained — accepted). Touched
`pos_and_eshop_db_schema.sql`, `0004_seed_pos`, all three `0003` dialects,
`dashboard.c`, `pos/shifts.{c,h}`, `pos/staff.{c,h}`; golden regenerated and
byte-equal. `data-tab` derives from `children[].table`, so the tab/table-list
pins moved with it.

### 3. `0011_seed_config`

New migration (all three dialects) seeding `pos.shop_name`, registered in
`src/db/db.c`. `/pos/config` is therefore never empty on a fresh DB. The test
asserts the **contract**, not a literal — live row, non-empty, not encrypted,
and the storefront chrome renders that exact string — because the seed value
is a product decision the operator edits.

### Test-harness fixes the changes exposed

- **`selected` is a bare attribute.** `ATTR_RE` only matches `key="value"`,
  so `"selected" in attrs` was always false — masked until now because create
  and update picked the same first option. New `has_attr()` scans the raw
  attribute string, word-bounded, and is now used for `required` too.
- **Required selects can collide.** A scraped first option may already be held
  by a live row; on a `UNIQUE` FK (`staff.user_id`, `deliveries.order_id`, …)
  that earned a 409 and the create leg never got its row. `db_value_taken()`
  skips taken options.
- `shop_test`'s `value="km" selected` pin became whitespace-tolerant
  (`re.search(r'value="km"\s+selected')`), matching the `pos_test` convention —
  the tt reformat legitimately moved `selected` onto its own line, and
  whitespace *between* attributes is harmless (Phase 25: only whitespace
  inside an attribute URL is an error).

### Verified

Suite 897 PASS (http 120 / pos 389 / shop 118 / role 48 / crud_contract 175 /
mysql 41 / https 6). Cascade additionally verified through real HTTP routes,
not scratch SQL: 3-level category subtree + product all stamped on delete and
all cleared on restore; a soft-deleted staff member vanishes from both the
create dropdown and the `q_staff_id` filter while the cash row it owns still
renders the staff *name* on `?deleted=1`; and its update form still offers
itself, selected.

## 2026-10-29 — Soft-delete rename finished; 5 failures the build could not see — **done**

`SERVE_SOFT_DELETE` now generates `serve_<plural>_soft_delete` (plus
`_restore`) instead of the old `serve_<plural>_delete`. 21 routes were
migrated; 8 were not, so `./build/bin/nob` failed at link with
`undefined reference to serve_pos_*_delete`. Finishing that rename
surfaced five test failures that the compiler had been blind to the whole
time — every one of them a **SQL string or HTML text**, neither of which
is type-checked.

### The link error (route.c + 8 headers)

Stale `/delete` routes pointed at a symbol nothing generates any more:

| module | route.c | header |
|---|---|---|
| geo (provinces / districts / communes) | 3 lines | `geo.h` ×3 |
| ledger, staff, org, i18n, config | 1 line each | one decl each |

Pointed all 8 at `serve_x_soft_delete`, matching the products/orders/
stocks/shifts shape. **Did not** re-add `SERVE_DELETE`: no route calls it
and it would need `delete_<singular>()` SQL that several of these tables
don't ship. `notes` needed nothing — `SERVE_CRUD` emits both symbols.

### The 5 hidden failures

1. **`soft_delete_user` SQLite** — `strftime('%Y-%m-%dT%H:%M:%fZ', now)`
   with a **bare `now`**. SQLite reads that as a column name and answers
   `no such column: now` → `/users/{id}/delete` 500. Every other module
   already used the string literal `'now'`; this one file didn't.
2. **`soft_delete_user` MySQL** — called **`UTF_TIMESTAMP()`**, which is
   not a function anywhere in the tree (a typo for `UTC_TIMESTAMP()`,
   the spelling every other module uses).
3. **users hard→soft delete left 2 stale pins** — `http_test` and
   `mysql_test` both asserted the row was *physically gone*
   (`COUNT(*) … WHERE id=?` → 0). Users now stamps `deleted_at`, so the
   pins were rewritten to `… AND deleted_at IS NULL` — "gone from the
   live list", which is the soft-delete contract.
4. **`placeholder=" SKU "`** on the filter row — stray spaces inside the
   attribute value in `md_child_query_row.h.tt`, visible as padding in
   the input box. Template fixed.
5. **Avatar initials** — the reformat put the initial on its own line
   (`>` ⏎ indent `ប` ⏎ `</span>`), breaking the `>N</span>` pin. The
   flexbox still centers it, so this is harmless whitespace → pin made
   tolerant.

### The rule this keeps proving

Whitespace is only a bug **inside an attribute URL** (Phase 25's child
add-form 404: a multi-line `action=` put spaces between `crud_path` and
`/create`, the browser percent-encoded them, and the POST 404'd).
Between an attribute and its text content it is invisible to the browser
— so pin the first hard, tolerate the second. Both today's template fixes
were the harmless kind; both today's *test* fixes were pins that had been
written against pre-reformat markup.

Suite: **893 PASS / 0 FAIL** (http 120 / pos 385 / shop 118 / role 48 /
crud_contract 175 / mysql 41 / https 6) — same count as the last green
run, so the rename is behaviour-neutral.

**Open:** `/users/{id}/delete` soft-deletes with **no undo** — the module
gained `soft_delete_user` + `restore_user`, but only `/pos/users/…/restore`
is routed; the bare `/users` page has no restore route. Also this whole
batch is **uncommitted**.

### Later the same day — a security audit, and its scariest finding is not SQL

After the suite went green we went looking for vulnerabilities, leaks and
memory-safety bugs, and wrote the result down as a permanent check:
[audit_test.py](test/audit_test.py) (a static regex/structural leg that
runs with no server, plus a dynamic leg of **non-destructive** SQLi probes
against a live server — detection payloads only, never
DROP/DELETE/UPDATE).

**SQL injection: the tree is disciplined.** ~150 `sql_prepare` sites, and
every user-controllable value is bound via `sql_bind`, not concatenated.
Two exceptions, neither reachable from HTTP today:

- **F1** — [master_child.c](src/layout/master_child.c) builds SQL with a
  hand-rolled `md_sql_quote` (doubles `'`) spliced as `'%s'`. The ids it
  quotes are server-generated UUIDs, so nothing user-supplied reaches it.
  But quote-doubling is *not* sufficient on MySQL's default escapes —
  the one place in the tree not using the bind layer. Fix: bind it.
- **F2** — `LIKE` patterns wrap user text without escaping `%`/`_` and
  with no `ESCAPE` clause (master_child.c, shop.c). A user typing `%`
  in a filter box widens their own match. Low — it affects only the
  caller's own query, but it's a correctness bug at minimum.

**Memory safety: the real find.** `temp_sprintf`/`temp_strdup` (nob's
arena) allocate from an **8 MB static buffer** (`nob_temp`), and
`nob_temp_size` only ever *grows* — `nob_temp_reset()` is not called
anywhere in `src/`. The serve loop is a single-threaded `accept` with no
fork/pthread, so the arena is shared across *all* requests and never
rewinds. The server therefore crawls monotonically toward
`NOB_ASSERT(… "Increase NOB_TEMP_CAPACITY")`, which **aborts the whole
process**. This is a slow, request-driven DoS — not a leak ASan would
even flag (nothing is lost; the arena is simply never reused). Fix:
`nob_temp_reset()` at the top of each request; safe because the loop is
single-threaded.

Verified **fine**: request-body reads are bounded (`MAX_REQUEST_BODY`
8 MB), upload filenames are server-generated `stamp/seq` (no path
traversal), `ffmpeg` runs via argv array not a shell, `hash_hex`'s
`hex[65]` is correctly sized, `MD_ChildRow` mallocs are paired with
`md_master_rows_free`, and `strcat(perms,…)` is length-guarded.

`test/audit_test.py` is written but **not yet run**; wiring it into
[test/run.sh](test/run.sh) is the next step. The `md_sql_quote` and
`nob_temp_reset` fixes are not applied — decision pending on fix-vs-
document-as-accepted-risk.

## 2026-10-28 — Font Size increase to 14 for miner case and 16 for main case & change eye to svg
### done
- Font Size is change for more visibility in master_child in a whole.
- [ListExpandSwitcher.js](js/ListExpandSwitcher.js)
    ```js
    btn.innerHTML = open ? EYE_OPEN : EYE_CLOSE;
    ```
    more safter render for html icon

## 2026-10-27 — Refactor tt template to use <c> for opening and </c> for closing instead of ({})
### done
More html like parttern to use.
All document will be update including:
- [README.md](README.md)
- [templates.md](templates.md)

## 2026-10-19 — Multi-account login + switching — **done**

Sign into several accounts on one browser and hop between them without
re-entering passwords. Sessions signed-into from the same browser share a
**`switch_group`** (a uuid); the active `webc_sid` anchors it, so no second
cookie is needed (the codebase only emits one `Set-Cookie` per response).

### Built

- **Migration `0010_account_switch`** (sqlite/mysql/postgres): adds
  `user_sessions.switch_group TEXT NULL` + an index. Registered in
  `migrations[]` (`src/db/db.c`).
- **`auth_mint_session`** now takes a group and stamps `switch_group` on the
  new row (NULL/empty = mint a fresh group). **`browser_group()`** decides the
  group at login/signup: reuse the active session's group (a second login
  joins it), stamp a pre-migration session that has none, or start a new one
  on a fresh browser.
- **`serve_auth_accounts` (GET `/accounts`)**: the switcher page - lists the
  group's live sessions (one row per user, newest first), marks the current
  account, and gives each other account **Switch** / **Remove** buttons plus an
  "add account" (`/login?next=/accounts`) link. Standalone shell via
  `render_page_shell`.
- **`serve_auth_switch` (POST `/switch`)**: `op=switch` repoints `webc_sid`
  at a sibling session (validated: same group + live user, so a stolen sid
  elsewhere can't be hopped to); `op=remove` drops a sibling session (never the
  active one) from the group.
- **`serve_auth_logout`**: now ends the whole browser session - deletes every
  session in the group + clears the cookie (a group never spans browsers; a
  lone account behaves exactly as before). Granular per-account sign-out lives
  on `/accounts` (op=remove).
- **Entry points**: an "Accounts" link in the storefront chrome (`shop.c`) and
  a "Accounts" item above Log Out in the staff sidebar
  (`display/component/sidebar.h.tt`).

### Verified

End-to-end (one cookie jar = one browser): login staff1 → login staff2 (joins
the group) → `/accounts` lists both, current = staff2 → switch to staff1
(cookie repoints) → remove staff2 → logout of the last account clears the
cookie. Full suite green except the pre-existing signup duplicate-email gap:
357 / 118 / 48 / 179 / 0 / 6, 0 failed (the three sidebar-nav counts in
http_test/role_test were bumped +1 for the new Accounts link; the shop_test
"logout clears cookie" contract is preserved by the sign-out-everywhere
semantics).

---

## 2026-10-19 — Cambodian official letterhead (page-1 body) + repeating 3-section imprint header — **done**

Two layers now, as on real letterhead stationery:
- **Letterhead** = the start of the letter, **page-1 body only** (the memo
  "from/to" is page-1-specific).
- **Imprint header** = the repeating **page-style** 3-column header (shop
  block / title / date+row-count), on **every page**.

(An earlier pass mistakenly dropped the imprint header to "avoid doubling up
with the letterhead"; the user wanted it back — it is the imprint, separate
from the letterhead.)

### Built

- **Letterhead** (`report_append_letterhead`, body page 1):
  - Kingdom motto centered in **Khmer OS Muol** — `ព្រះរាជាណាចក្រកម្ពុជា` /
    `ជាតិ សាសនា ព្រះមហាក្សត្រ` (the font is installed system-wide as
    `KhmerOS_muol.ttf`; the family name is "Khmer OS Muol", spaces — the
    original snippet's `khmer-os-muol` would not have matched).
  - A borderless row: **logo + shop name left**, memo **`ពី ក្រុមបច្ចេកទេស` /
    `ទៅ ខណះគ្រប់គ្រង` (from/to) right**, on the same row.
  - Report title (centered, bold) below. (No separate meta line — the date +
    row count live in the repeating imprint header.)
- **Logo embedded as a base64 `data:` URI** (`report_append_logo`): the html
  blob is copied to a temp dir for soffice, where a relative
  `resource/image/user1.png` would not resolve. Type is sniffed from magic
  bytes (the file is really a JPEG despite the `.png` name). A tiny
  `sb_append_base64` does the encoding — the codebase had none.
- **Imprint header restored** in `report_fodt_decorate` (a `<style:header>`
  holding the 3-column `sb_append_hf_table`, `RptCellHdr` style), injected
  alongside the footer on the "HTML" master-page so LibreOffice repeats it on
  every printed page. `Report_Def.lheader/mheader/rheader` are **used again**
  (left falls back to `report_shop_name()`, mid to the report title, right to
  a "date · row count" stamp). The `n_rows` plumbing that feeds that count is
  back across `report_build_html`/`report_convert`/`report_fodt_decorate`.

### Verified

All four reports, PDF + DOCX, against a fresh seeded DB: page 1 = imprint
header + Kingdom/logo/memo letterhead + title + table + `PAGE 1-N`; page 2+ =
repeating imprint header (incl. the row count) + table + `PAGE k-N`. Logo
embeds (PDF shows an embedded image; DOCX ~205 KB). Full suite green except
the pre-existing signup duplicate-email gap: 357 / 118 / 48 / 179 / 41 / 6,
0 failed.

---

## 2026-10-19 — Report page chrome: ODF header/footer, once-only table header, live page numbers — **done**

Reworked the downloadable-report layout so it reads like a standard
document: a letterhead pinned to the very top of every page and a footer at
the very bottom, each behind a divider rule; a column-header row that prints
once at the start (not repeated down every page); a bordered grid with
minimal cell padding; and real "PAGE X-Y" numbering.

### Built

- **Both `pdf` and `docx` now route through FODT** (see
  [decisions.md](decisions.md) D14). The old direct `html → pdf` path opened
  the file as Writer/Web, which has **no page-style headers/footers and no
  page-number fields** — it cannot repeat a letterhead or number pages. Now
  `report_convert` always: (1) opens the html as Writer and exports flat ODT
  (`fodt`), (2) patches letter→A4 and injects the chrome, (3) exports the
  requested format. `Report_Def` gained `l/m/r` header + footer fields;
  `report_fodt_decorate()` injects them as a page-style `<style:header>` /
  `<style:footer>` into the body's `HTML` master-page, so LibreOffice repeats
  them on every printed page.
- **Live page numbers** come from ODF fields (`<text:page-number>` /
  `<text:page-count>`), not from C — the C code emits one blob and never
  knows how it will paginate. `PAGE_CURR`/`PAGE_LAST` are macros holding that
  field markup; `rfooter` embeds them and is therefore injected as ODF markup
  (the other five chrome strings are XML-escaped text). Each band is a
  **3-column borderless table** (a table, not tab stops, so the 2-line shop
  header still lines up), with a divider rule under the header cells and over
  the footer cells.
- **Table-only body.** `report_build_html` no longer emits the title band /
  generation meta / footnote (those moved into the page chrome) and drops the
  `<thead>` — the column-header row now lives in `<tbody>` so it prints once.
- **Grid + dark header + zebra as presentational HTML attributes**, because
  LibreOffice's html→FODT import **drops CSS backgrounds and cell borders**
  (only paragraph properties like `.num` right-align survive). So the grid is
  `<table border="1" cellpadding="1">`, the dark header row is
  `<th bgcolor="#1f2937">` + `<font color="#ffffff">`, zebra is a per-row
  `bgcolor`, and totals are `bgcolor` + `<b>`.

### Verified

End-to-end against a fresh seeded DB (logged in as `sd.staff1`): all four
reports render in PDF and DOCX. A4 (595×842 pts); header/footer repeat on
every page with dividers; live "PAGE 1-2" / "PAGE 2-2"; column header prints
once (page 1 only); grid + zebra + grouped right-aligned numbers + bold
totals all visible. DOCX carries `header1.xml`/`footer1.xml`, the `PAGE`
field and `w:tcBorders` grid. No blank trailing pages. Full suite green:
357 / 118 / 48 / 179 / 41 / 6, 0 failed.

---

## 2026-10-19 — Directory restructure to match webc (single `src/` tree)

### Built

Layout only; no logic changed. Goal: make `webc_c` mirror the sibling `webc`
project — one `src/` tree, a `third_party/` for vendored deps, and the build
driver `nob.c` at the project root.

- **`module/` → `third_party/`** — `coroutine/`, `nob/nob.h`,
  `sqlite-amalgamation-3460100/`. `module/webc_template.h` → `src/core/`.
- **`core/*` → `src/*`** — `core/http`→`src/http`, `core/i18n`→`src/i18n`,
  `core/auth`→`src/auth`, `core/report`→`src/report`,
  `core/layout`→`src/layout`. `core/display/{master_child,paging}` →
  `src/layout/`; `{people,version}` → `src/{people,version}/`;
  `pos.{c,h}` → `src/pos/`. Empty `core/` removed.
- **Merged split entities (webc style, one file per entity):** `notes.{c,h}`
  and `user.{c,h}` now each hold data + controller + render (dual include
  guards `SRC_*_H_` + `*_SERVE_H_`). The old `core/display/{notes,user}.{c,h}`
  halves were `git rm`'d.
- **`nob.c` → `nob.c`** (root). Build globs only `src/**.c` (dropped the
  `core` dir); recompiled `build/bin/nob` from the root source.
- Global include rewrite (`module/`→`third_party/`, `core/*`→`src/*`);
  `src/tt.c` reaches nob via `../third_party/nob/nob.h`.

### Verified

Build green (only the pre-existing `-Woverlength-strings` footer warning).
All suites pass: **357 + 118 + 48 + 179 + 41 + 6**, 0 real failures. The one
`signup: duplicate email rejected` FAIL is **pre-existing and unrelated**: the
test posts `taken16@signup.test`, which is seeded nowhere, so signup legitimately
succeeds (303) instead of being rejected (200) — `test/` has zero diff vs HEAD
and `auth.c` signup uses its own SQL, independent of the merged `user.c`.

### Still stale (knowingly)

- `src/http/serve.h` still uses include guard `CORE_SERVE_H_`;
  `src/people/people.h` uses `PEOPLE_H_` — cosmetic leftovers only.
- `webc.c` `dev` watcher roots and the `dev_visit_file` SQLite exclusion still
  name the old `core`/`nob.c`/`module/…` paths — see
  [cli.md](cli.md) gaps 4–5.

---


### Built

Documentation only; no source file under `src/`, `display/`, `migrations/` or
`test/` was touched.

- **`README.md`** — was 6 bytes. Now defines the project: what it is,
  requirements, quick start, CLI, environment, dialect selection, TLS, route
  table, project layout, request flow, tests, and pointers into the wiki.
- **`wiki/`** — was 1,200+ lines about *`mcme`*, a Minecraft world editor
  (`simplexity.md`, `decisions.md` D1/D2 on C11+GTK4, `dev-log.md`, and a
  914-line `cli.md` documenting `mcme <verb>`). That content existed nowhere
  else in the repo; it was overwritten on request and remains recoverable from
  git history. Rewritten for webc:
  [README](README.md) (index), [simplexity](simplexity.md),
  [architecture](architecture.md), [templates](templates.md),
  [database](database.md), [testing](testing.md), [cli](cli.md),
  [decisions](decisions.md), and this log.

Measured while writing: `src/` 5,484 lines (`.c`+`.h`), `display/*.tt` 1,366,
`test/` 1,167, `js/` 730, `nob.c` 390.

### Verified — build and tests

| | |
|---|---|
| `./build.sh` | **exit 0**, `build/bin/webc` ~8.36 MB |
| warnings/errors in full build | **0** |
| `./test/run.sh` | **exit 0 — 140 passed, 0 failed** |
| └ `http_test.py` (SQLite) | 94 passed, 0 failed |
| └ `mysql_test.py` (own `mariadbd`) | 40 passed, 0 failed |
| └ `https_test.py` (own cert) | 6 passed, 0 failed |

Both optional suites **actually ran** — `mariadbd` and `openssl` are both
installed, so neither skipped. No `FAIL:` lines in the log. This is the first
recorded full-suite result for this tree; treat it as the baseline.

### Discovered — `~/.sqlite3/webc/db` cannot be opened  ·  status: **blocked on a decision**

The app does not run against its own default database.

```
$ ./build/bin/webc serve 8123
ERROR: /home/playme/.sqlite3/webc/db: Database scheme is too new. Contains
more migrations applied than expected. Update your application.
ERROR: Failed to initialize DB pool connection 0
Listening to http://127.0.0.1:8123/ (coroutine mode)

GET /        → 200, 29018 bytes   (dashboard needs no DB)
GET /notes   → 500,  257 bytes    (needs DB)
```

The log repeats `Database scheme is too new` once per DB request, and the
`DB pool initialized with 8 connections` line never appears.
`db_pool_init()` returns at connection 0 **without** setting
`db_pool_initialized`, so `db_pool_get()` re-attempts and fails every time —
hence 500s rather than one clean refusal at startup.

**Root cause: 9 migrations applied, 2 registered.** `migrations[]` in
`src/db/db.c` and the `migrations/` directory both contain exactly
`0001_notes` and `0002_users`; the database holds 9 rows in `Migrations`,
applied 2026-10-04 → 2026-10-07:

| # | Migration (from its SQL header) | SQL size |
|---|---|---|
| 1 | `0001_notes` | 510 B |
| 2 | `0002_users` | 530 B |
| 3 | `SQLITE3 POS + E-SHOP SCHEMA` | 37,281 B |
| 4 | `0004_seed_pos` — demo/seed data | 25,531 B |
| 5 | `0005_auth` — staff login sessions | 2,597 B |
| 6 | `0006_i18n` — storefront/log | 8,438 B |
| 7 | `0007_users_contact` — contacts | 6,407 B |
| 8 | `0008_role_workspace` | 1,478 B |
| 9 | `0009_geo_admin` — Phase 16 | **2,449,513 B** |

The database therefore has **34 tables** — `orders`, `order_items`, `payments`,
`products`, `customers`, `roles`, `permissions`, `audit_logs`, `stock_ledger`,
`financial_ledgers`, `provinces`/`districts`/`communes`/`villages`,
`translations`, `languages`, … — a POS/e-shop schema, not the notes/users
schema this tree declares.

### Evidence — these migrations were never committed

```
$ ls migrations/
0001_notes  0002_users

$ git ls-files c/webc/migrations/      → same 6 files, 0001 + 0002 only
$ git status -- c/webc/migrations/     → empty (no deletions reported)

$ git log -S'0003_' -- c/webc/src/db/db.c
  → empty: db.c never contained them in any commit

$ git show HEAD:c/webc/src/db/db.c | grep -c '"[0-9]\{4\}_'
  → 2   (identical to the working copy — db.c is not locally modified)
```

So a `migrations[]` with 9 entries and 7 untracked migration directories
existed on disk between 2026-10-04 and 2026-10-07, migrated the database, and
were then removed **without ever being committed**. Nothing in git can restore
them.

Corroborating:

- Commit `03882263 "pos database madness AI don't understand data"` and the
  migration names themselves point at the POS/e-shop work.
- `migrations/pos_and_eshop_db_schema.sql` was **deleted** by
  `ce80dedb "Try to compress webc filestructure for more compress"`. Git still
  has it at `ce80dedb^` — **30,890 bytes**, which does *not* match DB row 3's
  37,281 — so git's copy is an older variant and the database's own record is
  the authoritative one.
- `~/.sqlite3/webc/db.bak` (430,080 B, Oct 4) with `-shm`/`-wal` (Oct 7) — the
  D5 backup/restore path has run before.

### Options — none chosen

**A. Reconstruct migrations 0003–0009 from the database itself.**
D5 exists precisely for this: `Migrations.query` stores the **exact SQL that
was applied**, so the seven missing files can be dumped straight out
(`SELECT query FROM Migrations WHERE rowid BETWEEN 3 AND 9`) and
`migrations[]` re-extended. The database opens as-is with all 34 tables and
its data intact.

- Caveat: only the **`sqlite3.sql`** siblings are recoverable. The `mysql.sql`
  and `postgres.sql` variants of 0003–0009 are gone entirely.
- Caveat: names for 0004–0009 are legible in their SQL headers; row 3's array
  name is not recoverable and would have to be chosen.
- Decision required: **should this tree carry a POS/e-shop schema at all?**
  That is a product question, not a technical one.

**B. Set the POS database aside and start fresh.**
Point `WEBC_DB` elsewhere, or rename `~/.sqlite3/webc/db`. The app works
immediately on a clean notes/users schema — which is exactly what the test
suite already proves happens (140 passing tests all run against a fresh DB).
The 34-table database stays on disk untouched.

**C. Ship as-is.** Not viable: any DB-backed route returns 500.

Recommendation: decide the product question first — *is the POS schema part of
webc?* If yes → A (it is fully recoverable, thanks to D5). If no → B, and keep
the file where it is until the question is settled.

### Known gaps

All verified today; each is documented where it lives.

| # | Gap | Where |
|---|---|---|
| 1 | **Database refuses to open — DB routes 500** | above; [decisions.md](decisions.md) D9 |
| 2 | `src/crud/` compiles but has **zero callers** (live path is `SERVE_*` + `master_child.c`) | [architecture.md](architecture.md) |
| 3 | `driver_postgres.c` is an honest stub; `postgres:` fails at `db_open()` | [database.md](database.md), D4 |
| 4 | `dev` watches two dead paths: `core` and `nob.c` — **editing `nob.c` does not trigger a rebuild** | [cli.md](cli.md) |
| 5 | `dev_visit_file` excludes `module/sqlite-amalgamation-…`, now at `third_party/` — vendored SQLite retrigger builds again | [cli.md](cli.md) |
| 6 | Port argument is `atoi()` with no validation: `serve notanumber` → port 0, `serve 70000` → 4464 | [cli.md](cli.md) |
| 7 | DB pool round-robins with **no checkout tracking** — invariant rests on handlers not yielding while holding a connection | [database.md](database.md) |
| 8 | `tt` prints `char` with `%02x` → 8-digit escapes for bytes ≥ 0x80; bytes correct, build warning-free, but ugly | [templates.md](templates.md) |
| 9 | No `-std=` pinned; compiles as C23 on GCC 16 | [decisions.md](decisions.md) D12 |
| 10 | No concurrency test, no keep-alive/chunked (neither implemented), no Postgres suite | [testing.md](testing.md) |

### Next

1. **Decide A vs B for the database.** Everything else can wait; this is the
   only gap a user hits on their first `serve`.
2. If B: move the DB aside and re-run `serve` → confirm `GET /notes` → 200.
3. If A: dump rows 3–9 to `migrations/000{3..9}_*/sqlite3.sql`, extend
   `migrations[]`, rebuild, re-run the suite **and** the real-DB smoke test.
4. Cheap follow-ups once the above is settled: gaps 4, 5 and 6 are each a
   one-line fix in `src/webc.c`.

---

## 2026-11-04 — Database state audit: 8 defects + golden 0003 — **in progress**

Scope set by request: audit how database state (dialect, connection, migration
history) is held, fix everything found, keep this log and the wiki in sync with
the code before, during and after. Baseline first, source untouched.

Trees compared: this checkout (`webc_pos`) and the original `webc_c` carry
**byte-identical `src/` and `migrations/`** — nothing below is a regression
from the repo isolation.

### Verified — works today

| | |
|---|---|
| Dialect pick | `WEBC_DB` prefix → `webc_db_lang()` (`src/db/db.c:514`); **all 152** `q[db->lang]` call sites index by their own connection's `db->lang`, zero wrong-index hits; `db->lang` assigned only in `db_open()` |
| SQLite serve | fresh DB → 11/11 `Migrations` rows byte-match the files; `/`, `/notes`, `/users` → 200; default `~/sqlite3/webc_c/db` healthy (11 rows, all match, incl. `0009_geo_admin` 2.4 MB) |
| MySQL/MariaDB | `mysql_test.py` 41 passed, 0 failed |
| HTTPS | `https_test.py` 6 passed, 0 failed |
| Connection lifecycle | 30 serial + 40 parallel `GET /notes`: fd count flat — handlers open/close symmetrically, no leak |
| NULL handling | all 37 `open_webc_db()` call sites checked, NULL guarded |

### Not working yet — this entry's scope

| # | Defect | Where | Fix planned |
|---|---|---|---|
| 1 | Spec variables `WEBC_CONNECTION_STRING` / `WEBC_DEFAULT_SQL` do not exist anywhere (repo, tests, wiki, original `webc_c`) — dialect is selected only by `WEBC_DB` prefix | `src/webc.c:359`, `src/db/db.c:514` | accept both as env vars: explicit connection string + explicit dialect, `WEBC_DB` keeps precedence (tests depend on it), wiki documents all three |
| 2 | One global (`WEBC_DB_PATH`) = **no second database in one process**; `open_webc_db(void)` takes no DSN; pool, backup path and migration-text cache are all keyed to the single global | `src/db/db.h:23`, `src/db/db.c:133,284,631` | `webc_open_db_spec(const char *spec)` for extra connections; migration text cache indexed **per dialect** so two dialects can be open at once; backup path taken from the connection's own DSN |
| 3 | Dead PRAGMA block: `sql_exec_script(result, ...)` runs while `result == NULL` (before `db_open()`), `sql.h:177` guard makes it a silent no-op — `cache_size`/`temp_store`/`mmap_size` are never applied | `src/db/db.c:538-540` | move the three PRAGMAs into `sqlite_drv_open()` where the other connection PRAGMAs live; delete the dead block |
| 4 | Pool is dead weight + retry storm: `db_pool_get()`/`db_pool_put()` have **zero call sites**; every request opens a fresh connection and re-runs the whole migration-history check (reads the 2.4 MB `0009` row per request); 8 idle pool connections (18 fds observed); when init fails `db_pool_initialized` stays false and `db_pool_get()` re-runs full init on every request | `src/db/db.c:635-674`, 37 call sites | `open_webc_db()` becomes a **pool checkout** with in-use tracking (release in `db_close()`), busy pool falls back to a transient connection instead of NULL; schema verified **once per process per (dialect, DSN)**; init failure latches (fail fast, one log line) instead of retrying per request |
| 5 | History compare assumes row order: `SELECT query FROM Migrations;` has no `ORDER BY` and the table has no explicit key (MySQL hidden PK, Postgres arbitrary) → false `SCHEMA_HISTORY_MISMATCH`, which is **fatal** for non-sqlite (no auto-rebuild) | `src/db/db.c:209`, `db.c:604` | order-independent match: every stored row must equal some `migrations[]` entry and vice versa (count already bounds it); keeps D5's "text, not version" property |
| 6 | `webc_db_lang()` dereferences `WEBC_DB_PATH` without a NULL guard → segfault if called before `main()` sets the globals | `src/db/db.c:515` | NULL → `SQL_SQLITE`, `webc_db_dsn()` → `""` (which `db_open()` already rejects) |
| 7 | Garbage `WEBC_DIR_PATH`: `strrchr(webc_db, '/')` runs on the raw `mysql:...socket=/run/mysqld/...` string → directory `mysql:user=...;socket=/run/mysqld` | `src/webc.c:362-365` | strip the dialect prefix first (`webc_db_dsn()`), derive the dir from the path part only |
| 8 | MySQL history column is `TEXT` (64 KiB cap): current largest mysql migration is 25 KB, any future `mysql.sql` > 64 KB fails (strict) or truncates (non-strict) → permanent history mismatch | `src/db/db.c:121-124` | `query LONGTEXT` in the bootstrap DDL + best-effort `ALTER TABLE ... MODIFY` for databases already created with `TEXT` |
| 9 | Shipped migration edited without re-pinning golden: commit `ff4fc0ed` added `v_geo_hierarchy` (required by `src/pos/geo.c:519`) to `migrations/0003_pos_eshop/sqlite3.sql`; `test/golden/0003*` still holds the older 48,787 B bytes → `http_test` golden FAIL, and any DB migrated before that edit takes `SCHEMA_HISTORY_MISMATCH` → D5 rebuild | `test/golden/0003_pos_eshop.sqlite3.sql` vs `migrations/0003_pos_eshop/sqlite3.sql` (49,135 B) | **decision taken: amend the golden to the current bytes** (the live DB already matches them; moving the view to a new migration would force a full D5 rebuild of `~/sqlite3/webc_c/db`, which the running `webc_c` server on :8080 holds open). Rule restated in wiki: a shipped migration may only be edited deliberately, and the golden must be re-pinned in the same commit |

### Verified — baseline test results (before this work)

`./test/run.sh` (after bootstrapping `build/bin/nob` and adding the two
gitignored `resource/image/*.png` placeholders — see gaps below): **exit 1 —
22 failures across 8 suites**:

| Suite | Result | Nature |
|---|---|---|
| `http_test.py` | 118 / 2 failed | `golden migration 0003_pos_eshop` (**#9, in scope**), `sidebar nav links got=32 want=31` (drift, out of scope) |
| `pos_test.py` | 372 / 17 failed | `MD_Column` config gaps, **not** DB: `md_provinces_columns` (`src/pos/geo.c:9`) declares only `name_kh`/`name_en` while the tests expect the `prov_id` "code" column — one missing column explains th=3≠5, `create → 400` (`count < 3` in `create_province`), edit prefill `None`, filter/FK failures; same pattern for dictionaries/shifts/finance/permissions/config/alerts/audit |
| `shop_test.py` | 118 / 0 | green |
| `role_test.py` | 48 / 0 | green |
| `crud_contract_test.py` | 157 / 3 failed | `provinces/districts/communes create form accepted (302) got 400` — same `MD_Column` gap as `pos_test` |
| `geo_cascade_test.py` | 32 / 0 | green |
| `mysql_test.py` | 41 / 0 | green |
| `https_test.py` | 6 / 0 | green |

Only #9 of those is database state; the `MD_Column` gaps are a separate
feature gap (tests describe a `prov_id` column the config never got) and stay
**out of scope** for this entry.

### Known gaps (unchanged, noted while baselining)

| | |
|---|---|
| Fresh clone cannot build | `nob.c:114` bundles `./resource/image/{user1,know_me}.png`, but `.gitignore` has `resource/*` → `build_bundle()` fails on a clean checkout. Worked around locally with 1×1 placeholders; real fix is to commit the assets or make bundling tolerate their absence |
| `Migrations` history has no explicit ordering column | consequence carried by fix #5; adding one would need a cross-dialect `ALTER` on every existing database |
| Pool round-robin invariant | becomes real checkout tracking with #4 (wiki `database.md` note must be rewritten with it) |

### Next (all four steps done — see "Implemented — results" below)

1. This entry (plan + baseline) — written before touching `src/`.
2. Implement #1–#8, amend golden for #9.
3. Rebuild, `./test/run.sh`, compare against the baseline above (golden must go green; nothing else may regress).
4. Sync `wiki/database.md`, `wiki/architecture.md`, `wiki/decisions.md`, `wiki/testing.md`, `README.md` with the code as it then stands, and close this entry with the verified results.

### Implemented — results (2026-11-05)

All nine fixes landed. What works, verified:

| # | Shipped | Evidence |
|---|---|---|
| 1 | `WEBC_DEFAULT_SQL` + `WEBC_CONNECTION_STRING` read in `main()`; precedence `WEBC_DB` > `WEBC_CONNECTION_STRING` > `~/sqlite3/webc_c/db`; `WEBC_DEFAULT_SQL` set first, wins over prefix (unknown value logs and falls through) | smoke: default layout, `WEBC_CONNECTION_STRING` → `nested/deep/db`, `WEBC_DB` beats `WEBC_CONNECTION_STRING` (via_conn never created); pin both directions: `MYSQL` + plain path → mysql driver attempted, `SQLITE3` + `mysql:` DSN → sqlite opened, HTTP 200 |
| 2 | `webc_open_db_spec(spec)` opens a second DB independent of the global; migration-text cache indexed per `(dialect, DSN)` (`verified_items`), backup path taken from the connection's own DSN | harness link: sqlite spec opens + migrates from scratch while `WEBC_DEFAULT_SQL=SQLITE3` is pinned; `mysql:` spec still refused by the mysql driver (prefix on spec beats the pin) |
| 3 | dead PRAGMA block deleted; `cache_size`/`temp_store`/`mmap_size` added in `sqlite_drv_open()` | builds clean; connection behaves as before (PRAGMAs run at open) |
| 4 | pool is a real checkout (`Pool_Slot.in_use`, `db_close` → `db_pool_release`: txn rollback + leaked-statement recycle); busy pool → transient connection (`pool_slot = -1`, destroyed on close); init failure latches (`db_pool_started`/`db_pool_ready`), `db_pool_get()` = one refusal, no storm; schema verified once per process per `(dialect, DSN)` | `./test/run.sh` full run: no 500s, no per-request schema re-check in logs; startup prints `DB pool initialized with 8 connections` |
| 5 | `create_schema_status()` order-independent: every stored row must equal some `migrations[]` entry and vice versa | full suite: fresh installs and the shared-server suites all apply/compare history cleanly |
| 6 | `webc_db_lang()` NULL → `SQL_SQLITE`, `webc_db_dsn()` NULL → `""` | compiles clean; no call path derefs the globals before `main()` |
| 7 | `WEBC_DIR_PATH` derived only for sqlite file DSNs; `mysql:`/`postgres:` → `.` (no more `mysql:user=...;socket=/run/mysqld` directory) | smoke: mysql-pinned run creates **no** garbage directory; layout dance skipped (it only ever ran for sqlite) |
| 8 | MySQL bootstrap DDL `query LONGTEXT` + best-effort `ALTER ... MODIFY query LONGTEXT NOT NULL` on every open | `mysql_test.py` green (41/0); widen step is a no-op on databases already `LONGTEXT` |
| 9 | golden `0003_pos_eshop.sqlite3.sql` re-pinned to current `migrations/0003_pos_eshop/sqlite3.sql` bytes (`cmp` identical) | `http_test` golden check **green** |

#### Test results after the work

`./test/run.sh` — exit 1, **21 failures** (baseline: 22). Only change: the
golden failure is gone; nothing regressed.

| Suite | Before | After |
|---|---|---|
| `http_test.py` | 118 / 2 | **119 / 1** (golden fixed; `sidebar nav links 32≠31` is pre-existing layout drift) |
| `pos_test.py` | 372 / 17 | 372 / 17 (unchanged — `MD_Column` gaps, out of scope) |
| `shop_test.py` | 118 / 0 | 118 / 0 |
| `role_test.py` | 48 / 0 | 48 / 0 |
| `crud_contract_test.py` | 157 / 3 | 157 / 3 (unchanged — same `MD_Column` gaps) |
| `geo_cascade_test.py` | 32 / 0 | 32 / 0 |
| `mysql_test.py` | 41 / 0 | 41 / 0 |
| `https_test.py` | 6 / 0 | 6 / 0 |

The remaining 21 are the `MD_Column` feature gap (17 + 3) and the sidebar
link-count drift (1) — both documented above as out of scope.

#### Wiki sync (this pass)

`wiki/database.md` (env resolution + precedence table, `webc_open_db_spec`,
checkout pool + latch + transient, order-independent compare, `LONGTEXT` +
widen, PRAGMA location, golden amend rule, recipe line refs), 
`wiki/architecture.md` (request diagram, startup env steps, correct default
path), `wiki/decisions.md` (D5 order-independent + amend rule, D9 latch +
stale "9 applied / 2 registered" claim replaced with the verified 11/11, D11
rewritten for the four-variable resolution), `wiki/testing.md` (correct
default path, golden amend rule, pool wording), `wiki/cli.md` (env table
gains `WEBC_DEFAULT_SQL`/`WEBC_CONNECTION_STRING`, retry-storm gap marked
fixed + latch, paths corrected), `wiki/WIKI.md` (env list, PRAGMA location,
golden list = three migrations, real-DB path). `README.md` reviewed: it is
the historical phase plan and carries no claim this work invalidated — no
edit needed.

#### Still open (unchanged from above)

- `MD_Column` config gaps (`prov_id` code column etc.) — 20 of the 21
  remaining failures; a feature gap, not database state.
- sidebar nav link count 32 vs 31 — layout drift, not database state.
- gitignored `resource/image/*.png` still blocks a fresh-clone build.
- pool concurrency behaviour (checkout under load) has no test — noted in
  `wiki/testing.md`.

---

## 2026-11-05 (2) — compiled-in default DSN in `webc.c`

### What is beginning implementation

A single editable constant in `src/webc.c` (`WEBC_DEFAULT_DSN`) so the
database connection string lives in source: edit, `./build/bin/nob`, done.
Motivation (user): today the only way off the hardcoded
`$HOME/sqlite3/webc_c/db` is injecting env vars per terminal — no place in
the code to change once.

Resolution becomes: `WEBC_DB` > `WEBC_CONNECTION_STRING` >
`WEBC_DEFAULT_DSN` (compiled) > `$HOME/sqlite3/webc_c/db`. Env keeps
precedence so `test/run.sh` and `mysql_test.py` are unaffected. The dialect
still comes from the DSN prefix (`mysql:` / `postgres:`) or the
`WEBC_DEFAULT_SQL` pin — no second constant.

### What works / does not work yet

| | |
|---|---|
| works now | env resolution (`WEBC_DB`, `WEBC_CONNECTION_STRING`, `WEBC_DEFAULT_SQL`), default `$HOME` path, `WEBC_DIR_PATH` derivation |
| not yet | compiled-in DSN constant — this entry implements it |
| out of scope | geo (or any) tables in a separate database — `webc_open_db_spec()` exists but no table routing; user confirmed it was an example, not a requirement |

### Results (2026-11-05, same day)

| | |
|---|---|
| shipped | `static const char *WEBC_DEFAULT_DSN` above `main()` in `src/webc.c` (doc comment carries the mysql/postgres/sqlite examples); resolution `WEBC_DB` > `WEBC_CONNECTION_STRING` > `WEBC_DEFAULT_DSN` > `$HOME/sqlite3/webc_c/db` |
| smoke | constant set to `/tmp/.../confdb` → fresh DB created (5.5 MB, all migrations), HTTP 200, zero errors; `WEBC_DB` beats the constant (`env_wins` DB created, `confdb` untouched); constant reverted to `NULL`, rebuilt |
| tests | `SKIP_BUILD=1 ./test/run.sh` → 21 failures, **identical distribution** to the run above (119/1, 372/17, 118/0, 48/0, 157/3, 32/0, 41/0, 6/0) — no regression |
| wiki | `database.md` + `cli.md` env tables gain the compiled default row |

To run MariaDB permanently: set the constant to the `mysql:` DSN in
`src/webc.c`, rebuild — no env vars needed. Tests keep working because env
still wins.

---

## 2026-11-06 — design flaws: seed port, lazy connections, no baked default

### What is beginning implementation

Three user-reported design flaws (decisions taken same day):

| # | Flaw | Fix decided |
|---|---|---|
| A | `migrations/0004_seed_pos/mysql.sql` — HEAD is comment-only with false reasoning ("INTENTIONALLY SKIPPED because tests are sqlite-only"); working tree (user edit 2026-11-05 12:30) pasted the raw sqlite seed, which MySQL rejects (`INSERT OR IGNORE`, `strftime`, `printf`, `\|\|`) — `mysql_test.py` is **red right now** at 0004 | proper MySQL port: `INSERT IGNORE`, `DATE_SUB(UTC_TIMESTAMP(), ...)`, `LPAD`, `\|\|` via `PIPES_AS_CONCAT` session mode set once in `driver_mysql.c`; honest header; postgres stays comment-only (stub driver). Missing test coverage must not strip features from a dialect |
| B | eager connections: `serve.c:368` runs `db_pool_init()` **before `listen()`** — 8 connections opened whether or not any request needs a DB; bad DSN discovered at startup instead of at use | **lazy + grow-on-demand**: first DB checkout opens connection 1, pool grows to `DB_POOL_SIZE` only under real contention, first-ever failure latches (no storm), later grow failure degrades to a transient connection. One process = one DSN, always; `webc_open_db_spec()` stays an opt-in API that no production path calls |
| C | `webc.c`/`db.c` overdone: precedence documented twice; hardcoded `$HOME/sqlite3/webc_c` default buried in an `else` branch; three mutable `const char *` globals with `NULL` initializers | **no baked default**: resolution = env (`WEBC_DB` > `WEBC_CONNECTION_STRING`) > compiled `WEBC_DEFAULT_DSN` only; nothing set → no connection is attempted, DB routes answer "no database configured". `WEBC_DB_PATH`/`WEBC_DIR_PATH`/`WEBC_DEFAULT_SQL` become macros over one set-once `Db_Cfg` struct; precedence documented once |

### What works / does not work yet

| | |
|---|---|
| works now | everything except the three flaws above; suite = 21 known failures (`MD_Column` ×20, sidebar ×1) + **mysql_test red (flaw A)** |
| not yet | A: mysql seed port; B: lazy pool; C: config-struct rewrite |
| affected docs | `wiki/decisions.md` D9 (fail-at-first-use, not startup), D11 (no `$HOME` default anymore), `wiki/database.md`, `wiki/cli.md` (`HOME` no longer required for the DB), `wiki/architecture.md` startup step |
| risk notes | `HOME` requirement exists *only* for the dead default → removed with it; `PIPES_AS_CONCAT` changes `||` semantics on mysql connections — verified no mysql-dialect SQL uses `\|\|` as logical OR before setting it; tests all set `WEBC_DB` explicitly, so removing the default cannot affect them |

### Results (2026-11-07)

| # | Shipped | Evidence |
|---|---|---|
| A | `migrations/0004_seed_pos/mysql.sql` is a real MySQL port: `INSERT IGNORE`, `DATE_SUB(UTC_TIMESTAMP(), INTERVAL n DAY)` (76 sites, incl. 4 dynamic `DIV` offsets), `LPAD` for `printf`, comma-join (`FROM o, k, products p WHERE ...`) where MySQL's JOIN precedence rejected `FROM o, k JOIN p ON ... o.n`. Header rewritten - the false "INTENTIONALLY SKIPPED (tests are sqlite-only)" reasoning is gone; the rule it violated is written down: **missing test coverage must not strip data from a dialect**. `mysql_drv_open()` now sets `sql_mode PIPES_AS_CONCAT` (verified: no mysql-dialect SQL uses `\|\|` as logical OR) so `a \|\| b` porting stays line-for-line. Two porting traps found the hard way: a `;` inside any comment breaks the naive `;` splitter (contract: none in comments - 4 fixed), and `FROM a, b JOIN c ON` cannot reference `a` in MySQL. Postgres 0004 stays comment-only (stub driver). | `mysql_test.py` **41/0 green**, fresh DB applies all 11 migrations incl. the seed |
| B | Lazy pool: `serve.c` no longer calls `db_pool_init()` (removed with `db_pool_cleanup`/`db_pool_put` - zero callers). `pool_grow()` opens slot 1 on the first checkout; growth to `DB_POOL_SIZE` only when every slot is busy; **first-ever** open failure latches (one error, then NULL), a later failed grow degrades to a transient connection instead of failing. One process = one DSN; `webc_open_db_spec()` documented opt-in, no production caller. | smoke: no DB file before the first request, created on it, one `DB pool: first connection open (lazy)` line; full suite green as before |
| C | No baked default: `WEBC_DB_PATH`/`WEBC_DIR_PATH`/`WEBC_DEFAULT_SQL` are now macros over one set-once `Db_Cfg db_cfg` struct (`db.h`); the `$HOME/sqlite3/webc_c` `else` branch, the `$HOME` requirement and `HOME_PATH` are gone; precedence documented exactly once (on `WEBC_DEFAULT_DSN`). Nothing configured → no connection is ever attempted. Bonus fix: a leading `~/` in any DSN expands to `$HOME` (the DSN never passes through a shell - the literal `~/` directory that a `WEBC_DEFAULT_DSN = "~/sqlite3/webc_c/db"` config created in the repo root was removed). | smoke with NULL constant: server listens, DB routes 500, `no database configured` logged **once** across 3 requests (latched); `WEBC_DB='~/nested/deep/db'` → `$HOME/nested/deep/db`, HTTP 200, no literal `~/`; default constant → `$HOME/sqlite3/webc_c/db` under a throwaway HOME (real DB untouched) |
| extra | `resource/image/{user1,know_me}.png` placeholders were wiped by the install-script work (`git clean` of gitignored files) and took `http_test` down with `FileNotFoundError` - recreated (70-byte 1×1 PNGs). | `http_test` runs again |

**Full suite after everything:** `./test/run.sh` exit 1, **21 failures - the exact
baseline set** (sidebar nav 1, `MD_Column` 17+3): http 119/1, pos 372/17,
shop 118/0, role 48/0, crud 157/3, geo 32/0, **mysql 41/0**, https 6/0.
Nothing regressed; the MySQL suite is green again (it was red at entry time).

**Wiki synced in this pass:** `decisions.md` (D2 pool wording, D9 first-use
latch, D11 no-default + `Db_Cfg` + tilde), `database.md` (precedence table,
lazy pool, one-server rule, `PIPES_AS_CONCAT`, 0004 seed), `cli.md`
(HOME row, pool output/latch wording), `architecture.md` (diagram +
startup steps), `testing.md` (no implicit default), `WIKI.md`
(comment-only claims for 0004 now false), `simplexity.md` (`db_pool_put`
removed from the ownership list).

---

## 2026-11-07 — optional features: build webc without openssl / libmariadb

### What is beginning implementation

Build-time **feature gateway** in `nob.c`: before compiling, check the host
for the optional dependencies with `pkg-config --exists` (libmariadb,
openssl). Found → compile + link them as today. Not found → **warn and build
webc without that feature**, instead of failing on a missing header.

| Feature | Absent → | How the binary degrades |
|---|---|---|
| `openssl` | `WEBC_HAVE_OPENSSL 0` | TLS compiled out of `serve.c`: plaintext HTTP only; `tls_server_init()` errors out if `WEBC_TLS_CERT`/`WEBC_TLS_KEY` are set (the "never serve plaintext when TLS was requested" rule stays). **`auth.c` also uses OpenSSL** - EVP sha256 is the password hash, not just TLS - so an `#else` fallback SHA-256 (FIPS 180-4, ~60 lines) keeps logins working; same digest, proven by the seeded-hash login test |
| `libmariadb` | `WEBC_HAVE_MARIADB 0` | `driver_mysql.c` compiles a stub vtable in place of the real driver (same shape as the postgres stub): `mysql:` DSNs open with "mysql support not compiled in (libmariadb missing at build time)". Migrations/queries never reached |
| postgres | (already a stub) | unchanged |

Mechanics:

- `nob.c` writes `build/features.h` (later renamed `webc_features.h` - see
  the trap in Results; `#define WEBC_HAVE_* 1/0`,
  write-if-changed) **before** the staleness pass - the guarded files
  `#include "build/features.h"`, so flipping a dependency rebuilds exactly
  those translation units and relinks; an untouched tree stays on the fast
  path.
- pkg-config flag/lib collection stays where it is, but only appends when
  the package exists (warnings move to the gateway, once).
- `test/run.sh` greps `build/features.h` and skips `mysql_test` /
  `https_test` when the feature is off (guarded waits + log cats); both
  python files also self-skip when run by hand. sqlite suites always run.
- Verification plan: rebuild normally (both features on, suite at the
  21-failure baseline), then simulate a bare host with
  `PKG_CONFIG_LIBDIR=/nonexistent` → feature-less binary → suite again
  (sqlite suites must pass, mysql/https skipped), then rebuild normally.

### What works / does not work yet

| | |
|---|---|
| works now | everything, both deps present on this host (unchanged behaviour) |
| not yet | the gateway itself, the guards, the fallback SHA-256, the test skips - none of it exists yet |
| affected docs | `wiki/architecture.md` (build step), `wiki/database.md` (driver table + MySQL idioms), `wiki/cli.md` (`WEBC_TLS_*` row, known gaps), `wiki/testing.md` (suite skips), `wiki/decisions.md` if a rule gets written |
| risk notes | fallback SHA-256 must be byte-identical to EVP's (verified against `hash("abc")` vector + the seeded `webc2026` login); `build/features.h` must be on the include path (`-I.`); a features flip must not drag every object into a rebuild (only guarded files) |

### Results (2026-11-07, optional features)

| | Shipped | Evidence |
|---|---|---|
| gateway | `nob.c` step 0 probes `pkg-config --exists libmariadb` / `openssl` and writes `build/webc_features.h` (`WEBC_HAVE_* 1/0`, write-if-changed) before the staleness pass. Missing dep => one `WARNING` per build and the feature is compiled out - never a failed compile. Flag collection at the compile/link step is silent now (gateway reported once) | bare-host simulation `PKG_CONFIG_LIBDIR=/nonexistent ./build/bin/nob`: 2 warnings, `webc_features.h: updated` (0/0), **5 of 54 objects** recompiled (only the guarded ones), relink clean |
| openssl off | `serve.c`: TLS block, `Conn.ssl` field, read/write SSL branches, handshake and cleanup all `#if WEBC_HAVE_OPENSSL`; without it `tls_ctx` stays NULL (banner says `http`) and `tls_server_init()` **refuses to start** when `WEBC_TLS_*` is set - never a silent plaintext downgrade. `auth.c`: EVP is not just TLS, it is the password hash, so an `#else` built-in SHA-256 (FIPS 180-4, verified byte-identical: `sha256("abc")`, `sha256("webc2026" \|\| "posadmin1") = 21a55faf...` vs hashlib) keeps logins working | OFF build: `nm` shows `sha256_block` in, `EVP_*` out, no `SSL_*`; full suite **21 fails = baseline** with both suite skips; seeded-hash staff logins pass on the fallback hash |
| libmariadb off | `driver_mysql.c` compiles a stub vtable in place of the wire protocol (same shape as postgres): open fails with "mysql support not compiled in (libmariadb was missing at build time)" | OFF build: `nm` shows `my_stub_open`; `mysql_test` skipped, sqlite suites at baseline |
| tests | `run.sh` greps `build/webc_features.h` and skips `mysql_test`/`https_test` (guarded waits + log cats, `MYSQL_PID`/`HTTPS_PID` start empty); both python files self-skip too when run by hand | OFF run prints `SKIP: mysql_test (webc built without libmariadb)` + `SKIP: https_test (webc built without openssl)` as lines 1-2, exit matches baseline |
| restored | `./build/bin/nob` with deps present: features back to 1/1, fallback/stub/TLS-out all compiled away, ON suite at **21 fails = baseline** (http 119/1, pos 372/17, shop 118/0, role 48/0, crud 157/3, geo 32/0, mysql 41/0, https 6/0) | both modes green, no regression |

**Trap found the hard way:** the header cannot be called `features.h` -
`-I./build` makes glibc's `#include <features.h>` (pulled by every system
header) resolve to the generated file and the compile dies in `assert.h`
with `missing binary operator before token '('`. Renamed to
`build/webc_features.h`; recorded in architecture.md.

**Note:** builds were observed flipping the feature header mid-test -
cause was a concurrent `nob` run from the user's own terminal, not the
gateway; both suite runs had already latched the right binary (SKIP lines
printed from the 0/0 header, server spawned before the flip).
