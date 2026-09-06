/*
 * A-GNSS assistance data via nRF Cloud REST API.
 *
 * On cold start the modem fires NRF_MODEM_GNSS_EVT_AGNSS_REQ describing what
 * ephemeris/almanac/timing data it needs.  This module fetches that data from
 * nRF Cloud using the REST API, authenticated with a per-device JWT signed by
 * the key at CONFIG_NRF_CLOUD_SEC_TAG (the device must be onboarded to the
 * nRF Cloud account), then injects it via nrf_cloud_agnss_process().
 *
 * The nRF Cloud CA certificate (Amazon Root CA 1) is provisioned into the
 * modem on first boot at NRF_CLOUD_SEC_TAG.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <modem/modem_key_mgmt.h>
#include <modem/modem_info.h>
#include <modem/modem_jwt.h>
#include <net/nrf_cloud_rest.h>
#include <net/nrf_cloud_agnss.h>

#include "app.h"

LOG_MODULE_REGISTER(agnss, CONFIG_APP_LOG_LEVEL);

#define NRF_CLOUD_SEC_TAG CONFIG_NRF_CLOUD_SEC_TAG

/* Amazon Root CA 1 — used by api.nrfcloud.com */
static const char ca_cert[] =
	"-----BEGIN CERTIFICATE-----\n"
	"MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF\n"
	"ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6\n"
	"b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL\n"
	"MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv\n"
	"b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj\n"
	"ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM\n"
	"9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw\n"
	"IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6\n"
	"VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L\n"
	"93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm\n"
	"jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC\n"
	"AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA\n"
	"A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI\n"
	"U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs\n"
	"N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv\n"
	"o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU\n"
	"5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy\n"
	"rqXRfboQnoZsG4q5WTP468SQvvG5\n"
	"-----END CERTIFICATE-----\n";

static char jwt_buf[600];
static char rx_buf[2048];
static char agnss_data_buf[NRF_CLOUD_AGNSS_MAX_DATA_SIZE];

static int provision_ca(void)
{
	int err;
	bool exists;

	err = modem_key_mgmt_exists(NRF_CLOUD_SEC_TAG,
				    MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
				    &exists);
	if (err) {
		LOG_ERR("key_mgmt_exists: %d", err);
		return err;
	}
	if (exists) {
		return 0;
	}

	err = modem_key_mgmt_write(NRF_CLOUD_SEC_TAG,
				   MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
				   ca_cert, sizeof(ca_cert) - 1);
	if (err) {
		LOG_ERR("key_mgmt_write: %d", err);
		return err;
	}
	LOG_INF("nRF Cloud CA provisioned (sec_tag %d)", NRF_CLOUD_SEC_TAG);
	return 0;
}

static int get_serving_cell(struct lte_lc_cell *cell)
{
	int err;
	char buf[MODEM_INFO_MAX_RESPONSE_SIZE];

	err = modem_info_string_get(MODEM_INFO_CELLID, buf, sizeof(buf));
	if (err < 0) return err;
	cell->id = strtol(buf, NULL, 16);

	err = modem_info_string_get(MODEM_INFO_AREA_CODE, buf, sizeof(buf));
	if (err < 0) return err;
	cell->tac = strtol(buf, NULL, 16);

	err = modem_info_string_get(MODEM_INFO_OPERATOR, buf, sizeof(buf));
	if (err < 0) return err;
	cell->mnc = strtol(&buf[3], NULL, 10);
	buf[3] = '\0';
	cell->mcc = strtol(buf, NULL, 10);

	return 0;
}

int agnss_init(void)
{
	int err = modem_info_init();
	if (err) {
		LOG_ERR("modem_info_init: %d", err);
		return err;
	}
	return provision_ca();
}

int agnss_fetch(void *agnss_request)
{
	struct nrf_modem_gnss_agnss_data_frame *request = agnss_request;

	/* JWT needs a valid modem clock for the expiry field.  The date_time
	 * library auto-triggers on LTE connect: tries NITZ first, falls back
	 * to NTP, and pushes the result to the modem via AT+CCLK. */
	int err = nrf_cloud_jwt_generate(0, jwt_buf, sizeof(jwt_buf));
	if (err) {
		/* Expected once per boot: the clock is not set until NITZ or
		 * NTP has run, and the retry below covers it. */
		LOG_INF("JWT needs modem time; waiting for NTP fallback...");
		for (int i = 0; i < 15; i++) {
			k_msleep(1000);
			err = nrf_cloud_jwt_generate(0, jwt_buf, sizeof(jwt_buf));
			if (!err) break;
		}
		if (err) {
			LOG_ERR("JWT generate failed: %d", err);
			return err;
		}
	}

	struct nrf_cloud_rest_context rest_ctx = {
		.connect_socket = -1,
		.keep_alive = false,
		.timeout_ms = 30000,
		.auth = jwt_buf,
		.rx_buf = rx_buf,
		.rx_buf_len = sizeof(rx_buf),
		.fragment_size = 0,
	};

	struct nrf_cloud_rest_agnss_request req = {
		.type = request ? NRF_CLOUD_REST_AGNSS_REQ_CUSTOM
				: NRF_CLOUD_REST_AGNSS_REQ_ASSISTANCE,
		.agnss_req = request,
		.net_info = NULL,
		.mask_angle = NRF_CLOUD_AGNSS_MASK_ANGLE_NONE,
	};

	struct nrf_cloud_rest_agnss_result result = {
		.buf = agnss_data_buf,
		.buf_sz = sizeof(agnss_data_buf),
	};

	struct lte_lc_cells_info net_info = { 0 };
	if (get_serving_cell(&net_info.current_cell) == 0) {
		req.net_info = &net_info;
	}

	LOG_INF("requesting A-GNSS data from nRF Cloud...");

	/* GNSS and LTE share one RF front-end on the nRF91, so they cannot both
	 * have the radio.  A cold GNSS search wants long uninterrupted windows,
	 * which leaves a TLS handshake plus a multi-kilobyte download very
	 * little airtime — that is how this ends up timing out at 30 s and
	 * returning HTTP 0, having never reached the server at all.
	 *
	 * So hand the radio to LTE for the download.  gnss_stop() only reports
	 * success if the receiver was actually running, which is how the
	 * boot-time call (GNSS not started yet) knows to leave it alone.
	 * gnss_resume() rather than gnss_start(): the latter clears the
	 * have-fix state, which would turn a warm receiver cold. */
	bool paused = (gnss_stop() == 0);

	if (paused) {
		LOG_INF("GNSS paused for the download (shared RF front-end)");
	}

	err = nrf_cloud_rest_agnss_data_get(&rest_ctx, &req, &result);

	if (paused) {
		gnss_resume();
	}
	if (err) {
		LOG_ERR("agnss_data_get: %d (HTTP %d)", err, rest_ctx.status);
		return err;
	}

	LOG_INF("received %u bytes, processing", (unsigned)result.agnss_sz);

	err = nrf_cloud_agnss_process(result.buf, result.agnss_sz);
	if (err) {
		LOG_ERR("agnss_process: %d", err);
		return err;
	}

	LOG_INF("A-GNSS data injected");
	return 0;
}
