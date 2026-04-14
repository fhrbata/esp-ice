/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file http.h
 * @brief Simple HTTP/HTTPS client API.
 *
 * Provides minimal API for downloading files and fetching URLs.
 * Implementation uses libcurl with platform-native TLS backends.
 *
 * Usage:
 *   // Download with progress bar
 *   http_download(url, path, http_default_progress, NULL);
 *
 *   // Download silently
 *   http_download(url, path, NULL, NULL);
 *
 *   // Fetch into memory
 *   struct sbuf body = SBUF_INIT;
 *   int status = http_get("https://api.example.com/v1/info", &body);
 *   if (status == 200)
 *       printf("%s\n", body.buf);
 *   sbuf_release(&body);
 */
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

struct sbuf;

/**
 * @brief Progress callback for http_download.
 *
 * @param total  Total bytes expected (0 if unknown).
 * @param now    Bytes downloaded so far.
 * @param ctx    Opaque context passed to http_download.
 */
typedef void (*http_progress_fn)(size_t total, size_t now, void *ctx);

/**
 * @brief Default progress bar printed to stderr.
 *
 * Shows percentage and a simple bar. Pass as the progress argument
 * to http_download for standard progress output.
 */
void http_default_progress(size_t total, size_t now, void *ctx);

/**
 * @brief Download a URL to a file.
 *
 * @param url       HTTPS or HTTP URL.
 * @param path      Destination file path.
 * @param progress  Progress callback, or NULL for silent download.
 * @param ctx       Opaque context passed to progress callback.
 * @return 0 on success, -1 on error.
 */
int http_download(const char *url, const char *path, http_progress_fn progress,
		  void *ctx);

/**
 * @brief Fetch a URL into memory.
 *
 * Appends the response body to @p body. The caller should
 * initialize body with SBUF_INIT before calling.
 *
 * @param url   HTTPS or HTTP URL.
 * @param body  Destination sbuf for response body.
 * @return HTTP status code (e.g. 200) on success, -1 on error.
 */
int http_get(const char *url, struct sbuf *body);

#endif /* HTTP_H */
