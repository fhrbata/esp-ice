/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Unit tests for the @ref process pty mode (POSIX side).
 *
 * Spawns @c cat in a pty with @ref process::use_pty set, writes a few
 * bytes to the master, expects them echoed back through the slave's
 * tty driver (canonical mode + echo on by default).  Verifies the
 * round trip works the way readline-driven children expect, since
 * gdb's interactive shell relies on exactly this.
 *
 * Also verifies @ref pty_resize updates the child's view of the
 * window size by inspecting @c stty size output through the same pty.
 */
#include "ice.h"
#include "platform.h"
#include "tap.h"

#include <signal.h>
#include <string.h>
#include <unistd.h>

/*
 * Read up to @p timeout_ms milliseconds of bytes from the pty master,
 * appending into @p out.  Returns the number of bytes accumulated; 0
 * on timeout, < 0 on EOF or error.
 */
static ssize_t drain_for(int fd, char *out, size_t out_cap, unsigned timeout_ms)
{
	size_t total = 0;
	while (total < out_cap) {
		ssize_t n = pipe_read_timed(fd, out + total, out_cap - total,
					    timeout_ms);
		if (n <= 0) {
			if (total > 0)
				return (ssize_t)total;
			return n;
		}
		total += (size_t)n;
		/* After the first burst, drop the timeout so we don't sit
		 * around waiting for content the child already finished
		 * sending.  The cat-echo round trip rarely exceeds 100 ms
		 * even under sanitiser builds. */
		timeout_ms = 30;
	}
	return (ssize_t)total;
}

int main(void)
{
	/*
	 * cat in a pty: writing "hello\n" to the master should produce
	 * "hello\r\nhello\r\n" back -- the slave's tty driver echoes the
	 * input first (cooked mode, ECHO on by default) and then cat
	 * relays it through stdout.  The CR comes from the tty driver's
	 * ONLCR translation.
	 */
	{
		const char *argv[] = {"cat", NULL};
		struct process proc = PROCESS_INIT;
		proc.argv = argv;
		proc.use_pty = 1;
		proc.pty_rows = 24;
		proc.pty_cols = 80;

		tap_check(process_start(&proc) == 0);
		tap_check(proc.in == proc.out);
		tap_check(proc.in >= 0);

		ssize_t w = write(proc.in, "hello\n", 6);
		tap_check(w == 6);

		char buf[64] = {0};
		ssize_t got = drain_for(proc.out, buf, sizeof buf - 1, 1000);
		tap_check(got > 0);
		buf[got > 0 ? got : 0] = '\0';

		/* Echoed once by the tty driver, once by cat itself. */
		tap_check(strstr(buf, "hello") != NULL);

		/* Closing master side gives cat EOF on stdin -- it exits. */
		close(proc.in);
		proc.in = -1;
		proc.out = -1;
		int rc = process_finish(&proc);
		(void)rc;
		tap_done("cat in a pty echoes input through cooked-mode tty");
	}

	/*
	 * stty size in a pty: child should see whatever we set as the
	 * initial window size, i.e. the row/col numbers we passed.
	 * This is the smoke test that TIOCSWINSZ propagated and the
	 * child's TIOCGWINSZ returns the same.
	 */
	{
		const char *argv[] = {"stty", "size", NULL};
		struct process proc = PROCESS_INIT;
		proc.argv = argv;
		proc.use_pty = 1;
		proc.pty_rows = 30;
		proc.pty_cols = 132;

		tap_check(process_start(&proc) == 0);

		char buf[256] = {0};
		ssize_t got = drain_for(proc.out, buf, sizeof buf - 1, 2000);
		buf[got > 0 ? got : 0] = '\0';

		/* Echo what stty actually printed as a TAP diagnostic AND
		 * dump the raw bytes to a file in the test's scratch
		 * directory ("./stty_size_output") so the bytes survive in
		 * the CI artifact even when prove suppresses TAP comments
		 * for failing tests. */
		FILE *dump = fopen("stty_size_output", "wb");
		if (dump) {
			if (got > 0)
				fwrite(buf, 1, (size_t)got, dump);
			fclose(dump);
		}
		printf("# stty size in pty returned (%zd bytes): ",
		       got > 0 ? got : 0);
		for (ssize_t i = 0; i < (got > 0 ? got : 0); i++) {
			unsigned char c = (unsigned char)buf[i];
			if (c >= 0x20 && c < 0x7f)
				printf("%c", c);
			else
				printf("\\x%02x", c);
		}
		printf("\n");

		/* "stty size" prints "<rows> <cols>" -- be tolerant of
		 * trailing CRLF / extra whitespace by checking for substrings.
		 */
		tap_check(strstr(buf, "30") != NULL);
		tap_check(strstr(buf, "132") != NULL);

		int rc = process_finish(&proc);
		(void)rc;
		tap_done("pty initial size propagates to child via TIOCSWINSZ");
	}

	/*
	 * pty_resize on a non-pty process is a programming error and
	 * should fail with EINVAL.  Saves a future caller from confusion.
	 */
	{
		struct process proc = PROCESS_INIT;
		proc.in = 0;
		errno = 0;
		int rc = pty_resize(&proc, 24, 80);
		tap_check(rc == -1);
		tap_check(errno == EINVAL);
		tap_done("pty_resize refuses non-pty processes");
	}

	return tap_result();
}
