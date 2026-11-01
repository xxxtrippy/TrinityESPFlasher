#ifndef WIFI_H
#define WIFI_H

/* Join the home network set in wifi.c (station mode); fall back to a self-hosted
 * access point only if that network can't be joined. The reachable IP is printed
 * to the serial monitor on boot. */
void wifi_init(void);

#endif
