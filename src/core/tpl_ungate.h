// Counterpart of tpl_emit.h: restores every macro it pushed. Pop order is
// the reverse of the pushes only in the sense that each pop restores its
// own macro's prior state (definition or "undefined") - nesting works
// because an inner template's pops restore the outer template's
// definitions, and a host's pre-existing definitions (webc_template.h
// globals) are back in force for the rest of the TU. Guardless by design -
// see tpl_emit.h.
#pragma pop_macro("CURRENT_PATH")
#pragma pop_macro("NAV_ACTIVE")
#pragma pop_macro("CLS")
#pragma pop_macro("ESCAPED")
#pragma pop_macro("INT")
#pragma pop_macro("STR")
#pragma pop_macro("OUT")
