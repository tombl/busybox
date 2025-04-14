/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * COPYING NOTES
 *
 * timeout.c -- a timeout handler for shell commands
 *
 * Copyright (C) 2005-6, Roberto A. Foglietta <me@roberto.foglietta.name>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; version 2 of the License.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
 */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * REVISION NOTES:
 * released 17-11-2005 by Roberto A. Foglietta
 * talarm   04-12-2005 by Roberto A. Foglietta
 * modified 05-12-2005 by Roberto A. Foglietta
 * sizerdct 06-12-2005 by Roberto A. Foglietta
 * splitszf 12-05-2006 by Roberto A. Foglietta
 * rewrite  14-11-2008 vda
 */
//config:config TIMEOUT
//config:	bool "timeout (6.5 kb)"
//config:	default y
//config:	help
//config:	Runs a program and watches it. If it does not terminate in
//config:	specified number of seconds, it is sent a signal.

//applet:IF_TIMEOUT(APPLET(timeout, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_TIMEOUT) += timeout.o

//usage:#define timeout_trivial_usage
//usage:       "[-s SIG] [-k KILL_SECS] SECS PROG ARGS"
//usage:#define timeout_full_usage "\n\n"
//usage:       "Run PROG. Send SIG to it if it is not gone in SECS seconds.\n"
//usage:       "Default SIG: TERM."
//usage:       "If it still exists in KILL_SECS seconds, send KILL.\n"

#include <sched.h>   /* for clone() */
#include "libbb.h"

static NOINLINE int timeout_wait(duration_t timeout, pid_t pid)
{
	/* Just sleep(HUGE_NUM); kill(parent) may kill wrong process! */
	while (1) {
#if ENABLE_FLOAT_DURATION
		if (timeout < 1)
			sleep_for_duration(timeout);
		else
#endif
			sleep1();
		if (--timeout <= 0)
			break;
		if (kill(pid, 0)) {
			/* process is gone */
			return EXIT_SUCCESS;
		}
	}
	return EXIT_FAILURE;
}

struct child_args {
	duration_t timeout;
	duration_t kill_timeout;
	pid_t parent_pid;
	int signo;
};

/* This function runs in the cloned child process */
static int child_func(void *data)
{
	struct child_args *args = (struct child_args *)data;

	/* Sleep, then kill parent if it's still alive */
	if (timeout_wait(args->timeout, args->parent_pid) == EXIT_SUCCESS)
		return EXIT_SUCCESS;
	kill(args->parent_pid, args->signo);

	if (args->kill_timeout > 0) {
		if (timeout_wait(args->kill_timeout, args->parent_pid) == EXIT_SUCCESS)
			return EXIT_SUCCESS;
		kill(args->parent_pid, SIGKILL);
	}

	return EXIT_SUCCESS;
}

int timeout_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int timeout_main(int argc UNUSED_PARAM, char **argv)
{
	int signo;
	duration_t timeout;
	duration_t kill_timeout;
	pid_t parent_pid;
	pid_t pid;
	const char *opt_s = "TERM";
	char *opt_k = NULL;
	char child_stack[4096];
	struct child_args args;

	/* '+': stop at first non-option */
	getopt32(argv, "+s:k:", &opt_s, &opt_k);
	argv += optind;

	signo = get_signum(opt_s);
	if (signo < 0)
		bb_error_msg_and_die("unknown signal '%s'", opt_s);

	kill_timeout = 0;
	if (opt_k)
		kill_timeout = parse_duration_str(opt_k);

	if (!argv[0])
		bb_show_usage();
	timeout = parse_duration_str(argv[0]++);
	if (!argv[0]) /* no PROG? */
		bb_show_usage();

	parent_pid = getpid();

	/* Setup args for the child */
	args.timeout = timeout;
	args.kill_timeout = kill_timeout;
	args.parent_pid = parent_pid;
	args.signo = signo;

	/* Clone the child process to monitor the parent */
	pid = clone(child_func,
			child_stack + sizeof(child_stack),
			CLONE_VM | SIGCHLD,
			&args
	);

	if (pid < 0) {
		bb_perror_msg_and_die("clone");
	}

	/* Parent continues here immediately after child exits or execs */
	/* We removed the wait() because CLONE_VFORK suspends parent */
	/* until child calls _exit() or execve(), and our child_func */
	/* only calls _exit() eventually after sleeps/kills */

	/* Ok, exec the program as requested */
	BB_EXECVP_or_die(argv);
}
