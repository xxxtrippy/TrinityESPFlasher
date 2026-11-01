#ifndef SDCARD_H
#define SDCARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * microSD mount + xeBuild data-package access.
 *
 * The card is expected to hold the xeBuild "data" folder — the same set of
 * ingredient files J-Runner-With-Extras ships in its data/ directory for the
 * target dashboard version. We never bundle these (they are Microsoft-derived
 * and version-specific); the user supplies the card.
 *
 * Mount point: /sd
 * Expected package root: /sd/xebuild/data   (overridable)
 */

#define SD_MOUNT_POINT "/sd"
#define XEBUILD_DATA_ROOT SD_MOUNT_POINT "/xebuild/data"

/* Mount the card (SDMMC 1-bit). Returns true on success. */
bool sd_mount(void);
void sd_unmount(void);
bool sd_is_mounted(void);

/* Total / free bytes, for the web UI to show the user. 0 if unknown. */
uint64_t sd_total_bytes(void);
uint64_t sd_free_bytes(void);

/*
 * One ingredient file required to build an image. `required` entries must be
 * present for a build to proceed; optional ones (e.g. a specific xeLL variant)
 * are reported but do not block.
 */
typedef struct {
    const char *rel_path;  /* path under XEBUILD_DATA_ROOT, e.g. "CB_9188.bin" */
    const char *descr;     /* human label for the web UI                        */
    bool required;
    /* filled in by sd_check_package(): */
    bool present;
    uint32_t size;
} xebuild_ingredient_t;

/*
 * Validate that the card holds a usable xeBuild data package for a Trinity
 * glitch image. Fills `present`/`size` on each ingredient. Returns the number
 * of REQUIRED ingredients that are missing (0 == ready to build).
 *
 * The ingredient list is intentionally data-driven (see sdcard.c) because the
 * exact CB/patch filenames depend on the dashboard version the user targets.
 */
int sd_check_package(xebuild_ingredient_t *list, size_t count);

/* Open an ingredient for streaming read. Returns a FILE* (caller fcloses) or
 * NULL. `rel_path` is relative to XEBUILD_DATA_ROOT. */
void *sd_open_ingredient(const char *rel_path);

#endif
