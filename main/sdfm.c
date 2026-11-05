/*
 * sdfm.c — path confinement for the SD file manager.
 *
 * GPL-3.0. See LICENSE.
 */
#include "sdfm.h"

#include <string.h>
#include <stdio.h>

#include "sdcard.h"

bool sdfm_is_safe(const char *abspath)
{
    size_t n = strlen(SD_MOUNT_POINT);
    if (strncmp(abspath, SD_MOUNT_POINT, n) != 0) return false;
    /* must be exactly the mount, or mount followed by '/' */
    return abspath[n] == '\0' || abspath[n] == '/';
}

bool sdfm_resolve(const char *rel, char *out, size_t out_sz)
{
    if (!rel) rel = "";
    while (*rel == '/') rel++;              /* strip leading slashes */

    /* Reject any parent traversal outright. */
    if (strstr(rel, "..")) return false;

    int n;
    if (*rel == '\0') {
        n = snprintf(out, out_sz, "%s", SD_MOUNT_POINT);
    } else {
        n = snprintf(out, out_sz, "%s/%s", SD_MOUNT_POINT, rel);
    }
    if (n < 0 || (size_t)n >= out_sz) return false;

    return sdfm_is_safe(out);
}
