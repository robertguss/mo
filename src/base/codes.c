#include "base/codes.h"

#include <stddef.h>

/* Every code is shown with four digits. */
#define MO_CODE_CHECK(number, identifier, section)                                                 \
    _Static_assert((number) >= 1 && (number) <= 9999, "E" #number " has four digits");
MO_CODES(MO_CODE_CHECK)
#undef MO_CODE_CHECK

const struct mo_code_info *mo_code_find(uint32_t number) {
    /* A switch, so that two rows with one number fail to compile. */
    switch (number) {
#define MO_CODE_CASE(code, name, cited)                                                            \
    case code: {                                                                                   \
        static const struct mo_code_info info = {                                                  \
            .number = code, .identifier = #name, .section = cited};                                \
        return &info;                                                                              \
    }
        MO_CODES(MO_CODE_CASE)
#undef MO_CODE_CASE
    default:
        return NULL;
    }
}
