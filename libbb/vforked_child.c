/* vi: set sw=4 ts=4: */
/*
 * Keep track of vforked children: see bb_vfork() in libbb.h.
 *
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 */
#include "libbb.h"
#include <sys/syscall.h>

/* The vforks in progress in this memory. The process that makes the first
 * one owns the list; its child may make the next, and so on. Each parent is
 * suspended until its child execs or exits, so they form one list, and they
 * finish in reverse order. Level n holds the signal mask that its parent
 * restores when it resumes, and the pid of its child once the child has
 * started. None of this can live in the parent's stack frame, which the
 * child is running on and may reuse.
 *
 * A child with a copy of this memory must not inherit the list: busybox's
 * fork() and daemon(), and bb_clone() without CLONE_VM, empty it in the
 * child (bb_forget_vforks) before running any of the caller's code. A copy
 * made some other way must call bb_forget_vforks() itself before it runs
 * busybox code; until then a pid in the list it copied could be reused for
 * it, and it could look vforked. Where vfork() is really fork(), as on some
 * uClibc builds, its child does count as vforked, which is harmless: it is
 * code written for a vforked child, and its exit() would flush the stdio
 * buffers it copied from its parent a second time. Pids are compared only
 * as each process sees itself, never through getppid(): in a new pid
 * namespace, a child has no pid for its parent.
 *
 * No guarantees are made once a parent is killed while it is suspended in
 * a vfork. Its own parent then resumes while its children still run in the
 * same memory, and possibly on the same stack, which nothing here can make
 * safe. The bookkeeping only sees to it that those children can't change
 * the levels of the processes that remain: they aren't in the list, so
 * they can't vfork while it is in use, and a child that hasn't started
 * yet can join only the newest level, above every level that remains.
 * Once the list is empty again, one of them may start a new one, and the
 * owner's own vforks then fail with EAGAIN until that one is done.
 */
enum { MAX_VFORK_DEPTH = 8 };
static struct {
	unsigned depth;
	struct vfork_level {
		/* The parent, as it sees itself, and a token of its own: see
		 * bb_vfork_finish(). */
		pid_t parent;
		const void *token;
		pid_t child; /* 0 until the child has started */
		sigset_t mask;
	} level[MAX_VFORK_DEPTH];
} vforks;

/* Not getpid(): some libcs cache the pid, and older glibc updates its cache
 * across vfork() in memory that the parent shares. */
static pid_t current_pid(void)
{
	return syscall(SYS_getpid);
}

void FAST_FUNC bb_forget_vforks(void)
{
	vforks.depth = 0;
}

/* Our level, searching down from level top: n for the child of level n-1,
 * 0 for the owner, or -1 for a process that isn't in the list. Searching
 * from the newest first matters only where pid namespaces let two levels'
 * pids, each seen by its own process, be the same number: the process that
 * is running is the newest. */
static int level_of(pid_t self, unsigned top)
{
	unsigned n;

	if (!vforks.depth)
		return -1;
	for (n = top; n > 0; n--)
		if (vforks.level[n - 1].child == self)
			return n;
	return self == vforks.level[0].parent ? 0 : -1;
}

#if BB_MMU
/* The parentheses keep libbb.h's macros from expanding. */
pid_t FAST_FUNC bb_fork(void)
{
	pid_t pid = (fork)();

	if (pid == 0)
		bb_forget_vforks();
	return pid;
}

int FAST_FUNC bb_daemon(int nochdir, int noclose)
{
	pid_t self = current_pid();
	/* daemon() may fail after it has forked. */
	int r = (daemon)(nochdir, noclose);

	if (current_pid() != self)
		bb_forget_vforks();
	return r;
}
#endif

int FAST_FUNC bb_vforked_child(void)
{
	return level_of(current_pid(), vforks.depth) > 0;
}

/* Returns 0, or -1 with errno EAGAIN if vforks are nested too deeply, or if
 * this process can't vfork. */
int FAST_FUNC bb_vfork_prepare(const void *token)
{
	sigset_t all, mask;
	pid_t self;
	int level;

	sigfillset(&all);
	sigprocmask(SIG_BLOCK, &all, &mask);
	self = current_pid();
	if (!vforks.depth) {
		level = 0;
	} else {
		level = level_of(self, vforks.depth);
		if (level < 0)
			goto fail;
	}
	if (level >= MAX_VFORK_DEPTH)
		goto fail;
	vforks.level[level].parent = self;
	vforks.level[level].token = token;
	vforks.level[level].child = 0;
	vforks.level[level].mask = mask;
	vforks.depth = level + 1;
	return 0;
 fail:
	sigprocmask(SIG_SETMASK, &mask, NULL);
	errno = EAGAIN;
	return -1;
}

/* Called in the child (pid == 0) as soon as it runs, and in the parent once
 * it resumes, or straight away if the vfork failed, with the token it gave
 * bb_vfork_prepare(). */
void FAST_FUNC bb_vfork_finish(pid_t pid, const void *token)
{
	int errno_save = errno;
	sigset_t mask;
	int restore = 0;

	if (pid == 0) {
		/* Our level is the newest, made by our parent. */
		unsigned top = vforks.depth - 1;

		if (vforks.depth && !vforks.level[top].child) {
			vforks.level[top].child = current_pid();
			mask = vforks.level[top].mask;
			restore = 1;
		}
	} else {
		/* Our level is the newest one we made, recognized by our pid
		 * as we see it and by our token, the address of an object of
		 * ours that is live until we return. In a new pid namespace,
		 * another level's parent can have our pid; and a vforked child
		 * running in our frame can reuse our token. */
		pid_t self = current_pid();
		unsigned n = vforks.depth;

		while (n-- > 0) {
			struct vfork_level *l = &vforks.level[n];

			if (l->parent == self && l->token == token) {
				/* Copied first: once our level is gone from
				 * the list, a killed parent's child could
				 * reuse it. Our child is done, so anything
				 * above our level is too, unless a killed
				 * parent left it running. */
				mask = l->mask;
				barrier();
				vforks.depth = n;
				restore = 1;
				break;
			}
		}
	}
	if (restore)
		sigprocmask(SIG_SETMASK, &mask, NULL);
	errno = errno_save;
}
