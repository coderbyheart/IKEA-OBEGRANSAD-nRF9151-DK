/*
 * Weather forecast for the current hour from api.met.no (MET Norway).
 */

#ifndef WEATHER_H_
#define WEATHER_H_

#include <stdbool.h>

enum weather_icon {
	WEATHER_CLEAR_DAY,
	WEATHER_CLEAR_NIGHT,
	WEATHER_PARTLY_CLOUDY,
	WEATHER_CLOUDY,
	WEATHER_FOG,
	WEATHER_RAIN,
	WEATHER_SNOW,
	WEATHER_THUNDER,
};

struct weather {
	enum weather_icon icon;
	/* Degrees Celsius, rounded */
	int temperature;
};

/* Provisions the certificate used to verify api.met.no, call before connecting to LTE */
int weather_init(void);
/* Starts and stops the periodic updates, which need an LTE connection */
void weather_set_connected(bool connected);
/* Returns false until the first forecast was received */
bool weather_get(struct weather *out);

#endif /* WEATHER_H_ */
