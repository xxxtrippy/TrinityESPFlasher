#ifndef JOBS_H
#define JOBS_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Shared job engine: the dump / build / flash operations run on one background
 * worker. The touch UI kicks jobs and polls their progress. (Formerly driven
 * by the web server; the standalone build drives it from the screen.)
 */

typedef enum { JOB_NONE, JOB_DUMP, JOB_BUILD, JOB_FLASH, JOB_XELL } job_t;

typedef struct {
    bool  busy;
    int   percent;
    char  stage[48];
    bool  done;
    bool  ok;         /* last finished job succeeded */
    bool  flashable;  /* a BUILD_OK image is ready to flash */
    bool  have_dump;
    uint32_t flash_config;
    bool  linked;     /* SMC stopped / console reachable */
} job_status_t;

void jobs_init(void);

/* Returns false if the worker is busy with another job. */
bool jobs_start(job_t job);

/* Exclusive console access for the USB/J-Runner link. */
bool jobs_usb_claim(void);
void jobs_usb_release(void);

/* Snapshot current status. */
void jobs_get(job_status_t *out);

/* Set the CPU key (32 hex chars) used by JOB_BUILD. Returns false if invalid.
 * The UI calls jobs_load_cpukey_from_sd() to read /sd/cpukey.txt instead. */
bool jobs_set_cpukey_hex(const char *hex);

/* Try to load the CPU key from /sd/cpukey.txt (first 32 hex chars). */
bool jobs_load_cpukey_from_sd(void);

/* Run console detect (stop SMC, read flash config) synchronously; updates the
 * linked/flash_config fields. Quick, so not a worker job. */
void jobs_detect(void);

#endif
