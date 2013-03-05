/**
 * @file io_src_sig.c
 * @date 22 oct. 2012
 * @author nicolas.carrier@parrot.com
 * @brief Signal source. Don't use. Signals are crap.
 *
 * Copyright (C) 2012 Parrot S.A.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif /* _GNU_SOURCE */
#include <signal.h>
#include <unistd.h>

#include <errno.h>
#include <stdarg.h>
#include <string.h>

#include "io_mon.h"
#include "io_src_sig.h"
#include "io_platform.h"
#include "io_utils.h"

/**
 * @def to_src
 * @brief Convert a source to it's signal source container
 */
#define to_src_sig(p) container_of(p, struct io_src_sig, src)

/**
 * Source callback, reads the signal and notifies the client
 * @param src Underlying monitor source of the signal source
 */
static int sig_cb(struct io_src *src)
{
	ssize_t ret;
	struct io_src_sig *sig = to_src_sig(src);

	if (io_mon_has_error(src->events))
		return -EIO;

	ret = io_read(src->fd, &(sig->si), sizeof(sig->si));
	if (sizeof(sig->si) != ret)
		return -errno;

	return sig->cb(sig);
}

/**
 * Callback called when the source is removed
 * @param src Underlying monitor source of the signal source
 */
static void sig_clean(struct io_src *src)
{
	struct io_src_sig *sig;

	if (NULL == src)
		return;
	sig = to_src_sig(src);

	/* restore gently the signal mask */
	sigprocmask(SIG_SETMASK, &(sig->old_mask), NULL);
	sigemptyset(&(sig->mask));
	sigemptyset(&(sig->old_mask));
	sig->cb = NULL;

	memset(&(sig->si), 0, sizeof(sig->si));
}

/**
 * Builds the signal mask, with a list of signal numbers
 * @param m Signal set, initialized in output
 * @param args List of signals, the first being already processed
 * @param signo First signal of the list, already extracted from args
 * @return Opposite of the first sigaddset errno on error, 0 otherwise
 */
static int build_sig_mask(sigset_t *m, va_list args, int signo)
{
	int ret;

	/* add all the signals to the mask */
	sigemptyset(m);
	while (0 != signo) {
		ret = sigaddset(m, signo);
		if (-1 == ret)
			return -errno;
		signo = va_arg(args, int);
	}

	return 0;
}

/**
 * Check if any of the arguments of io_src_sig_init si invalid
 * @param sig Signal source
 * @param cb Signal source callback
 * @param signo First signal of the ellipsis
 * @return non-zero if at least one argument is invalid, 0 otherwise
 */
static int sig_init_args_are_invalid(struct io_src_sig *sig, io_sig_cb_t *cb,
		int signo)
{
	return NULL == sig || NULL == cb || 0 == signo;
}

int io_src_sig_init(struct io_src_sig *sig, io_sig_cb_t *cb, ...)
{
	int ret;
	int fd;
	va_list args;
	sigset_t *m;
	int signo;

	/* firts signal MUST be retrieved before any modification of sig */
	va_start(args, cb);
	signo = va_arg(args, int);
	if (sig_init_args_are_invalid(sig, cb, signo))
		return -EINVAL;

	/* here we can start to modify the context */
	memset(sig, 0, sizeof(*sig));
	m = &(sig->mask);

	ret = build_sig_mask(m, args, signo);
	va_end(args);
	if (0 > ret)
		return ret;

	/*
	 * block signals so that they aren't handled according to their default
	 * dispositions
	 */
	ret = sigprocmask(SIG_BLOCK, m, &(sig->old_mask));
	if (-1 == ret)
		return -errno;

	/* set up signal fd */
	fd = signalfd(-1, m, SFD_NONBLOCK | SFD_CLOEXEC);
	if (-1 == fd) {
		ret = -errno;
		goto out;
	}

	sig->cb = cb;

	/* can fail only on parameters */
	return io_src_init(&(sig->src), fd, IO_IN, sig_cb, sig_clean);
out:
	sig_clean(&(sig->src));

	return ret;
}
