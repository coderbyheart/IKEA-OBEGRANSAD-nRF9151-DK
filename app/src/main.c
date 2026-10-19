/*
 * Countdown in days, clock and weather forecast for the IKEA OBEGRÄNSAD 16x16
 * LED panel on the nRF9151 DK, using the date and time (and time zone) provided
 * by the LTE network. Button 1 switches between them.
 */

#include <limits.h>
#include <stdio.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/timeutil.h>
#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include <date_time.h>
#include <dk_buttons_and_leds.h>

#include "panel.h"
#include "weather.h"

LOG_MODULE_REGISTER(display, LOG_LEVEL_INF);

/* Keep this low when powering the panel from a PC USB port */
#define BRIGHTNESS_PERCENT 25

/* The date to count down to */
#define TARGET_YEAR  2027
#define TARGET_MONTH 1
#define TARGET_DAY   31

enum mode {
	MODE_COUNTDOWN,
	MODE_CLOCK,
	MODE_WEATHER,
	MODE_COUNT,
};

static atomic_t mode = ATOMIC_INIT(MODE_WEATHER);

/* 4x7 digits for the countdown, one byte per row, bit 3 is the leftmost pixel */
static const uint8_t small_digits[10][7] = {
	{0x6, 0x9, 0x9, 0x9, 0x9, 0x9, 0x6}, /* 0 */
	{0x2, 0x6, 0x2, 0x2, 0x2, 0x2, 0x7}, /* 1 */
	{0x6, 0x9, 0x1, 0x2, 0x4, 0x8, 0xf}, /* 2 */
	{0x6, 0x9, 0x1, 0x6, 0x1, 0x9, 0x6}, /* 3 */
	{0x9, 0x9, 0x9, 0xf, 0x1, 0x1, 0x1}, /* 4 */
	{0xf, 0x8, 0xe, 0x1, 0x1, 0x9, 0x6}, /* 5 */
	{0x6, 0x8, 0x8, 0xe, 0x9, 0x9, 0x6}, /* 6 */
	{0xf, 0x1, 0x2, 0x2, 0x4, 0x4, 0x4}, /* 7 */
	{0x6, 0x9, 0x9, 0x6, 0x9, 0x9, 0x6}, /* 8 */
	{0x6, 0x9, 0x9, 0x7, 0x1, 0x1, 0x6}, /* 9 */
};

/* 8x7 digits for the clock, one byte per row, MSB is the leftmost pixel.
 * From github.com/ph1p/ikea-led-obegraensad (MIT License).
 */
static const uint8_t big_digits[10][7] = {
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

static void draw_glyph(int x, int y, const uint8_t *rows, int width)
{
	for (int row = 0; row < 7; row++) {
		for (int col = 0; col < width; col++) {
			panel_set(x + col, y + row, rows[row] & (1 << (width - 1 - col)));
		}
	}
}

/* The number of days centered on the panel */
static void draw_days(int days)
{
	char text[4];
	int n = snprintf(text, sizeof(text), "%d", CLAMP(days, 0, 999));
	int x = (PANEL_COLS - (n * 5 - 1)) / 2;

	panel_clear();
	for (int i = 0; i < n; i++) {
		draw_glyph(x + i * 5, (PANEL_ROWS - 7) / 2, small_digits[text[i] - '0'], 4);
	}
}

/* Hours in rows 0-6, minutes in rows 8-14, seconds as a bar in row 15 */
static void draw_clock(const struct tm *t)
{
	panel_clear();
	draw_glyph(0, 0, big_digits[t->tm_hour / 10], 8);
	draw_glyph(8, 0, big_digits[t->tm_hour % 10], 8);
	draw_glyph(0, 8, big_digits[t->tm_min / 10], 8);
	draw_glyph(8, 8, big_digits[t->tm_min % 10], 8);
	for (int x = 0; x <= t->tm_sec * PANEL_COLS / 60; x++) {
		panel_set(x, PANEL_ROWS - 1, true);
	}
}

/* Days from the local date to the target date */
static int days_until_target(int64_t local_ms)
{
	struct tm target = {
		.tm_year = TARGET_YEAR - 1900,
		.tm_mon = TARGET_MONTH - 1,
		.tm_mday = TARGET_DAY,
	};
	int64_t today = local_ms / 1000 / 86400;

	return timeutil_timegm64(&target) / 86400 - today;
}

/* 16x8 weather icons, '#' is a lit pixel */
static const char *const weather_icons[][8] = {
	[WEATHER_CLEAR_DAY] = {
		".......##.......",
		"..#...####...#..",
		"...#.######.#...",
		"..##.######.##..",
		"..##.######.##..",
		"...#.######.#...",
		"..#...####...#..",
		".......##.......",
	},
	[WEATHER_CLEAR_NIGHT] = {
		".....#####......",
		"....###.........",
		"...###.......#..",
		"...###......###.",
		"...###.......#..",
		"....###....#....",
		".....#####......",
		"................",
	},
	[WEATHER_PARTLY_CLOUDY] = {
		"........#.......",
		"....#..###..#...",
		"...#..#####..#..",
		"..#.########....",
		".....#########..",
		"..############..",
		"..############..",
		"....########....",
	},
	[WEATHER_CLOUDY] = {
		"................",
		".....####.......",
		"....######.####.",
		"..############..",
		".##############.",
		".##############.",
		"..############..",
		"................",
	},
	[WEATHER_FOG] = {
		"................",
		"..############..",
		"................",
		".############...",
		"................",
		"...############.",
		"................",
		".##############.",
	},
	[WEATHER_RAIN] = {
		".....####.......",
		"....######.####.",
		"..############..",
		".##############.",
		".##############.",
		"..############..",
		"..#..#..#..#....",
		"...#..#..#..#...",
	},
	[WEATHER_SNOW] = {
		".....####.......",
		"....######.####.",
		"..############..",
		".##############.",
		".##############.",
		"..############..",
		"..#...#...#...#.",
		"....#...#...#...",
	},
	[WEATHER_THUNDER] = {
		".....####.......",
		"....######.####.",
		"..############..",
		".##############.",
		".##############.",
		"..############..",
		".......##.......",
		"......##........",
	},
};

/* Icon in rows 0-7, the temperature centered in rows 9-15 */
static void draw_weather(const struct weather *w)
{
	char text[8];
	int n = snprintf(text, sizeof(text), "%d", CLAMP(w->temperature, -99, 99));
	int width = 0;
	int x;

	panel_clear();
	for (int row = 0; row < 8; row++) {
		for (int col = 0; col < PANEL_COLS; col++) {
			panel_set(col, row, weather_icons[w->icon][row][col] == '#');
		}
	}

	/* The minus sign is 3 pixels wide, digits 4, with a pixel in between */
	for (int i = 0; i < n; i++) {
		width += (text[i] == '-' ? 3 : 4) + (i > 0);
	}
	x = (PANEL_COLS - width) / 2;
	for (int i = 0; i < n; i++) {
		if (text[i] == '-') {
			for (int col = 0; col < 3; col++) {
				panel_set(x + col, 12, true);
			}
			x += 4;
		} else {
			draw_glyph(x, 9, small_digits[text[i] - '0'], 4);
			x += 5;
		}
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

static void button_handler(uint32_t button_state, uint32_t has_changed)
{
	if (has_changed & button_state & DK_BTN1_MSK) {
		atomic_set(&mode, (atomic_get(&mode) + 1) % MODE_COUNT);
	}
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
		weather_set_connected(true);
		break;
	case LTE_LC_NW_REG_SEARCHING:
		LOG_INF("Searching for LTE network");
		weather_set_connected(false);
		break;
	default:
		weather_set_connected(false);
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

	err = dk_buttons_init(button_handler);
	if (err) {
		LOG_ERR("Buttons init failed: %d", err);
	}

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("Modem library init failed: %d", err);
		return 0;
	}

	err = weather_init();
	if (err) {
		LOG_ERR("Weather init failed: %d", err);
	}

	date_time_register_handler(date_time_handler);

	err = lte_lc_connect_async(lte_handler);
	if (err) {
		LOG_ERR("LTE connect failed: %d", err);
		return 0;
	}

	int shown_days = INT_MIN;
	enum mode shown_mode = MODE_COUNT;
	static const char *const mode_names[] = {"countdown", "clock", "weather"};

	for (int step = 0;; step++) {
		enum mode current = atomic_get(&mode);
		int64_t ms;

		if (current != shown_mode) {
			LOG_INF("Showing %s", mode_names[current]);
			shown_mode = current;
		}

		/* The network provides the local time zone; fall back to UTC without it.
		 * Checked first because date_time_now() logs a warning while there is no time.
		 */
		if (current == MODE_WEATHER) {
			struct weather w;

			if (weather_get(&w)) {
				draw_weather(&w);
			} else {
				draw_waiting(step);
			}
		} else if (date_time_is_valid() &&
		    (date_time_now_local(&ms) == 0 || date_time_now(&ms) == 0)) {
			if (current == MODE_CLOCK) {
				time_t secs = ms / 1000;
				struct tm t;

				gmtime_r(&secs, &t);
				draw_clock(&t);
			} else {
				int days = days_until_target(ms);

				if (days != shown_days) {
					LOG_INF("%d days until %04d-%02d-%02d", days, TARGET_YEAR,
						TARGET_MONTH, TARGET_DAY);
					shown_days = days;
				}
				draw_days(days);
			}
		} else {
			draw_waiting(step);
		}

		panel_show();
		k_msleep(100);
	}

	return 0;
}
