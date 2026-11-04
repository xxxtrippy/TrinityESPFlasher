#ifndef RGB_H
#define RGB_H

#include <stdbool.h>

bool rgb_init(void);

void rgb_ready(void);
void rgb_dumping(void);
void rgb_building(void);
void rgb_flashing(void);
void rgb_verifying(void);
void rgb_error(void);
void rgb_complete(void);
void rgb_off(void);

#endif
