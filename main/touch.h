#ifndef TOUCH_H
#define TOUCH_H

#include <stdbool.h>

bool touch_init(void);

/* If the panel is being touched, writes screen coords (0..319, 0..239) to
 * *x,*y and returns true. Otherwise returns false. */
bool touch_get(int *x, int *y);

#endif
