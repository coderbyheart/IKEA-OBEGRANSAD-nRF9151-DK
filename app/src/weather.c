#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/atomic.h>
#include <modem/modem_key_mgmt.h>

#include "met_no_ca.h"
#include "weather.h"

LOG_MODULE_REGISTER(weather, LOG_LEVEL_INF);

#define HOST    "api.met.no"
/* MET Norway asks for at most four decimals */
#define LATITUDE  "59.9365"
#define LONGITUDE "10.7707"
/* MET Norway requires an identifying User-Agent */
#define USER_AGENT "obegraensad-nrf9151dk github.com/coderbyheart/IKEA-OBEGRANSAD-nRF9151-DK"

#define SEC_TAG 42

#define UPDATE_INTERVAL K_MINUTES(30)
#define RETRY_INTERVAL  K_MINUTES(1)

/* The first time series entry is within the first kilobyte of the body, the rest is ignored */
static char response[2048];

static atomic_t connected;
static struct weather latest;
static bool have_latest;
static K_MUTEX_DEFINE(lock);

int weather_init(void)
{
	bool exists;
	int err = modem_key_mgmt_exists(SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, &exists);

	if (err) {
		return err;
	}
	if (exists && modem_key_mgmt_cmp(SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, met_no_ca,
					 sizeof(met_no_ca) - 1) == 0) {
		return 0;
	}

	LOG_INF("Provisioning certificate");
	return modem_key_mgmt_write(SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, met_no_ca,
				    sizeof(met_no_ca) - 1);
}

void weather_set_connected(bool value)
{
	atomic_set(&connected, value);
}

bool weather_get(struct weather *out)
{
	bool valid;

	k_mutex_lock(&lock, K_FOREVER);
	valid = have_latest;
	*out = latest;
	k_mutex_unlock(&lock);

	return valid;
}

static enum weather_icon icon_from_symbol(const char *symbol, size_t len)
{
	char code[32];
	bool night;

	snprintf(code, sizeof(code), "%.*s", (int)len, symbol);
	night = strstr(code, "_night") != NULL;

	/* Symbol codes are described at https://api.met.no/weatherapi/weathericon/2.0/documentation */
	if (strstr(code, "thunder")) {
		return WEATHER_THUNDER;
	} else if (strstr(code, "snow") || strstr(code, "sleet")) {
		return WEATHER_SNOW;
	} else if (strstr(code, "rain")) {
		return WEATHER_RAIN;
	} else if (strstr(code, "fog")) {
		return WEATHER_FOG;
	} else if (strstr(code, "partlycloudy") || strstr(code, "fair")) {
		return night ? WEATHER_CLEAR_NIGHT : WEATHER_PARTLY_CLOUDY;
	} else if (strstr(code, "clearsky")) {
		return night ? WEATHER_CLEAR_NIGHT : WEATHER_CLEAR_DAY;
	}

	return WEATHER_CLOUDY;
}

/* Temperature with one decimal (e.g. -0.4), rounded to whole degrees */
static bool parse_temperature(const char *p, int *out)
{
	bool negative = *p == '-';
	int whole = 0;
	bool digits = false;

	if (negative) {
		p++;
	}
	for (; *p >= '0' && *p <= '9'; p++) {
		whole = whole * 10 + (*p - '0');
		digits = true;
	}
	if (!digits) {
		return false;
	}
	if (*p == '.' && p[1] >= '5' && p[1] <= '9') {
		whole++;
	}
	*out = negative ? -whole : whole;

	return true;
}

/* The forecast for the current hour is the first entry of the time series */
static int parse(const char *body, struct weather *out)
{
	const char *series = strstr(body, "\"timeseries\"");
	const char *temp = series ? strstr(series, "\"air_temperature\":") : NULL;
	const char *next = series ? strstr(series, "\"next_1_hours\"") : NULL;
	const char *symbol = next ? strstr(next, "\"symbol_code\":\"") : NULL;
	const char *end;

	if (!temp || !symbol) {
		return -ENOMSG;
	}
	if (!parse_temperature(temp + strlen("\"air_temperature\":"), &out->temperature)) {
		return -EBADMSG;
	}
	symbol += strlen("\"symbol_code\":\"");
	end = strchr(symbol, '"');
	if (!end) {
		return -EBADMSG;
	}
	out->icon = icon_from_symbol(symbol, end - symbol);

	return 0;
}

static int send_all(int fd, const char *data, size_t len)
{
	while (len > 0) {
		ssize_t n = zsock_send(fd, data, len, 0);

		if (n < 0) {
			return -errno;
		}
		data += n;
		len -= n;
	}

	return 0;
}

static int fetch(struct weather *out)
{
	static const char request[] =
		"GET /weatherapi/locationforecast/2.0/compact?lat=" LATITUDE "&lon=" LONGITUDE
		" HTTP/1.1\r\n"
		"Host: " HOST "\r\n"
		"User-Agent: " USER_AGENT "\r\n"
		/* The modem fails to receive large TLS records, ask for the start only */
		"Range: bytes=0-1499\r\n"
		"Connection: close\r\n\r\n";
	struct zsock_addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
	struct zsock_addrinfo *addr;
	nrf_sec_tag_t tags[] = {SEC_TAG};
	int verify = TLS_PEER_VERIFY_REQUIRED;
	size_t used = 0;
	char *body;
	int fd, err;

	err = zsock_getaddrinfo(HOST, "443", &hints, &addr);
	if (err) {
		LOG_ERR("DNS lookup failed: %d", err);
		return -EHOSTUNREACH;
	}
	((struct sockaddr_in *)addr->ai_addr)->sin_port = htons(443);

	fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
	if (fd < 0) {
		err = -errno;
		zsock_freeaddrinfo(addr);
		return err;
	}

	if (zsock_setsockopt(fd, SOL_TLS, TLS_SEC_TAG_LIST, tags, sizeof(tags)) ||
	    zsock_setsockopt(fd, SOL_TLS, TLS_HOSTNAME, HOST, sizeof(HOST) - 1) ||
	    zsock_setsockopt(fd, SOL_TLS, TLS_PEER_VERIFY, &verify, sizeof(verify))) {
		err = -errno;
		LOG_ERR("TLS setup failed: %d", err);
		goto out;
	}

	err = zsock_connect(fd, addr->ai_addr, addr->ai_addrlen) ? -errno : 0;
	if (err) {
		LOG_ERR("Connect failed: %d", err);
		goto out;
	}

	err = send_all(fd, request, sizeof(request) - 1);
	if (err) {
		LOG_ERR("Send failed: %d", err);
		goto out;
	}

	/* Only the beginning of the (large) response is needed */
	while (used < sizeof(response) - 1) {
		ssize_t n = zsock_recv(fd, response + used, MIN(sizeof(response) - 1 - used, 512), 0);

		if (n < 0) {
			err = -errno;
			LOG_ERR("Receive failed: %d", err);
			goto out;
		}
		if (n == 0) {
			break;
		}
		used += n;
	}
	response[used] = '\0';

	if (strncmp(response, "HTTP/1.1 200", strlen("HTTP/1.1 200")) != 0 &&
	    strncmp(response, "HTTP/1.1 206", strlen("HTTP/1.1 206")) != 0) {
		LOG_ERR("Unexpected response: %.12s", response);
		err = -EPROTO;
		goto out;
	}
	body = strstr(response, "\r\n\r\n");
	err = body ? parse(body, out) : -EBADMSG;
	if (err) {
		LOG_ERR("Could not parse the forecast: %d", err);
	}

out:
	zsock_close(fd);
	zsock_freeaddrinfo(addr);

	return err;
}

static void weather_thread(void)
{
	for (;;) {
		struct weather w;
		int err;

		while (!atomic_get(&connected)) {
			k_sleep(K_SECONDS(5));
		}

		err = fetch(&w);
		if (err == 0) {
			LOG_INF("Forecast: %d degrees, icon %d", w.temperature, w.icon);
			k_mutex_lock(&lock, K_FOREVER);
			latest = w;
			have_latest = true;
			k_mutex_unlock(&lock);
		}

		k_sleep(err ? RETRY_INTERVAL : UPDATE_INTERVAL);
	}
}

K_THREAD_DEFINE(weather_tid, 4096, weather_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(10), 0, 0);
