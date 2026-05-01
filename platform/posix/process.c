/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file platform/posix/process.c
 * @brief POSIX child process API and self-path resolution.
 *
 * process_start() / process_finish() use fork()/execvp()/waitpid()
 * with optional pipe redirection.  process_exe() resolves the path of
 * the running binary via /proc/self/exe (Linux) or
 * _NSGetExecutablePath (macOS), caching the result on first call.
 */
/* Pull in X/Open extensions (posix_openpt, grantpt, ptsname) which the
 * build's @c -D_POSIX_C_SOURCE=200112L doesn't fully expose under
 * glibc.  Must be set before any system header is included. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif

#include "ice.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

const char *process_exe(void)
{
	static char buf[4096];
	static const char *result;
	static int initialized;

	if (initialized)
		return result;
	initialized = 1;

#ifdef __APPLE__
	uint32_t size = (uint32_t)sizeof(buf);
	if (_NSGetExecutablePath(buf, &size) == 0)
		result = buf;
#else
	ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (n > 0) {
		buf[n] = '\0';
		result = buf;
	}
#endif

	return result;
}

/**
 * @brief Start a child process (POSIX).
 *
 * Creates pipes for any requested redirections, then forks. The child
 * wires up the pipe ends with dup2() and exec's argv[0].
 */
/*
 * Open a POSIX pseudo-terminal master.  Returns the master fd; the
 * slave path is written to @p slave_path (caller-owned, big enough).
 * On error returns -1 with errno set.  The pty mode requested is the
 * usual "raw on the slave-side, child sees a real tty" -- callers
 * driving readline-y things (gdb, vim, ssh -t) want this.
 */
static int pty_open_master(char *slave_path, size_t slave_len)
{
	int master = posix_openpt(O_RDWR | O_NOCTTY);
	if (master < 0)
		return -1;
	if (grantpt(master) < 0 || unlockpt(master) < 0) {
		close(master);
		return -1;
	}
	const char *name = ptsname(master);
	if (!name) {
		close(master);
		return -1;
	}
	if (strlen(name) >= slave_len) {
		close(master);
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(slave_path, name);
	return master;
}

int process_start(struct process *proc)
{
	int pipe_in[2] = {-1, -1};
	int pipe_out[2] = {-1, -1};
	int pipe_err[2] = {-1, -1};
	int pty_master = -1;
	char pty_slave_path[256] = {0};

	if (proc->use_pty) {
		pty_master =
		    pty_open_master(pty_slave_path, sizeof pty_slave_path);
		if (pty_master < 0) {
			err_errno("posix_openpt");
			return -1;
		}
	} else {
		if (proc->pipe_in && pipe(pipe_in) == -1) {
			err_errno("pipe");
			return -1;
		}
		if (proc->pipe_out && pipe(pipe_out) == -1) {
			err_errno("pipe");
			goto err_close;
		}
		if (proc->pipe_err && pipe(pipe_err) == -1) {
			err_errno("pipe");
			goto err_close;
		}
	}

	proc->pid = fork();
	if (proc->pid == -1) {
		err_errno("fork");
		goto err_close;
	}

	/* Child */
	if (proc->pid == 0) {
		if (proc->use_pty) {
			/* Detach from the parent's controlling tty so the
			 * pty slave can take over as the new controlling
			 * tty.  Then open the slave, set winsize if asked,
			 * and dup it over stdio. */
			setsid();
			int slave = open(pty_slave_path, O_RDWR);
			if (slave < 0) {
				err_errno("open(pty slave): '%s'",
					  pty_slave_path);
				_exit(EXIT_FAILURE);
			}
			ioctl(slave, TIOCSCTTY, 0);
			if (proc->pty_rows > 0 && proc->pty_cols > 0) {
				struct winsize ws;

				memset(&ws, 0, sizeof ws);
				ws.ws_row = (unsigned short)proc->pty_rows;
				ws.ws_col = (unsigned short)proc->pty_cols;
				ioctl(slave, TIOCSWINSZ, &ws);
			}
			dup2(slave, STDIN_FILENO);
			dup2(slave, STDOUT_FILENO);
			dup2(slave, STDERR_FILENO);
			if (slave > STDERR_FILENO)
				close(slave);
			close(pty_master);
		} else {
			if (proc->pipe_in) {
				close(pipe_in[1]);
				dup2(pipe_in[0], STDIN_FILENO);
				close(pipe_in[0]);
			}
			if (proc->pipe_out) {
				close(pipe_out[0]);
				dup2(pipe_out[1], STDOUT_FILENO);
				close(pipe_out[1]);
			}
			if (proc->pipe_err) {
				close(pipe_err[0]);
				dup2(pipe_err[1], STDERR_FILENO);
				close(pipe_err[1]);
			}

			if (proc->merge_err)
				dup2(STDOUT_FILENO, STDERR_FILENO);
		}

		if (proc->env) {
			for (int i = 0; proc->env[i]; i++) {
				const char *kv = proc->env[i];
				const char *eq = strchr(kv, '=');
				char name[256];
				size_t nlen;

				if (!eq)
					continue;
				nlen = (size_t)(eq - kv);
				if (nlen >= sizeof(name))
					continue;
				memcpy(name, kv, nlen);
				name[nlen] = '\0';
				setenv(name, eq + 1, 1);
			}
		}

		if (proc->dir && chdir(proc->dir) == -1) {
			err_errno("chdir: '%s'", proc->dir);
			_exit(EXIT_FAILURE);
		}

		if (proc->use_shell) {
			const char *sh_argv[] = {"/bin/sh", "-c", proc->argv[0],
						 NULL};
			execvp(sh_argv[0], (char *const *)sh_argv);
			err_errno("execvp: '%s'", sh_argv[0]);
		} else {
			execvp(proc->argv[0], (char *const *)proc->argv);
			err_errno("execvp: '%s'", proc->argv[0]);
		}
		_exit(127);
	}

	/* Parent: keep our ends, close child's ends */
	if (proc->use_pty) {
		/* The master fd is bidirectional -- alias it to both in and
		 * out so callers can use the familiar names (read from out,
		 * write to in).  process_finish closes only proc->in to
		 * avoid double-close. */
		proc->in = pty_master;
		proc->out = pty_master;
		proc->err = -1;
		proc->_pty_internal = NULL;
	} else {
		if (proc->pipe_in) {
			close(pipe_in[0]);
			proc->in = pipe_in[1];
		}
		if (proc->pipe_out) {
			close(pipe_out[1]);
			proc->out = pipe_out[0];
		}
		if (proc->pipe_err) {
			close(pipe_err[1]);
			proc->err = pipe_err[0];
		}
	}

	return 0;

err_close:
	if (pty_master != -1)
		close(pty_master);
	if (pipe_in[0] != -1) {
		close(pipe_in[0]);
		close(pipe_in[1]);
	}
	if (pipe_out[0] != -1) {
		close(pipe_out[0]);
		close(pipe_out[1]);
	}
	if (pipe_err[0] != -1) {
		close(pipe_err[0]);
		close(pipe_err[1]);
	}
	return -1;
}

/**
 * @brief Wait for a child process to finish (POSIX).
 *
 * Closes any open pipe fds, then waits for the child to terminate.
 */
int process_finish(struct process *proc)
{
	int status;

	if (proc->use_pty) {
		/* @c proc->in and @c proc->out are aliases of the same
		 * master fd -- close once. */
		if (proc->in != -1) {
			close(proc->in);
			proc->in = -1;
			proc->out = -1;
		}
	} else {
		if (proc->pipe_in && proc->in != -1) {
			close(proc->in);
			proc->in = -1;
		}
		if (proc->pipe_out && proc->out != -1) {
			close(proc->out);
			proc->out = -1;
		}
		if (proc->pipe_err && proc->err != -1) {
			close(proc->err);
			proc->err = -1;
		}
	}

	if (waitpid(proc->pid, &status, 0) == -1) {
		err_errno("waitpid");
		return -1;
	}

	if (WIFEXITED(status))
		return WEXITSTATUS(status);

	if (WIFSIGNALED(status))
		return WTERMSIG(status) + 128;

	return -1;
}

/**
 * @brief Read from a pipe with a timeout (POSIX).
 *
 * select() waits up to @p timeout_ms; read() then drains whatever is
 * available.  A successful select() followed by read() == 0 means the
 * writing end has been closed (EOF), reported as -1 so the caller can
 * exit its read loop without a separate EOF signal.
 */
int pty_resize(struct process *proc, int rows, int cols)
{
	struct winsize ws;

	if (!proc->use_pty || proc->in < 0) {
		errno = EINVAL;
		return -1;
	}
	if (rows <= 0 || cols <= 0) {
		errno = EINVAL;
		return -1;
	}
	memset(&ws, 0, sizeof ws);
	ws.ws_row = (unsigned short)rows;
	ws.ws_col = (unsigned short)cols;
	if (ioctl(proc->in, TIOCSWINSZ, &ws) < 0)
		return -1;
	return 0;
}

ssize_t pipe_read_timed(int fd, void *buf, size_t n, unsigned timeout_ms)
{
	fd_set rfds;
	struct timeval tv;
	int rc;
	ssize_t got;

	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);

	tv.tv_sec = (long)(timeout_ms / 1000u);
	tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);

	do {
		rc = select(fd + 1, &rfds, NULL, NULL, &tv);
	} while (rc < 0 && errno == EINTR);

	if (rc < 0)
		return -1;
	if (rc == 0)
		return 0;

	got = read(fd, buf, n);
	if (got <= 0)
		return -1;
	return got;
}

unsigned long long mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000ull +
	       (unsigned long long)ts.tv_nsec / 1000000ull;
}

void delay_ms(uint32_t ms)
{
	struct timespec ts;
	ts.tv_sec = (time_t)(ms / 1000u);
	ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
	nanosleep(&ts, NULL);
}

int self_pid(void) { return (int)getpid(); }
