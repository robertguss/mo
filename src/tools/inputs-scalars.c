#include "tools/inputs-internal.h"

#include <string.h>

static MO_MUST_USE int64_t signed_word(uint64_t word) {
    return word <= INT64_MAX ? (int64_t)word : -1 - (int64_t)(UINT64_MAX - word);
}

static MO_MUST_USE int64_t integer(struct mo_input_builder *builder) {
    uint64_t word = mo_input_random(&builder->random);
    uint32_t bits = builder->size < 60 ? builder->size + 4 : 64;
    if (bits < 64) {
        word &= (UINT64_C(1) << bits) - 1;
        if ((word & (UINT64_C(1) << (bits - 1))) != 0)
            word |= UINT64_MAX << bits;
    }
    return signed_word(word);
}

static MO_MUST_USE mo_i128 smaller(mo_i128 value, uint32_t variant) {
    MO_ASSERT(variant < 128);
    if (variant == 0)
        return 0;
    mo_u128 magnitude = value < 0 ? (mo_u128)(-(value + 1)) + 1 : (mo_u128)value;
    /* Zero was already tried. Skip repeated one-unit distances too. */
    if (magnitude <= 1 || (variant > 1 && (magnitude >> (variant - 1)) <= 1))
        return value;
    mo_u128 distance = magnitude >> variant;
    if (distance == 0)
        distance = 1;
    mo_i128 result = (mo_i128)(magnitude - distance);
    return value < 0 ? -result : result;
}

static MO_MUST_USE uint64_t float_smaller(uint64_t bits, uint32_t variant) {
    uint64_t sign = bits & (UINT64_C(1) << 63);
    uint64_t magnitude = bits & ~(UINT64_C(1) << 63);
    if (variant == 0)
        return 0;
    if (magnitude >= UINT64_C(0x7ff0000000000000)) {
        return sign | (variant == 1 ? UINT64_C(0x3ff0000000000000) : UINT64_C(0x7fefffffffffffff));
    }
    if (variant == 2)
        return sign | (magnitude == 0 ? 0 : magnitude - 1);
    uint64_t exponent = magnitude >> 52;
    if (exponent > 1)
        return sign | (magnitude - (UINT64_C(1) << 52));
    return sign | (magnitude >> 1);
}

enum mo_input_result mo_input_immediate_build(struct mo_input_builder *builder,
                                              const struct mo_input_value_request *request,
                                              struct mo_cell *out) {
    enum mo_type_kind kind = builder->inputs->types->items[request->type].kind;
    bool random = request->mode == MO_INPUT_PRNG;
    uint32_t index = request->mode == MO_INPUT_SAMPLE ? request->boundary : 0;
    const int64_t ints[] = {0, 1, -1, INT64_MIN, INT64_MAX};
    const int64_t times[] = {0, INT64_C(-62135596800000000), INT64_C(-62104060800000001),
                             INT64_C(253370764800000000), INT64_C(253402300799999999)};
    const uint64_t floats[] = {0,
                               UINT64_C(0x3ff0000000000000),
                               UINT64_C(0xbff0000000000000),
                               UINT64_C(0x8000000000000000),
                               UINT64_C(0x7fefffffffffffff),
                               UINT64_C(0xffefffffffffffff),
                               UINT64_C(0x7ff0000000000000),
                               UINT64_C(0xfff0000000000000),
                               UINT64_C(0x7ff8000000000000),
                               1,
                               UINT64_C(0x8000000000000001)};
    const uint32_t chars[] = {0, 'a', 0xe9, 0xd7ff, 0xe000, 0x10ffff};
    mo_i128 max = (mo_i128)(((mo_u128)1 << 127) - 1);
    const mo_i128 decimals[] = {
        0, 1, INT64_C(1000000000000000000), -INT64_C(1000000000000000000), -max, max};
    const struct mo_range ranges[] = {
        {0, 0}, {0, 1}, {1, 0}, {-1, 1}, {INT64_MIN, INT64_MAX}, {INT64_MAX, INT64_MIN}};
    bool copy = request->mode == MO_INPUT_COPY;
    if (copy && request->edit == NULL) {
        *out = request->source;
        return MO_INPUT_OK;
    }
    uint32_t variant = request->edit == NULL ? 0 : request->edit->variant;
    struct mo_cell result = {0};
    switch (kind) {
    case MO_TYPE_UNIT:
        result = mo_cell_unit();
        break;
    case MO_TYPE_BOOL:
        result = mo_cell_bool(copy     ? false
                              : random ? (mo_input_random(&builder->random) & 1) != 0
                                       : index != 0);
        break;
    case MO_TYPE_BYTE: {
        const uint8_t bytes[] = {0, 1, UINT8_MAX};
        uint8_t value = copy     ? (uint8_t)smaller(mo_cell_get_byte(request->source), variant)
                        : random ? (uint8_t)mo_input_random(&builder->random)
                                 : bytes[index];
        result = mo_cell_byte(value);
        break;
    }
    case MO_TYPE_INT:
        result = mo_cell_int(copy     ? (int64_t)smaller(mo_cell_get_int(request->source), variant)
                             : random ? integer(builder)
                                      : ints[index]);
        break;
    case MO_TYPE_FLOAT: {
        uint64_t bits;
        if (copy) {
            memcpy(&bits, request->source.bytes, sizeof bits);
            bits = float_smaller(bits, variant);
        } else {
            bits = random ? mo_input_random(&builder->random) : floats[index];
        }
        memcpy(result.bytes, &bits, sizeof bits);
        break;
    }
    case MO_TYPE_CHAR: {
        uint32_t scalar;
        if (copy) {
            scalar = (uint32_t)smaller(mo_cell_get_char(request->source), variant);
            if (scalar >= 0xd800 && scalar <= 0xdfff)
                scalar = 0xd7ff;
        } else if (random) {
            scalar = (uint32_t)(mo_input_random(&builder->random) % 0x10f800);
            if (scalar >= 0xd800)
                scalar += 0x800;
        } else
            scalar = chars[index];
        result = mo_cell_char(scalar);
        break;
    }
    case MO_TYPE_TIME: {
        int64_t value = copy     ? (int64_t)smaller(mo_cell_get_time(request->source), variant)
                        : random ? times[1] + (int64_t)(mo_input_random(&builder->random) %
                                                        UINT64_C(315537897600000000))
                                 : times[index];
        result = mo_cell_time(value);
        break;
    }
    case MO_TYPE_DURATION: {
        const int64_t durations[] = {0, INT64_MIN, INT64_MAX};
        result =
            mo_cell_duration(copy ? (int64_t)smaller(mo_cell_get_duration(request->source), variant)
                             : random ? integer(builder)
                                      : durations[index]);
        break;
    }
    case MO_TYPE_DECIMAL: {
        mo_i128 value;
        if (copy) {
            value = smaller(mo_cell_get_decimal(request->source), variant);
        } else if (random) {
            uint32_t cap = builder->size < 37 ? builder->size + 1 : 38;
            uint32_t digits = 1 + (uint32_t)(mo_input_random(&builder->random) % cap);
            uint32_t scale = (uint32_t)(mo_input_random(&builder->random) % 19);
            mo_u128 magnitude = 0;
            for (uint32_t d = 0; d < digits; d++)
                magnitude = magnitude * 10 + mo_input_random(&builder->random) % 10;
            /* A scale of 0 is integral; 18 retains the smallest step. Stop
             * increasing the magnitude before the representation maximum. */
            for (uint32_t d = scale; d < 18 && magnitude <= (mo_u128)max / 10; d++)
                magnitude *= 10;
            value = (mo_i128)magnitude;
            if ((mo_input_random(&builder->random) & 1) != 0)
                value = -value;
        } else
            value = decimals[index];
        result = mo_cell_decimal(value);
        break;
    }
    case MO_TYPE_RANGE: {
        struct mo_range range;
        if (copy) {
            range = mo_cell_get_range(request->source);
            if (variant < 65)
                range.start = (int64_t)smaller(range.start, variant);
            else
                range.end = (int64_t)smaller(range.end, variant - 65);
        } else if (random) {
            range.start = integer(builder);
            range.end = integer(builder);
        } else
            range = ranges[index];
        result = mo_cell_range(range.start, range.end);
        break;
    }
    case MO_TYPE_TEXT:
    case MO_TYPE_BYTES:
    case MO_TYPE_BIGINT:
    case MO_TYPE_TUPLE:
    case MO_TYPE_LIST:
    case MO_TYPE_SET:
    case MO_TYPE_MAP:
    case MO_TYPE_RECORD:
    case MO_TYPE_CHOICE:
    case MO_TYPE_FUNCTION:
    case MO_TYPE_ENV:
    case MO_TYPE_CAPABILITY:
        MO_ASSERT(false);
        return MO_INPUT_UNSUPPORTED;
    }
    if (copy && memcmp(result.bytes, request->source.bytes, sizeof result.bytes) == 0)
        return MO_INPUT_DONE;
    *out = result;
    return MO_INPUT_OK;
}

static MO_MUST_USE enum mo_input_result blob(struct mo_input_builder *builder,
                                             const struct mo_input_value_request *request,
                                             struct mo_cell *out) {
    enum mo_type_kind kind = builder->inputs->types->items[request->type].kind;
    size_t length = 0;
    const uint8_t *source = NULL;
    enum mo_bigint_sign sign = MO_BIGINT_ZERO;
    uint8_t *bytes = NULL;
    bool copy = request->mode == MO_INPUT_COPY;
    if (copy) {
        source = mo_value_data(request->source, &length);
        if (kind == MO_TYPE_BIGINT)
            sign = mo_value_bigint_sign(request->source);
        if (request->edit != NULL && kind == MO_TYPE_BIGINT) {
            if (length == 0)
                return MO_INPUT_DONE;
            if (!mo_input_work(&builder->budget, length))
                return MO_INPUT_LIMIT;
            if (request->edit->variant == 0) {
                length = 0;
                sign = MO_BIGINT_ZERO;
            } else {
                bytes = mo_scratch_resize(&builder->scratch, NULL, 0, length);
                if (bytes == NULL)
                    return MO_INPUT_NO_MEMORY;
                if (request->edit->variant == 1) {
                    uint8_t carry = 0;
                    for (size_t i = length; i != 0;) {
                        i--;
                        bytes[i] = (uint8_t)((source[i] >> 1) | carry);
                        carry = (uint8_t)(source[i] << 7);
                    }
                } else {
                    memcpy(bytes, source, length);
                    size_t i = 0;
                    while (i < length && bytes[i] == 0)
                        bytes[i++] = UINT8_MAX;
                    MO_ASSERT(i < length);
                    bytes[i]--;
                }
                while (length != 0 && bytes[length - 1] == 0)
                    length--;
                if (length == 0)
                    sign = MO_BIGINT_ZERO;
                source = bytes;
            }
        } else if (request->edit != NULL) {
            size_t start = request->edit->start, end = start + request->edit->count;
            if (kind == MO_TYPE_TEXT) {
                if (!mo_input_work(&builder->budget, length))
                    return MO_INPUT_LIMIT;
                uint32_t scalar = 0;
                start = end = length;
                for (size_t i = 0; i < length; i++) {
                    if ((source[i] & 0xc0) != 0x80) {
                        if (scalar == request->edit->start)
                            start = i;
                        if (scalar == request->edit->start + request->edit->count)
                            end = i;
                        scalar++;
                    }
                }
            }
            MO_ASSERT(start <= end && end <= length);
            size_t shortened = length - (end - start);
            if (!mo_input_bytes(&builder->budget, shortened))
                return MO_INPUT_LIMIT;
            if (shortened != 0) {
                bytes = mo_scratch_resize(&builder->scratch, NULL, 0, shortened);
                if (bytes == NULL)
                    return MO_INPUT_NO_MEMORY;
                memcpy(bytes, source, start);
                memcpy(bytes + start, source + end, length - end);
            }
            return (kind == MO_TYPE_TEXT
                        ? mo_value_text(builder->heap, (const char *)bytes, shortened, out)
                        : mo_value_bytes(builder->heap, bytes, shortened, out))
                       ? MO_INPUT_OK
                       : MO_INPUT_NO_MEMORY;
        }
    } else {
        uint32_t index = request->mode == MO_INPUT_SAMPLE ? request->boundary : 0;
        bool random = request->mode == MO_INPUT_PRNG;
        if (random) {
            uint64_t cap = (uint64_t)request->size * (kind == MO_TYPE_BIGINT ? 16 : 4);
            if (cap > builder->budget.left.bytes)
                cap = builder->budget.left.bytes;
            length = (size_t)(mo_input_random(&builder->random) % (cap + 1));
        } else
            length = index == 0 ? 0 : index < 3 ? 1 : kind == MO_TYPE_BIGINT ? 128 : 1024;
        if (!mo_input_bytes(&builder->budget, length))
            return MO_INPUT_LIMIT;
        if (length != 0) {
            bytes = mo_scratch_resize(&builder->scratch, NULL, 0, length);
            if (bytes == NULL)
                return MO_INPUT_NO_MEMORY;
            if (random) {
                for (size_t i = 0; i < length; i++)
                    bytes[i] = (uint8_t)mo_input_random(&builder->random);
            } else
                memset(bytes, kind == MO_TYPE_TEXT ? 'x' : UINT8_MAX, length);
        }
        if (kind == MO_TYPE_TEXT) {
            if (!random && index == 1)
                bytes[0] = 'a';
            if (!random && index == 2) {
                if (!mo_input_bytes(&builder->budget, 1))
                    return MO_INPUT_LIMIT;
                length = 2;
                source = (const uint8_t *)"\xc3\xa9";
            } else {
                /* ASCII, é, the euro sign and an emoji; never split a scalar. */
                static const char *const scalars[] = {"a", "\xc3\xa9", "\xe2\x82\xac",
                                                      "\xf0\x9f\x98\x80"};
                static const size_t lengths[] = {1, 2, 3, 4};
                for (size_t i = 0; random && i < length;) {
                    uint32_t scalar = (uint32_t)(mo_input_random(&builder->random) % 4);
                    if (lengths[scalar] > length - i)
                        scalar = 0;
                    memcpy(bytes + i, scalars[scalar], lengths[scalar]);
                    i += lengths[scalar];
                }
                source = bytes;
            }
        } else {
            source = bytes;
            if (kind == MO_TYPE_BYTES && !random && index == 1)
                bytes[0] = 0;
            if (kind == MO_TYPE_BIGINT && length != 0) {
                if (!random && index < 3)
                    bytes[0] = 1;
                if (bytes[length - 1] == 0)
                    bytes[length - 1] = 1;
                sign = random ? (mo_input_random(&builder->random) & 1) != 0 ? MO_BIGINT_NEGATIVE
                                                                             : MO_BIGINT_POSITIVE
                       : index == 2 || index == 4 ? MO_BIGINT_NEGATIVE
                                                  : MO_BIGINT_POSITIVE;
            }
        }
    }
    if (copy && !mo_input_bytes(&builder->budget, length))
        return MO_INPUT_LIMIT;
    bool ok = kind == MO_TYPE_TEXT ? mo_value_text(builder->heap, (const char *)source, length, out)
              : kind == MO_TYPE_BYTES ? mo_value_bytes(builder->heap, source, length, out)
                                      : mo_value_bigint(builder->heap, sign, source, length, out);
    return ok ? MO_INPUT_OK : MO_INPUT_NO_MEMORY;
}

enum mo_input_result mo_input_scalar_build(struct mo_input_builder *builder,
                                           const struct mo_input_value_request *request,
                                           struct mo_cell *out) {
    const struct mo_input_factory *factory = builder->inputs->info[request->type].factory;
    if (factory != NULL) {
        struct mo_input_limits remaining = builder->budget.left;
        remaining.depth = request->remaining_depth;
        struct mo_input_limits before = remaining;
        enum mo_input_result result;
        if (request->mode == MO_INPUT_COPY) {
            if (request->edit != NULL) {
                if (factory->shrink == NULL)
                    return MO_INPUT_DONE;
                result = factory->shrink(factory->context, builder->heap, request->source,
                                         request->edit->variant, &remaining, out);
            } else {
                if (factory->copy == NULL)
                    return MO_INPUT_UNSUPPORTED;
                result = factory->copy(factory->context, builder->heap, request->source, &remaining,
                                       out);
            }
        } else {
            struct mo_input_request leaf = {
                .kind = request->mode == MO_INPUT_PRNG ? MO_INPUT_RANDOM : MO_INPUT_BOUNDARY,
                .boundary = request->mode == MO_INPUT_SAMPLE ? request->boundary : 0,
                .size = request->size,
                .seed = mo_input_random(&builder->random)};
            result = factory->build(factory->context, builder->heap, &leaf, &remaining, out);
        }
        MO_ASSERT(remaining.nodes <= before.nodes && remaining.bytes <= before.bytes &&
                  remaining.work <= before.work && remaining.depth == before.depth &&
                  remaining.elements == before.elements &&
                  remaining.combinations == before.combinations &&
                  remaining.metadata == before.metadata &&
                  remaining.prepare_work == before.prepare_work);
        builder->budget.left.nodes = remaining.nodes;
        builder->budget.left.bytes = remaining.bytes;
        builder->budget.left.work = remaining.work;
        return result;
    }
    enum mo_type_kind kind = builder->inputs->types->items[request->type].kind;
    return kind == MO_TYPE_TEXT || kind == MO_TYPE_BYTES || kind == MO_TYPE_BIGINT
               ? blob(builder, request, out)
               : mo_input_immediate_build(builder, request, out);
}
