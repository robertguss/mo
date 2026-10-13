#include "bytecode/assemble.h"

bool mo_text_shorthand(struct mo_text_reader *r) {
    static const char *const seeds[] = {"strings", "types", "layouts", "constants"};
    for (unsigned i = 0; i < 4; i++) {
        if (mo_text_is(r, seeds[i]) && !mo_text_indexed_section(r, (enum mo_bytecode_section)i)) {
            return false;
        }
    }
    if (r->token.kind != 0) {
        mo_text_refuse(r, r->token.start, "expected shorthand declaration");
        return false;
    }
    return true;
}
