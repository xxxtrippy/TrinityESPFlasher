#ifndef SDFM_H
#define SDFM_H

#include <stdbool.h>
#include <stddef.h>

/*
 * SD file-manager helpers. Every path is confined to the SD mount (/sd) — the
 * resolver rejects anything that would escape it (.. traversal, absolute paths
 * outside /sd), so the web UI can never touch firmware or anything off-card.
 *
 * `rel` is a path relative to the SD root, e.g. "" (root), "xebuild/data",
 * "cpukey.txt". A leading '/' is tolerated and treated as root-relative.
 */

/* Resolve `rel` to a real /sd/... path in `out` (size `out_sz`). Returns false
 * if the result would escape /sd. */
bool sdfm_resolve(const char *rel, char *out, size_t out_sz);

/* True if the absolute path is inside the SD mount. */
bool sdfm_is_safe(const char *abspath);

#endif
