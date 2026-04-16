/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file http.c
 * @brief HTTP/HTTPS client implementation using libcurl.
 *
 * libcurl handles platform differences at link time:
 *  - Linux: OpenSSL (statically linked)
 *  - macOS: SecureTransport
 *  - Windows: Schannel (via MinGW/MSYS2)
 */
#include <curl/curl.h>

#include "ice.h"

/**
 * @brief Context for http_download's curl callbacks.
 */
struct download_ctx {
	http_progress_fn progress;
	void *user_ctx;
};

/**
 * @brief libcurl write callback that appends data to an sbuf.
 */
static size_t write_to_sbuf(void *data, size_t size, size_t nmemb, void *userp)
{
	struct sbuf *sb = userp;
	size_t bytes = size * nmemb;

	sbuf_add(sb, data, bytes);
	return bytes;
}

/**
 * @brief libcurl progress callback that delegates to the user's
 * progress function.
 */
static int curl_progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
			    curl_off_t ultotal, curl_off_t ulnow)
{
	struct download_ctx *ctx = clientp;

	(void)ultotal;
	(void)ulnow;

	if (ctx->progress)
		ctx->progress((size_t)dltotal, (size_t)dlnow, ctx->user_ctx);

	return 0;
}

/**
 * @brief Set common curl options (timeouts, redirects, errors).
 */
static void curl_set_defaults(CURL *curl, const char *url)
{
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "esp-ice/" VERSION);
}

void http_default_progress(size_t total, size_t now, void *ctx)
{
	(void)ctx;

	if (!total)
		fprintf(stderr, "\r  %zu bytes", now);
	else
		fprintf(stderr, "\r  %d%%  %zu / %zu bytes",
			(int)(now * 100 / total), now, total);
}

int http_download(const char *url, const char *path, http_progress_fn progress,
		  void *ctx)
{
	CURL *curl;
	FILE *fp;
	CURLcode res;
	struct download_ctx dl_ctx = {progress, ctx};

	fp = fopen(path, "wb");
	if (!fp) {
		err_errno("cannot create '%s'", path);
		return -1;
	}

	curl = curl_easy_init();
	if (!curl) {
		fclose(fp);
		err("curl_easy_init failed");
		return -1;
	}

	curl_set_defaults(curl, url);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);

	if (progress) {
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
				 curl_progress_cb);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &dl_ctx);
	}

	res = curl_easy_perform(curl);

	curl_easy_cleanup(curl);
	fclose(fp);

	if (res != CURLE_OK) {
		err("download failed: %s: %s", url, curl_easy_strerror(res));
		return -1;
	}

	return 0;
}

int http_get(const char *url, struct sbuf *body)
{
	CURL *curl;
	CURLcode res;
	long status;

	curl = curl_easy_init();
	if (!curl) {
		err("curl_easy_init failed");
		return -1;
	}

	curl_set_defaults(curl, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_sbuf);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);

	res = curl_easy_perform(curl);

	if (res != CURLE_OK) {
		err("HTTP request failed: %s: %s", url,
		    curl_easy_strerror(res));
		curl_easy_cleanup(curl);
		return -1;
	}

	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);

	return (int)status;
}
