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
#include "ice.h"

#include <sys/select.h>
#include <sys/wait.h>
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
int process_start(struct process *proc)
{
	int pipe_in[2] = {-1, -1};
	int pipe_out[2] = {-1, -1};
	int pipe_err[2] = {-1, -1};

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

	proc->pid = fork();
	if (proc->pid == -1) {
		err_errno("fork");
		goto err_close;
	}

	/* Child */
	if (proc->pid == 0) {
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

		if (proc->env) {
			for (int i = 0; proc->env[i]; i++)
				putenv((char *)proc->env[i]);
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

	return 0;

err_close:
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
