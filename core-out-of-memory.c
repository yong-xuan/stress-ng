/*
 * Copyright (C) 2013-2021 Canonical, Ltd.
 * Copyright (C) 2022-2023 Colin Ian King.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#include "stress-ng.h"

#if defined(__linux__)

#define OOM_SCORE_ADJ_MIN	"-1000"
#define OOM_SCORE_ADJ_MAX	"1000"

#define OOM_ADJ_NO_OOM		"-17"
#define OOM_ADJ_MIN		"-16"
#define OOM_ADJ_MAX		"15"

/*
 *  stress_process_oomed()
 *	check if a process has been logged as OOM killed
 */
bool stress_process_oomed(const pid_t pid)
{
	int fd;
	bool oomed = false;

	fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return oomed;

	for (;;) {
		char buf[4096], *ptr;
		ssize_t ret;

		ret = read(fd, buf, sizeof(buf) - 1);
		if (ret < 0)
			break;
		buf[ret] = '\0';

		/*
		 * Look for 'Out of memory: Kill process 22566'
		 */
		ptr = strstr(buf, "process");
		if (ptr && (strstr(buf, "Out of memory") ||
			    strstr(buf, "oom_reaper"))) {
			intmax_t oom_pid;

			if (sscanf(ptr + 7, "%10" SCNdMAX, &oom_pid) == 1) {
				if (oom_pid == (intmax_t)pid) {
					oomed = true;
					break;
				}
			}
		}
	}
	(void)close(fd);

	return oomed;
}

static const char *stress_args_name(const stress_args_t *args)
{
	if (!args)
		return "main";
	return args->name;
}

/*
 *    stress_set_adjustment()
 *	try to set OOM adjustment, retry if EAGAIN or EINTR, give up
 *	after multiple retries.  Returns 0 for success, -errno for
 *	on failure.
 */
static int stress_set_adjustment(
	const stress_args_t *args,
	const char *procname,
	const char *str)
{
	const size_t len = strlen(str);
	int i, saved_errno = 0;

	for (i = 0; i < 32; i++) {
		ssize_t n;
		int fd;

		fd = open(procname, O_WRONLY);
		if (fd < 0)
			return -errno;

		n = write(fd, str, len);
		saved_errno = errno;

		(void)close(fd);
		if (n > 0)
			return 0;
		if (n < 0) {
			if ((saved_errno != EAGAIN) &&
			    (saved_errno != EINTR)) {
				if (args && args->instance == 0)
					pr_dbg("%s: can't set oom_score_adj, errno=%d (%s)\n",
						stress_args_name(args), errno, strerror(errno));
				return -saved_errno;
			}
		}
	}
	/* Unexpected failure, report why */
	if (args && args->instance == 0)
		pr_dbg("%s: can't set oom_score_adj, errno=%d (%s)\n", stress_args_name(args),
			saved_errno, strerror(saved_errno));
	return -1;
}

/*
 *  stress_set_oom_adjustment()
 *	attempt to stop oom killer
 *	if we have root privileges then try and make process
 *	unkillable by oom killer
 *	NOTE: null args -> main stress-ng process, otherwise a stressor
 */
void stress_set_oom_adjustment(const stress_args_t *args, const bool killable)
{
	bool high_priv;
	bool make_killable = killable;
	char *str;
	int ret;

	/*
	 *  --no-oom-adjust option ignores any oom adjustments
	 */
	if (g_opt_flags & OPT_FLAGS_NO_OOM_ADJUST)
		return;

	high_priv = (getuid() == 0) && (geteuid() == 0);

	/*
	 *  main cannot be killable; if OPT_FLAGS_OOMABLE set make
	 *  all child procs easily OOMable
	 */
	if (args && (g_opt_flags & OPT_FLAGS_OOMABLE))
		make_killable = true;

	/*
	 *  Try modern oom interface
	 */
	if (make_killable)
		str = OOM_SCORE_ADJ_MAX;
	else
		str = high_priv ? OOM_SCORE_ADJ_MIN : "0";
	ret = stress_set_adjustment(args, "/proc/self/oom_score_adj", str);
	/*
	 *  Success or some random failure that's not -ENOENT
	 */
	if ((ret == 0) || (ret != -ENOENT))
		return;
	/*
	 *  Fall back to old oom interface if we got -ENOENT
	 */
	if (make_killable)
		str = OOM_ADJ_MAX;
	else
		str = high_priv ? OOM_ADJ_NO_OOM : OOM_ADJ_MIN;

	(void)stress_set_adjustment(args, "/proc/self/oom_adj", str);
}
#else
void stress_set_oom_adjustment(const stress_args_t *args, const bool killable)
{
	(void)args;
	(void)killable;
}
bool stress_process_oomed(const pid_t pid)
{
	(void)pid;

	return false;
}
#endif

/*
 *  stress_oomable_child()
 *  	generic way to run a process that is possibly going to be
 *	OOM'd and we retry if it gets killed.
 */
int stress_oomable_child(
	const stress_args_t *args,
	void *context,
	stress_oomable_child_func_t func,
	const int flag)
{
	pid_t pid;
	int ooms = 0;
	int segvs = 0;
	int buserrs = 0;
	int rc = EXIT_SUCCESS;
	size_t signal_idx = 0;
	const bool not_quiet = !(flag & STRESS_OOMABLE_QUIET);

	/*
	 *  Kill child multiple times, start with SIGALRM and work up
	 */
	static const int signals[] = {
		SIGALRM,
		SIGALRM,
		SIGALRM,
		SIGALRM,
		SIGTERM,
		SIGKILL
	};

again:
	if (!stress_continue(args))
		return EXIT_SUCCESS;
	if (stress_time_now() > args->time_end)
		return EXIT_SUCCESS;
	pid = fork();
	if (pid < 0) {
		/* Keep trying if we are out of resources */
		if ((errno == EAGAIN) || (errno == ENOMEM)) {
			/* don't retry for 1/10th sec */
			(void)shim_usleep(100000);
			goto again;
		}
		if (not_quiet)
			pr_err("%s: fork failed: errno=%d: (%s)\n",
				args->name, errno, strerror(errno));
		return -1;
	} else if (pid > 0) {
		/* Parent, wait for child */
		int status, ret;

rewait:
		stress_set_proc_state(args->name, STRESS_STATE_WAIT);
		ret = waitpid(pid, &status, 0);
		if (ret < 0) {
			stress_set_proc_state(args->name, STRESS_STATE_RUN);
			/* No longer alive? */
			if (errno == ECHILD)
				goto report;
			if ((errno != EINTR) && not_quiet)
				pr_dbg("%s: waitpid(): errno=%d (%s)\n",
					args->name, errno, strerror(errno));

			(void)shim_kill(pid, signals[signal_idx]);
			if (++signal_idx >= SIZEOF_ARRAY(signals))
				goto report;

			/*
			 *  First time round do fast re-wait
			 *  in case child can be reaped quickly,
			 *  there after do slow backoff on each
			 *  iteration until we give up and do
			 *  the final SIGKILL
			 */
			if (signal_idx > 1)
				(void)shim_usleep(500000);
			goto rewait;
		} else if (WIFSIGNALED(status)) {
			stress_set_proc_state(args->name, STRESS_STATE_RUN);
			if (not_quiet)
				pr_dbg("%s: child died: %s (instance %d)\n",
					args->name, stress_strsignal(WTERMSIG(status)),
					args->instance);
			/* Bus error death? retry */
			if (WTERMSIG(status) == SIGBUS) {
				buserrs++;
				goto again;
			}

			/* If we got killed by OOM killer, re-start */
			if ((signals[signal_idx] != SIGKILL) && (WTERMSIG(status) == SIGKILL)) {
				/*
				 *  The --oomable flag was enabled, so
				 *  the behaviour here is to no longer
				 *  retry.  The exit return is EXIT_SUCCESS
				 *  because the child is allowed to terminate
				 *  by being OOM'd.
				 */
				if (g_opt_flags & OPT_FLAGS_OOMABLE) {
					stress_log_system_mem_info();
					if (not_quiet)
						pr_dbg("%s: assuming killed by OOM "
							"killer, bailing out "
							"(instance %d)\n",
							args->name, args->instance);
					stress_clean_dir(args->name, args->pid, args->instance);
					return EXIT_SUCCESS;
				} else {
					stress_log_system_mem_info();
					if (not_quiet)
						pr_dbg("%s: assuming killed by OOM "
							"killer, restarting again "
							"(instance %d)\n",
							args->name, args->instance);
					ooms++;
					goto again;
				}
			}
			/* If we got killed by sigsegv, re-start */
			if (WTERMSIG(status) == SIGSEGV) {
				if (not_quiet)
					pr_dbg("%s: killed by SIGSEGV, "
						"restarting again "
						"(instance %d)\n",
						args->name, args->instance);
				segvs++;
				goto again;
			}
		}
		rc = WEXITSTATUS(status);
	} else {
		/* Child */
		int ret;

		if (!stress_continue(args)) {
			stress_set_proc_state(args->name, STRESS_STATE_EXIT);
			_exit(EXIT_SUCCESS);
		}

		stress_parent_died_alarm();

		/* Make sure this is killable by OOM killer */
		stress_set_oom_adjustment(args, true);

		/* Explicitly drop capabilities, makes it more OOM-able */
		if (flag & STRESS_OOMABLE_DROP_CAP) {
			VOID_RET(int, stress_drop_capabilities(args->name));
		}
		/*
		 * Process may have exceeded run time by the time it was
		 * fully runnable, so check for this before doing expensive
		 * stressor invocation
		 */
		if (!stress_continue(args) ||
		    (stress_time_now() > args->time_end)) {
			stress_set_proc_state(args->name, STRESS_STATE_EXIT);
			_exit(EXIT_SUCCESS);
		}

		/* ..and finally re-start the stressor */
		ret = func(args, context);
		pr_fail_check(&rc);
		if (rc != EXIT_SUCCESS)
			ret = rc;

		stress_set_proc_state(args->name, STRESS_STATE_EXIT);
		_exit(ret);
	}

report:
	if ((ooms + segvs + buserrs > 0) && not_quiet) {
		pr_dbg("%s: OOM restarts: %d"
			", SIGSEGV restarts: %d"
			", SIGBUS restarts: %d\n",
			args->name, ooms, segvs, buserrs);
	}

	return rc;
}
