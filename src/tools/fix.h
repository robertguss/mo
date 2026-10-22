/* Private command integration: the edits `mo fix` makes (reference appendix A). */
#ifndef MO_TOOLS_FIX_H
#define MO_TOOLS_FIX_H

#include "tools/tools.h"

struct stat;

/* Writes into out the text of sources' file with the fixes applied that the diagnostics offer
 * alone: a diagnostic with exactly one fix, whose edits all fall in that file. Diagnostics are
 * taken in position order, and a fix that would overlap one already taken, as struct mo_fix
 * defines overlap, waits for a later round. Sorts the diagnostics in place. Sets *applied to the
 * number of fixes taken, and returns false only when out or the planning memory runs out. */
MO_MUST_USE bool mo_fix_source(const struct mo_sources *sources, uint32_t file,
                               struct mo_diags *diags, struct mo_buffer *out, uint32_t *applied);

/* Replaces the regular file at path with output, atomically, keeping its permission bits. original
 * is the file's lstat from before it was read: a file replaced since then is left alone (EBUSY).
 * On failure *error holds the errno that explains it. */
MO_MUST_USE bool mo_replace_file(const char *path, const struct mo_buffer *output,
                                 const struct stat *original, int *error);

#endif
