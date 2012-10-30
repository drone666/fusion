/**
 * @file io_src_msg_test.c
 * @date 22 oct. 2012
 * @author nicolas.carrier@parrot.com
 * @brief Unit tests for signal source.
 *
 * Copyright (C) 2012 Parrot S.A.
 */

#include <unistd.h>
#include <fcntl.h>

#include <limits.h>
#include <stdbool.h>
#include <signal.h>
#include <stddef.h>

#include <CUnit/Basic.h>

#include <io_mon.h>
#include <io_src_msg.h>

#include <fautes.h>
#include <fautes_utils.h>

static void reached_state(int *glob_state, int state)
{
	*glob_state |= state;
}

struct msg {
	char a;
	int b;
	double c;
};

struct my_msg_src {
	struct msg msg;
	struct io_src_msg src;
	int pipefds[2];
};

#define to_src_my_msg_src(p) container_of(p, struct my_msg_src, src)

static void my_msg_src_clean_cb(struct io_src *src)
{
	struct io_src_msg *msg = to_src_msg(src);
	struct my_msg_src *my_src = to_src_my_msg_src(msg);

	close(my_src->pipefds[0]);
	close(my_src->pipefds[1]);
	memset(my_src, 0, sizeof(my_src));
	// TODO add call to parent cleanup function
}

static const struct msg MSG1 = {11, 11111, 11.111};
static const struct msg MSG2 = {22, 22222, 22.222};
static const struct msg MSG3 = {33, 33333, 33.333};
static const struct msg MSG4 = {44, 44444, 44.444};

#define STATE_START 0
#define STATE_MSG1_RECEIVED 1
#define STATE_MSG2_RECEIVED 2
#define STATE_MSG3_RECEIVED 4
#define STATE_MSG4_RECEIVED 8
#define STATE_ALL_DONE 15
int state = STATE_START;

static int msg_cb(struct io_src_msg *src)
{
	int ret;
	struct my_msg_src *my_src = to_src_my_msg_src(src);
//		__attribute__((unused))struct msg *my_msg = src->msg;
/*		printf("received : \"%d %d %f\"\n", my_msg->a, my_msg->b,
			my_msg->c);*/

	CU_ASSERT_NOT_EQUAL(state, STATE_ALL_DONE);

	if (0 == memcmp(src->msg, &MSG1, src->len)) {
		CU_ASSERT_EQUAL(state, STATE_START);
		reached_state(&state, STATE_MSG1_RECEIVED);

		ret = write(my_src->pipefds[1], &MSG2,
				sizeof(struct msg));
		CU_ASSERT_NOT_EQUAL(ret, -1);
	} else if (0 == memcmp(src->msg, &MSG2, src->len)) {
		CU_ASSERT_EQUAL(state, STATE_MSG1_RECEIVED);
		reached_state(&state, STATE_MSG2_RECEIVED);

		ret = write(my_src->pipefds[1], &MSG3,
				sizeof(struct msg));
		CU_ASSERT_NOT_EQUAL(ret, -1);
	} else if (0 == memcmp(src->msg, &MSG3, src->len)) {
		CU_ASSERT_EQUAL(state, STATE_MSG1_RECEIVED |
				STATE_MSG2_RECEIVED);
		reached_state(&state, STATE_MSG3_RECEIVED);

		ret = write(my_src->pipefds[1], &MSG4,
				sizeof(struct msg));
		CU_ASSERT_NOT_EQUAL(ret, -1);
	} else if (0 == memcmp(src->msg, &MSG4, src->len)) {
		CU_ASSERT_EQUAL(state, STATE_MSG1_RECEIVED |
				STATE_MSG2_RECEIVED |
				STATE_MSG3_RECEIVED);
		reached_state(&state, STATE_MSG4_RECEIVED);
	}

	return 0;
}

/* main and only test. sends ourselves messages and check we receive them */
static void testSRC_MSG_INIT(void)
{
	fd_set rfds;
	int ret;
	struct io_mon mon;
	struct my_msg_src msg_src;
	bool loop = true;
	struct timeval timeout;

	ret = io_mon_init(&mon);
	CU_ASSERT_EQUAL(ret, 0);
	ret = pipe(msg_src.pipefds);
	CU_ASSERT_EQUAL(ret, 0);
	ret = io_src_msg_init(&(msg_src.src),
			msg_src.pipefds[0],
			msg_cb,
			my_msg_src_clean_cb,
			&(msg_src.msg),
			sizeof(msg_src.msg));
	CU_ASSERT_EQUAL(ret, 0);

	ret = io_mon_add_source(&mon, &(msg_src.src.src));
	CU_ASSERT_EQUAL(ret, 0);

	ret = write(msg_src.pipefds[1], &MSG1, sizeof(msg_src.msg));
	CU_ASSERT_NOT_EQUAL(ret, -1);

	/* normal use case */
	while (loop) {
		/* restore the timer */
		timeout.tv_sec = 1;
		timeout.tv_usec = 0;

		/* restore the read file descriptor set */
		FD_ZERO(&rfds);
		FD_SET(mon.epollfd, &rfds);
		ret = select(mon.epollfd + 1, &rfds, NULL, NULL, &timeout);

		/* error, not normal */
		CU_ASSERT_NOT_EQUAL(ret, -1);
		if (-1 == ret)
			goto out;

		/* timeout, not normal */
		CU_ASSERT_NOT_EQUAL(ret, 0);
		if (0 == ret)
			goto out;

		ret = io_mon_process_events(&mon);
		CU_ASSERT_EQUAL(ret, 0);
		if (0 != ret)
			goto out;

		loop = STATE_ALL_DONE != state;
	}

out:
	/* debriefing */
	CU_ASSERT(state & STATE_MSG1_RECEIVED);
	CU_ASSERT(state & STATE_MSG2_RECEIVED);
	CU_ASSERT(state & STATE_MSG3_RECEIVED);
	CU_ASSERT(state & STATE_MSG4_RECEIVED);

	/* error cases */
	ret = io_src_msg_init(NULL, msg_src.pipefds[0], msg_cb, my_msg_src_clean_cb, &(msg_src.msg), sizeof(struct msg));
	CU_ASSERT_NOT_EQUAL(ret, 0);
	ret = io_src_msg_init(&(msg_src.src), -1, msg_cb, my_msg_src_clean_cb, &(msg_src.msg), sizeof(struct msg));
	CU_ASSERT_NOT_EQUAL(ret, 0);
	ret = io_src_msg_init(&(msg_src.src), msg_src.pipefds[0], msg_cb, my_msg_src_clean_cb, NULL, sizeof(struct msg));
	CU_ASSERT_NOT_EQUAL(ret, 0);
	ret = io_src_msg_init(&(msg_src.src), msg_src.pipefds[0], msg_cb, my_msg_src_clean_cb, &(msg_src.msg), 0);
	CU_ASSERT_NOT_EQUAL(ret, 0);
	ret = io_src_msg_init(&(msg_src.src), msg_src.pipefds[0], NULL, my_msg_src_clean_cb, &(msg_src.msg), sizeof(struct msg));
	CU_ASSERT_NOT_EQUAL(ret, 0);

	/* cleanup */
	io_mon_clean(&mon);
}

static const test_t tests[] = {
		{
				.fn = testSRC_MSG_INIT,
				.name = "io_src_msg_init"
		},

		/* NULL guard */
		{.fn = NULL, .name = NULL},
};

suite_t src_msg_suite = {
		.name = "io_src_msg",
		.init = NULL,
		.clean = NULL,
		.tests = tests,
};