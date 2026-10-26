#ifndef MO_FUZZ_STORE_RECORD_H
#define MO_FUZZ_STORE_RECORD_H

#include "base/binary.h"
#include "host/host.h"

static void fuzz_store_record(enum mo_store_record_kind kind, const uint8_t *data, size_t size) {
    if (size > MO_LIMIT_SAVED_BYTES + MO_STORE_FRAME_BYTES)
        return;
    struct mo_store_record record = {.sequence = UINT64_MAX};
    bool ok = mo_store_record_read(kind, data, size, &record);
    if (ok)
        MO_ASSERT(record.kind == kind && record.length <= size && record.sequence <= INT64_MAX);
    else
        MO_ASSERT(record.sequence == UINT64_MAX);
    /* Repair the checksum as well: arbitrary input must reach length/sequence
     * validation instead of spending every iteration at the checksum gate. */
    if (size >= MO_STORE_FRAME_BYTES) {
        struct mo_buffer bytes;
        mo_buffer_init(&bytes, MO_LIMIT_SAVED_BYTES + MO_STORE_FRAME_BYTES);
        MO_ASSERT(mo_buffer_append(&bytes, data, size - 8));
        MO_ASSERT(mo_binary_write(&bytes, mo_binary_checksum(bytes.bytes, bytes.length), 8));
        ok = mo_store_record_read(kind, bytes.bytes, bytes.length, &record);
        if (ok)
            MO_ASSERT(record.length == bytes.length - MO_STORE_FRAME_BYTES && record.kind == kind);
        mo_buffer_free(&bytes);
    }
}

#endif
