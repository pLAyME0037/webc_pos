// Emit-macro gate for generated template headers (build/h_to_html/*.h).
// tt.c includes this file before every template body and tpl_ungate.h after
// it, so template hosts never hand-define OUT/STR/INT/ESCAPED/... again.
//
// Mechanism: push_macro saves the current definition (or "undefined"), the
// macro is then redefined to the standard body for the template body, and
// tpl_ungate.h pops it back. That is stateless and correct in every shape:
//  - nested templates (pagination, sidebar, md_child_* are #included from
//    inside an outer template body) push the outer's definition, redefine
//    identically, pop it back - the outer body keeps working;
//  - a host that pre-defines a macro (src/core/webc_template.h's globals
//    in the POS/auth TUs) gets its definition restored after the template;
//  - two templates in sequence each get a clean push/define/pop cycle;
//  - no ownership marks to leak (an inner ungate can no longer tear down
//    an outer macro).
//
// push_macro/pop_macro are supported by GCC, Clang and MSVC (the only
// compilers this build targets). Deliberately NO include guard: this is a
// per-include-action part and must re-run for every template.
//
// The target is the local `sb` (aliased at non-sb hosts with a one-line
// `String_Builder *sb = &buffer;`). Statement macros carry a trailing
// semicolon in the body - the historic per-site convention: templates call
// them bare, e.g. `CLS(...)` with no `;`, and `;;` from an explicit call
// site is a harmless empty statement (there are no unbraced if/else call
// sites). STR/ESCAPED are NULL-guarded and INT prints any integer.
#pragma push_macro("OUT")
#undef OUT
#define OUT(buf, size) sb_append_buf(sb, buf, size);

#pragma push_macro("STR")
#undef STR
#define STR(x) sb_append_cstr(sb, (x) ? (x) : "");

#pragma push_macro("INT")
#undef INT
#define INT(x) sb_appendf(sb, "%lld", (long long)(x));

#pragma push_macro("ESCAPED")
#undef ESCAPED
#define ESCAPED(x) sb_append_html_escaped(sb, (x) ? (x) : "");

#pragma push_macro("CLS")
#undef CLS
#define CLS(cond, t, f) sb_append_cstr(sb, (cond) ? (t) : (f));

#pragma push_macro("NAV_ACTIVE")
#undef NAV_ACTIVE
#define NAV_ACTIVE(prefix) (strncmp((current_path), (prefix), strlen(prefix)) == 0)

#pragma push_macro("CURRENT_PATH")
#undef CURRENT_PATH
#define CURRENT_PATH current_path
