/*
 * Clock for the IKEA OBEGRÄNSAD 16x16 LED panel on the nRF9151 DK, using the
 * time (and time zone) provided by the LTE network.
 */

#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include <date_time.h>

#include "panel.h"

LOG_MODULE_REGISTER(clock, LOG_LEVEL_INF);

/* Keep this low when powering the panel from a PC USB port */
#define BRIGHTNESS_PERCENT 25

/* 8x7 digits, one byte per row, MSB is the leftmost pixel.
 * From github.com/ph1p/ikea-led-obegraensad (MIT License).
 */
static const uint8_t digits[10][7] = {
	{0x3e, 0x63, 0x73, 0x6b, 0x67, 0x63, 0x3e}, /* 0 */
	{0x03, 0x07, 0x0f, 0x03, 0x03, 0x03, 0x03}, /* 1 */
	{0x7e, 0x03, 0x03, 0x3e, 0x60, 0x63, 0x7f}, /* 2 */
	{0x3e, 0x63, 0x03, 0x1f, 0x03, 0x63, 0x3e}, /* 3 */
	{0x63, 0x63, 0x63, 0x3f, 0x03, 0x03, 0x03}, /* 4 */
	{0x7f, 0x63, 0x60, 0x7e, 0x03, 0x63, 0x3e}, /* 5 */
	{0x3e, 0x63, 0x60, 0x7e, 0x63, 0x63, 0x3e}, /* 6 */
	{0x7f, 0x43, 0x07, 0x0c, 0x18, 0x18, 0x18}, /* 7 */
	{0x3e, 0x63, 0x63, 0x3e, 0x63, 0x63, 0x3e}, /* 8 */
	{0x3e, 0x63, 0x63, 0x3f, 0x03, 0x63, 0x3e}, /* 9 */
};

static void draw_digit(int x, int y, int d)
{
	for (int row = 0; row < 7; row++) {
		for (int col = 0; col < 8; col++) {
			panel_set(x + col, y + row, digits[d][row] & (0x80 >> col));
		}
	}
}

/* Hours in rows 0-6, minutes in rows 8-14, seconds as a bar in row 15 */
static void draw_clock(const struct tm *t)
{
	panel_clear();
	draw_digit(0, 0, t->tm_hour / 10);
	draw_digit(8, 0, t->tm_hour % 10);
	draw_digit(0, 8, t->tm_min / 10);
	draw_digit(8, 8, t->tm_min % 10);
	for (int x = 0; x <= t->tm_sec * PANEL_COLS / 60; x++) {
		panel_set(x, PANEL_ROWS - 1, true);
	}
}

/* A dot running around the border (60 pixels) while waiting for the time */
static void draw_waiting(int step)
{
	int i = step % 60;
	int x, y;

	if (i < 15) {
		x = i, y = 0;
	} else if (i < 30) {
		x = 15, y = i - 15;
	} else if (i < 45) {
		x = 45 - i, y = 15;
	} else {
		x = 0, y = 60 - i;
	}

	panel_clear();
	panel_set(x, y, true);
}

static void lte_handler(const struct lte_lc_evt *const evt)
{
	if (evt->type != LTE_LC_EVT_NW_REG_STATUS) {
		return;
	}

	switch (evt->nw_reg_status) {
	case LTE_LC_NW_REG_REGISTERED_HOME:
	case LTE_LC_NW_REG_REGISTERED_ROAMING:
		LOG_INF("Connected to LTE network");
		break;
	case LTE_LC_NW_REG_SEARCHING:
		LOG_INF("Searching for LTE network");
		break;
	default:
		break;
	}
}

static void date_time_handler(const struct date_time_evt *evt)
{
	if (evt->type == DATE_TIME_OBTAINED_MODEM) {
		LOG_INF("Got time from LTE network");
	}
}

int main(void)
{
	int err;

	err = panel_init();
	if (err) {
		LOG_ERR("Panel init failed: %d", err);
		return 0;
	}
	panel_brightness(BRIGHTNESS_PERCENT);

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("Modem library init failed: %d", err);
		return 0;
	}

	date_time_register_handler(date_time_handler);

	err = lte_lc_connect_async(lte_handler);
	if (err) {
		LOG_ERR("LTE connect failed: %d", err);
		return 0;
	}

	for (int step = 0;; step++) {
		int64_t ms;

		/* The network provides the local time zone; fall back to UTC without it.
		 * Checked first because date_time_now() logs a warning while there is no time.
		 */
		if (date_time_is_valid() &&
		    (date_time_now_local(&ms) == 0 || date_time_now(&ms) == 0)) {
			time_t secs = ms / 1000;
			struct tm t;

			gmtime_r(&secs, &t);
			draw_clock(&t);
		} else {
			draw_waiting(step);
		}

		panel_show();
		k_msleep(100);
	}

	return 0;
}
