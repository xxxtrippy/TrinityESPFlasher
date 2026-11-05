#ifndef KUART_H
#define KUART_H
#include <stdbool.h>
#include <stddef.h>

/* Xbox kernel debug UART capture + xeLL CPU-key auto-capture. */
#define KUART_LOG_SZ 4096

void   kuart_start(void);
size_t kuart_log_copy(char *dst, size_t cap);   /* last KUART_LOG_SZ bytes */
bool   kuart_cpukey(char out[33]);              /* true once a key was seen */

#endif
