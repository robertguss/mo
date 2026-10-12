#include "core/core.h"

#include <string.h>

#include "base/codes.h"
#include "base/limits.h"

/* First pass: program invariants. Expression checking follows in the next pass. */
static MO_MUST_USE bool refuse(struct mo_diags *diags, uint16_t code, const char *reason) {
    struct mo_diag diag = {.code = code, .message = "invalid compiler Core", .reason = reason};
    if (!mo_diags_add(diags, &diag))
        return false;
    return false;
}
static MO_MUST_USE bool same(mo_core_string a, mo_core_string b) {
    return a.length == b.length && (a.length == 0 || memcmp(a.bytes, b.bytes, a.length) == 0);
}
static MO_MUST_USE bool natural_at_most(mo_core_natural n, size_t limit) {
    size_t value = 0;
    for (size_t i = 0; i < n.digits.length; i++) {
        size_t digit = (size_t)(n.digits.bytes[i] - '0');
        if (digit > limit || value > (limit - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    return !n.negative;
}
bool mo_core_check(const struct mo_core *p, struct mo_diags *diags) {
    MO_ASSERT(p != NULL && diags != NULL);
    if (!mo_core_metadata_valid(p))
        return refuse(diags, MO_CODE_CORE_CHECK_METADATA,
                      "Compiler metadata has an invalid shape.");
    for (size_t i = 0; i < p->consts.count; i++)
        for (size_t j = 0; j < i; j++)
            if (same(p->consts.items[i].name, p->consts.items[j].name))
                return refuse(diags, MO_CODE_CORE_CHECK_CONSTANT_NAME,
                              "Two Core constants have the same lookup name.");
    const struct mo_core_fun *entry = NULL;
    for (size_t i = 0; i < p->funs.count; i++)
        if (same(p->entry, p->funs.items[i].name)) {
            entry = &p->funs.items[i];
            break;
        }
    if (entry == NULL)
        return refuse(diags, MO_CODE_CORE_CHECK_ENTRY, "The Core entry function does not exist.");
    if (entry->kind != MO_CORE_FUNCTION)
        return refuse(diags, MO_CODE_CORE_CHECK_ENTRY_KIND,
                      "The Core entry names spec code rather than an ordinary function.");
    for (size_t i = 0; i < p->funs.count; i++) {
        const struct mo_core_fun *f = &p->funs.items[i];
        bool valid =
            f->kind == MO_CORE_FUNCTION ||
            ((f->kind == MO_CORE_TEST || f->kind == MO_CORE_EXAMPLE) && f->params.count == 0 &&
             f->ret->tag == MO_CORE_TY_UNIT) ||
            (f->kind == MO_CORE_PROMISE && f->params.count != 0 && f->ret->tag == MO_CORE_TY_INT) ||
            (f->kind == MO_CORE_ANCHOR && f->params.count == 0 && f->ret->tag == MO_CORE_TY_INT);
        if (!valid)
            return refuse(diags, MO_CODE_CORE_CHECK_KIND,
                          "A Core spec function has the wrong parameter or result signature.");
        if (!natural_at_most(f->const_count, p->consts.count))
            return refuse(diags, MO_CODE_CORE_CHECK_PREFIX,
                          "A Core function requires more constants than the program declares.");
    }
    return true;
}
