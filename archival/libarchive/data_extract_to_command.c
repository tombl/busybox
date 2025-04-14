/* vi: set sw=4 ts=4: */
/*
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 */
#include "libbb.h"
#include "bb_archive.h"
#include <sched.h>

enum {
	//TAR_FILETYPE,
	TAR_MODE,
	TAR_FILENAME,
	TAR_REALNAME,
#if ENABLE_FEATURE_TAR_UNAME_GNAME
	TAR_UNAME,
	TAR_GNAME,
#endif
	TAR_SIZE,
	TAR_UID,
	TAR_GID,
	TAR_MAX,
};

static const char *const tar_var[] ALIGN_PTR = {
	// "FILETYPE",
	"MODE",
	"FILENAME",
	"REALNAME",
#if ENABLE_FEATURE_TAR_UNAME_GNAME
	"UNAME",
	"GNAME",
#endif
	"SIZE",
	"UID",
	"GID",
};

static void xputenv(char *str)
{
	if (putenv(str))
		bb_die_memory_exhausted();
}

static void str2env(char *env[], int idx, const char *str)
{
	env[idx] = xasprintf("TAR_%s=%s", tar_var[idx], str);
	xputenv(env[idx]);
}

static void dec2env(char *env[], int idx, unsigned long long val)
{
	env[idx] = xasprintf("TAR_%s=%llu", tar_var[idx], val);
	xputenv(env[idx]);
}

static void oct2env(char *env[], int idx, unsigned long val)
{
	env[idx] = xasprintf("TAR_%s=%lo", tar_var[idx], val);
	xputenv(env[idx]);
}


struct child_args {
	file_header_t *file_header;
	char **tar_env;
	int *p;
	archive_handle_t *archive_handle;
};

static int child_func(void *data)
{
	struct child_args *args = (struct child_args *)data;
	/* Child */
	/* str2env(tar_env, TAR_FILETYPE, "f"); - parent should do it once */
	oct2env(args->tar_env, TAR_MODE, args->file_header->mode);
	str2env(args->tar_env, TAR_FILENAME, args->file_header->name);
	str2env(args->tar_env, TAR_REALNAME, args->file_header->name);
#if ENABLE_FEATURE_TAR_UNAME_GNAME
	str2env(args->tar_env, TAR_UNAME, args->file_header->tar__uname);
	str2env(args->tar_env, TAR_GNAME, args->file_header->tar__gname);
#endif
	dec2env(args->tar_env, TAR_SIZE, args->file_header->size);
	dec2env(args->tar_env, TAR_UID, args->file_header->uid);
	dec2env(args->tar_env, TAR_GID, args->file_header->gid);
	close(args->p[1]);
	xdup2(args->p[0], STDIN_FILENO);
	signal(SIGPIPE, SIG_DFL);
	execl(args->archive_handle->tar__to_command_shell,
		args->archive_handle->tar__to_command_shell,
		"-c",
		args->archive_handle->tar__to_command,
		(char *)0);
	bb_perror_msg_and_die("can't execute '%s'", args->archive_handle->tar__to_command_shell);
	return 0;
}

void FAST_FUNC data_extract_to_command(archive_handle_t *archive_handle)
{
	file_header_t *file_header = archive_handle->file_header;

#if 0 /* do we need this? ENABLE_FEATURE_TAR_SELINUX */
	char *sctx = archive_handle->tar__sctx[PAX_NEXT_FILE];
	if (!sctx)
		sctx = archive_handle->tar__sctx[PAX_GLOBAL];
	if (sctx) { /* setfscreatecon is 4 syscalls, avoid if possible */
		setfscreatecon(sctx);
		free(archive_handle->tar__sctx[PAX_NEXT_FILE]);
		archive_handle->tar__sctx[PAX_NEXT_FILE] = NULL;
	}
#endif

	if ((file_header->mode & S_IFMT) == S_IFREG) {
		pid_t pid;
		int p[2], status;
		char *tar_env[TAR_MAX];

		memset(tar_env, 0, sizeof(tar_env));

		char child_stack[4096];
		struct child_args args = {
			.file_header = file_header,
			.tar_env = tar_env,
			.p = p,
			.archive_handle = archive_handle
		};

		xpipe(p);
		pid = clone(child_func,
			child_stack + sizeof(child_stack),
			CLONE_VM | CLONE_VFORK | SIGCHLD,
			&args
		);
		if (pid < 0)
			bb_perror_msg_and_die("clone");
		close(p[0]);
		/* Our caller is expected to do signal(SIGPIPE, SIG_IGN)
		 * so that we don't die if child don't read all the input: */
		bb_copyfd_exact_size(archive_handle->src_fd, p[1], -file_header->size);
		close(p[1]);

		status = wait_for_exitstatus(pid);
		if (WIFEXITED(status) && WEXITSTATUS(status))
			bb_error_msg_and_die("'%s' returned status %d",
				archive_handle->tar__to_command, WEXITSTATUS(status));
		if (WIFSIGNALED(status))
			bb_error_msg_and_die("'%s' terminated by signal %d",
				archive_handle->tar__to_command, WTERMSIG(status));

		if (!BB_MMU) {
			int i;
			for (i = 0; i < TAR_MAX; i++) {
				if (tar_env[i])
					bb_unsetenv_and_free(tar_env[i]);
			}
		}
	}

#if 0 /* ENABLE_FEATURE_TAR_SELINUX */
	if (sctx)
		/* reset the context after creating an entry */
		setfscreatecon(NULL);
#endif
}
