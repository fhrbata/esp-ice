/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file fs.c
 * @brief Portable filesystem helpers (mkdirp, rmtree).
 */
#include "fs.h"
#include "ice.h"

#include <fcntl.h>
#include <signal.h>

int mkdirp(const char *dir)
{
	struct sbuf sb = SBUF_INIT;
	char *p;
	int rc = 0;

	if (!dir || !*dir)
		return 0;

	sbuf_addstr(&sb, dir);

	/*
	 * Find the first "creatable" position.  Drive prefixes like "C:"
	 * on Windows and a leading root separator cannot be mkdir()'d --
	 * skip past them so we never call mkdir("C:") or mkdir("").
	 */
	p = sb.buf;
#ifdef _WIN32
	if (sb.len >= 2 && isalpha((unsigned char)p[0]) && p[1] == ':')
		p += 2;
#endif
	if (*p == '/' || *p == '\\')
		p++;

	for (; *p; p++) {
		if (*p != '/' && *p != '\\')
			continue;
		char c = *p;
		*p = '\0';
		if (mkdir(sb.buf, 0755) != 0 && errno != EEXIST) {
			*p = c;
			rc = -1;
			goto done;
		}
		*p = c;
	}

	if (mkdir(sb.buf, 0755) != 0 && errno != EEXIST)
		rc = -1;

done:
	sbuf_release(&sb);
	return rc;
}

int mkdirp_for_file(const char *path)
{
	struct sbuf dir = SBUF_INIT;
	const char *sep;
	const char *bs;
	int rc = 0;

	sep = strrchr(path, '/');
	bs = strrchr(path, '\\');
	if (bs && (!sep || bs > sep))
		sep = bs;

	if (!sep || sep == path)
		return 0;

	sbuf_add(&dir, path, sep - path);
	rc = mkdirp(dir.buf);
	sbuf_release(&dir);
	return rc;
}

int write_file_atomic(const char *path, const void *data, size_t len)
{
	struct sbuf tmp = SBUF_INIT;
	FILE *fp;
	int rc = -1;

	sbuf_addf(&tmp, "%s.tmp", path);

	fp = fopen(tmp.buf, "wb");
	if (!fp)
		goto done;
	if (fwrite(data, 1, len, fp) != len) {
		fclose(fp);
		goto unlink_tmp;
	}
	if (fclose(fp) != 0)
		goto unlink_tmp;

	if (rename(tmp.buf, path) != 0)
		goto unlink_tmp;

	rc = 0;
	goto done;

unlink_tmp:
	unlink(tmp.buf);
done:
	sbuf_release(&tmp);
	return rc;
}

struct rmtree_ctx {
	const char *path;
	int verbose;
	int rc;
};

static int rmtree_cb(const char *name, void *ud)
{
	struct rmtree_ctx *ctx = ud;
	struct sbuf child = SBUF_INIT;

	sbuf_addf(&child, "%s/%s", ctx->path, name);

	if (ctx->verbose)
		printf("Removing: %s\n", child.buf);

	if (is_directory(child.buf)) {
		if (rmtree(child.buf, ctx->verbose) < 0)
			ctx->rc = -1;
		if (rmdir(child.buf) < 0) {
			warn_errno("cannot remove '%s'", child.buf);
			ctx->rc = -1;
		}
	} else if (unlink(child.buf) < 0) {
		warn_errno("cannot remove '%s'", child.buf);
		ctx->rc = -1;
	}

	sbuf_release(&child);
	return 0;
}

int rmtree(const char *path, int verbose)
{
	struct rmtree_ctx ctx = {.path = path, .verbose = verbose, .rc = 0};

	if (dir_foreach(path, rmtree_cb, &ctx) < 0) {
		warn_errno("cannot open '%s'", path);
		return -1;
	}
	return ctx.rc;
}

/* ------------------------------------------------------------------ */
/* Lockfiles                                                           */
/* ------------------------------------------------------------------ */

/*
 * One process never holds more than a handful of locks, so a tiny
 * fixed-size registry avoids pulling in svec.  Paths are duplicated
 * on acquire and freed on release (or at exit).
 */
#define LOCK_MAX 4
#define LOCK_POLL_MS 100
static char *held_locks[LOCK_MAX];
static size_t held_locks_nr;
static int lock_handlers_registered;

static void lock_atexit_cleanup(void)
{
	for (size_t i = 0; i < held_locks_nr; i++) {
		unlink(held_locks[i]);
		free(held_locks[i]);
		held_locks[i] = NULL;
	}
	held_locks_nr = 0;
}

/*
 * Called from a signal handler -- must stay async-signal-safe.
 *
 * POSIX: unlink(2) is on the async-signal-safe list; free() is not,
 * so we leak the strdup'd paths (the process is terminating anyway).
 *
 * Windows: unlink macro-expands to unlink_w (mbs_to_wcs + _wunlink +
 * free), which isn't strictly AS-safe on paper because of the
 * internal malloc/free.  In practice this doesn't matter: MSVCRT
 * delivers SIGINT on a brand-new thread -- not on the interrupted
 * thread's stack -- so the usual "handler re-entering a half-done
 * malloc" hazard that motivates the POSIX AS-safe list doesn't
 * apply.  Same tradeoff lock_acquire() already documents.  The
 * NOLINT below silences bugprone-signal-handler, which (correctly)
 * can't see past the platform macro to know it's intentional.
 */
static void lock_signal_cleanup(int sig)
{
	for (size_t i = 0; i < held_locks_nr; i++)
		/* NOLINTNEXTLINE(bugprone-signal-handler) */
		unlink(held_locks[i]);
	_exit(128 + sig);
}

int lock_acquire(const char *path, unsigned timeout_ms)
{
	unsigned long long deadline = 0;
	int fd;

	if (held_locks_nr >= LOCK_MAX) {
		errno = EMFILE;
		return -1;
	}

	if (mkdirp_for_file(path) < 0)
		return -1;

	if (timeout_ms)
		deadline = mono_ms() + timeout_ms;

	for (;;) {
		fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
		if (fd >= 0)
			break;
		if (errno != EEXIST || !timeout_ms)
			return -1;
		if (mono_ms() >= deadline) {
			errno = EEXIST;
			return -1;
		}
		delay_ms(LOCK_POLL_MS);
	}
	close(fd);

	if (!lock_handlers_registered) {
		atexit(lock_atexit_cleanup);
		/*
		 * Best-effort: SIGINT and SIGTERM are ISO C, defined on
		 * POSIX and Windows.  Windows' SIGINT delivery via the
		 * MSVCRT runs on a separate thread and is timing-dependent,
		 * so a Ctrl-C on Windows may still leave a stale lock --
		 * same tradeoff git makes.  Users see the clear
		 * "remove the lock if no ice is running" hint either way.
		 */
		signal(SIGINT, lock_signal_cleanup);
		signal(SIGTERM, lock_signal_cleanup);
		lock_handlers_registered = 1;
	}
	held_locks[held_locks_nr++] = sbuf_strdup(path);
	return 0;
}

void lock_release(const char *path)
{
	for (size_t i = 0; i < held_locks_nr; i++) {
		if (!strcmp(held_locks[i], path)) {
			unlink(path);
			free(held_locks[i]);
			held_locks[i] = held_locks[--held_locks_nr];
			held_locks[held_locks_nr] = NULL;
			return;
		}
	}
}

void lock_forget(const char *path)
{
	for (size_t i = 0; i < held_locks_nr; i++) {
		if (!strcmp(held_locks[i], path)) {
			free(held_locks[i]);
			held_locks[i] = held_locks[--held_locks_nr];
			held_locks[held_locks_nr] = NULL;
			return;
		}
	}
}

/*
 * On Windows the executable check is "does a matching file exist",
 * since _access() doesn't implement X_OK.  We try the bare name and a
 * small set of common PATHEXT extensions; that covers ~everything
 * users put in core.pager / ice-<name> in practice.  On POSIX we need
 * X_OK (execute bit) -- bare existence isn't enough.
 */
#ifdef _WIN32
static const char *const path_exts[] = {"", ".exe", ".cmd", ".bat", NULL};
#define ACCESS_MODE F_OK
#else
static const char *const path_exts[] = {"", NULL};
#define ACCESS_MODE X_OK
#endif

int find_in_path(const char *name)
{
	const char *p = getenv("PATH");
	struct sbuf buf = SBUF_INIT;

	if (!p || !*p)
		return 0;

	while (*p) {
		const char *end = p;
		while (*end && *end != PATH_SEP_CHAR)
			end++;

		for (int i = 0; path_exts[i]; i++) {
			sbuf_reset(&buf);
			if (end != p) {
				sbuf_add(&buf, p, (size_t)(end - p));
				sbuf_addch(&buf, '/');
			}
			sbuf_addstr(&buf, name);
			sbuf_addstr(&buf, path_exts[i]);
			if (!access(buf.buf, ACCESS_MODE)) {
				sbuf_release(&buf);
				return 1;
			}
		}

		if (!*end)
			break;
		p = end + 1;
	}

	sbuf_release(&buf);
	return 0;
}
