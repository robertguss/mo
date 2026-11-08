#ifndef MO_UPLOAD_H
#define MO_UPLOAD_H
#include "base/base.h"
#include "base/limits.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One candidate upload: the bundle's bytes arrive as consecutive decoded pages. */
struct mo_upload {
    uint8_t *bytes;  /* caller storage with capacity >= total */
    size_t total;    /* declared when the slot opened: 1..MO_LIMIT_BUNDLE_BYTES */
    size_t received; /* decoded bytes accepted so far */
    uint64_t next;   /* index the next page must carry */
};
enum mo_upload_status {
    MO_UPLOAD_MORE,     /* page accepted; more pages expected */
    MO_UPLOAD_COMPLETE, /* page accepted; received == total */
    MO_UPLOAD_ORDER,    /* index or offset is not the next exact range */
    MO_UPLOAD_TOTAL,    /* the page's total differs from the slot's */
    MO_UPLOAD_SIZE,     /* decoded page empty, longer than a page, a short non-final page, or past
                           total */
    MO_UPLOAD_ENCODING  /* not canonical padded base64 */
};
void mo_upload_init(struct mo_upload *upload, uint8_t *bytes, size_t total);
/* Page i carries index i, offset i * MO_LIMIT_UPGRADE_PAGE_BYTES and the slot's total; all but
 * the last decode to exactly MO_LIMIT_UPGRADE_PAGE_BYTES bytes. Checks run in the order total,
 * index, offset, encoding, size. Any refusal leaves *upload and its bytes unchanged. */
MO_MUST_USE enum mo_upload_status mo_upload_page(struct mo_upload *upload, uint64_t index,
                                                 uint64_t offset, uint64_t total,
                                                 const uint8_t *text, size_t length);
/* RFC 4648 standard alphabet, '=' padding required, no whitespace, zero trailing bits, so each
 * byte string has exactly one accepted encoding. Never writes out past capacity; on refusal
 * *written and out are unspecified. */
MO_MUST_USE bool mo_upload_base64(const uint8_t *text, size_t length, uint8_t *out, size_t capacity,
                                  size_t *written);
#endif
