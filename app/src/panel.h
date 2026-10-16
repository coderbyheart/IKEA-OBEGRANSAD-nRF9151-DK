/*
 * Driver for the IKEA OBEGRÄNSAD 16x16 LED panel.
 */

#ifndef PANEL_H_
#define PANEL_H_

#include <stdbool.h>
#include <stdint.h>

#define PANEL_ROWS 16
#define PANEL_COLS 16

int panel_init(void);
void panel_clear(void);
void panel_set(int x, int y, bool on);
/* Sends the frame buffer to the panel */
int panel_show(void);
int panel_brightness(uint8_t percent);

#endif /* PANEL_H_ */
