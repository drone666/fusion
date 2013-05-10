#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/types.h>
#include <sys/wait.h>
#ifdef PIDWATCH_HAS_CAPABILITY_SUPPORT
#include <sys/capability.h>
#endif /* PIDWATCH_HAS_CAPABILITY_SUPPORT */

#include <unistd.h>

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <CUnit/Basic.h>

#include <pidwatch.h>

#include <fautes.h>

pid_t g_pid_max;

void read_pid_max(void)
{
	int ret;
	FILE *pmf = NULL;
	long long ll_pm;

	pmf = fopen("/proc/sys/kernel/pid_max", "rb");
	if (NULL == pmf) {
		fprintf(stderr, "Can't read /proc/sys/kernel/pid_max\n");
		exit(1);
	}

	ret = fscanf(pmf, "%lld", &ll_pm);
	if (1 != ret) {
		if (ferror(pmf))
			perror("fscanf");
		else
			fprintf(stderr, "unexpected EOF reading pid_max\n");
		exit(1);
	}

	g_pid_max = (pid_t)ll_pm;
}

void dump_args(int argc, char *argv[])
{
	do {
		fprintf(stderr, "%s ", *argv);
	} while (*(++argv));
}

pid_t __attribute__((sentinel)) launch(char *prog, ...)
{
	int ret;
	int child_argc = 0;
	char *child_argv[10] = {NULL};
	char *child_envp[] = {NULL};
	char *arg = (char *)-1;
	va_list args;
	pid_t pid;

	child_argv[child_argc++] = prog;

	va_start(args, prog);
	for (; child_argc < 10 && NULL != arg; child_argc++) {
		arg = va_arg(args, char *);
		child_argv[child_argc] = arg;
	}
	va_end(args);

	if (NULL != arg) {
		errno = E2BIG;
		return -1;
	}
	child_argv[child_argc] = NULL; /* not necessary but clearer */

	pid = fork();
	if (-1 == pid)
		return -1;

	if (0 == pid) {
		/* in child */
		fprintf(stderr, "Executing ");
		dump_args(child_argc, child_argv);
		fprintf(stderr, "\n");
		ret = execvpe(child_argv[0], child_argv, child_envp);
		if (-1 == ret) {
			perror("execve");
			exit(1);
		}
	}

	/* in parent */
	return pid;
}

#define E(type, ret)  \
({ \
	type _return = ret; \
	if (-1 == _return) { \
		int _old_errno = errno; \
\
		fprintf(stderr, "%s():%d : ", __func__, __LINE__); \
		perror("" #ret); \
		errno = _old_errno; \
	} \
\
	_return;\
})\

void testPIDWATCH_CREATE(void)
{
	pid_t pid;
	int pidfd;
	int status;
	int invalid_flag;

	/* normal cases */
	pid = E(pid_t, launch("sleep", "1", NULL));
	CU_ASSERT_NOT_EQUAL(pid, -1);
	pidfd = E(int, pidwatch_create(pid, SOCK_CLOEXEC));
	CU_ASSERT_NOT_EQUAL(pidfd, -1);
	/* cleanup */
	waitpid(pid, &status, 0);
	close(pidfd);

	/* error cases */
	pid = E(pid_t, launch("ls", "supercalifragilistic", NULL));
	CU_ASSERT_NOT_EQUAL(pid, -1);
	sleep(1);
	/*
	 * if the child dies before we set up the watch, it is a zombie, thus
	 * considered dead, ESRCH is raised
	 */
	pidfd = pidwatch_create(pid, SOCK_CLOEXEC);
	CU_ASSERT(ESRCH == errno);
	CU_ASSERT_EQUAL(pidfd, -1);
	/* cleanup */
	waitpid(pid, &status, 0);

	/* invalid arguments */
	pidfd = pidwatch_create(-63, SOCK_CLOEXEC);
	CU_ASSERT_EQUAL(pidfd, -1);
	invalid_flag = ~(SOCK_CLOEXEC | SOCK_NONBLOCK);
	pidfd = pidwatch_create(g_pid_max, SOCK_CLOEXEC);
	CU_ASSERT_EQUAL(pidfd, -1);
	pidfd = pidwatch_create(1, invalid_flag); /* pid 1 is always valid */
	CU_ASSERT_EQUAL(pidfd, -1);
}

void testPIDWATCH_WAIT(void)
{
	pid_t pid;
	pid_t pid_ret;
	int pidfd;
	int status;
	int wstatus;
	int ret;

	/* normal cases */
	/* normal termination */
	pid = E(pid_t, launch("sleep", "1", NULL));
	CU_ASSERT_NOT_EQUAL(pid, -1);
	pidfd = E(int, pidwatch_create(pid, SOCK_CLOEXEC));
	CU_ASSERT_NOT_EQUAL(pidfd, -1);
	pid_ret = E(pid_t, pidwatch_wait(pidfd, &status));
	CU_ASSERT_NOT_EQUAL(pid_ret, -1);
	/* cleanup */
	waitpid(pid, &wstatus, 0);
	CU_ASSERT_EQUAL(status, wstatus);
	close(pidfd);

	/* terminated by signal */
	pid = E(pid_t, launch("sleep", "1", NULL));
	CU_ASSERT_NOT_EQUAL(pid, -1);
	pidfd = E(int, pidwatch_create(pid, SOCK_CLOEXEC));
	CU_ASSERT_NOT_EQUAL(pidfd, -1);
	ret = E(int, kill(pid, 9));
	CU_ASSERT_NOT_EQUAL(ret, -1);
	pid_ret = E(pid_t, pidwatch_wait(pidfd, &status));
	CU_ASSERT_NOT_EQUAL(pid_ret, -1);
	/* cleanup */
	waitpid(pid, &wstatus, 0);
	CU_ASSERT_EQUAL(status, wstatus);
	close(pidfd);


	/* error cases */
	pid_ret = pidwatch_wait(-1, &status);
	CU_ASSERT_EQUAL(pid_ret, -1);
	pid_ret = pidwatch_wait(1, NULL);
	CU_ASSERT_EQUAL(pid_ret, -1);
}

#ifdef PIDWATCH_HAS_CAPABILITY_SUPPORT
void free_cap(cap_t *cap)
{
	if (cap)
		cap_free(*cap);
}

/**
 * Checks if a capability is effective for the current process. If not, allows
 * to try activating it.
 * @param value Capability to check
 * @param try If non-zero and if the capability isn't set, try to raise it
 * @return -1 on error, with errno set, 0 if the capability was already
 * effective, 1 if not, but it was permetted, try was non-zero and it was raised
 */
int check_proc_cap(cap_value_t value, int try)
{
	int ret;
	cap_t __attribute__((cleanup(free_cap))) caps;
	cap_flag_value_t flag_value;

	caps = cap_get_proc();
	if (NULL == caps)
		return -1;

	ret = cap_get_flag(caps, value, CAP_EFFECTIVE, &flag_value);
	if (-1 == ret)
		return -1;
	if (CAP_SET != value) {
		ret = cap_set_flag(caps, CAP_EFFECTIVE, 1, &value, CAP_SET);
		if (-1 == ret)
			return -1;
		ret = cap_set_proc(caps);
		if (-1 == ret)
			return -1;
		return 1;
	}

	return 0;
}
#endif /* PIDWATCH_HAS_CAPABILITY_SUPPORT */

static const struct test_t tests[] = {
		{
				.fn = testPIDWATCH_CREATE,
				.name = "pidwatch_create"
		},
		{
				.fn = testPIDWATCH_WAIT,
				.name = "pidwatch_wait"
		},

		/* NULL guard */
		{.fn = NULL, .name = NULL},
};

static int init_pw_suite(void)
{
#ifdef PIDWATCH_HAS_CAPABILITY_SUPPORT
	int ret;

	ret = check_proc_cap(CAP_NET_ADMIN, 1);
	if (-1 == ret) {
		fprintf(stderr, "CAP_NET_ADMIN is needed for pidwatch\n");
		return 1;
	}
#endif /* PIDWATCH_HAS_CAPABILITY_SUPPORT */

	return 0;
}

static int clean_pw_suite(void)
{
	return 0;
}

struct suite_t pidwatch_suite = {
		.name = "pidwatch",
		.init = init_pw_suite,
		.clean = clean_pw_suite,
		.tests = tests,
};
