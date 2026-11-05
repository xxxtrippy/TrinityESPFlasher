#ifndef USBPF_H
#define USBPF_H
/* PicoFlasher-compatible USB CDC device on the native USB port so J-Runner
 * can dump/flash through this board. NAND only (Trinity). */
void usbpf_start(void);
#endif
