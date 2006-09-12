#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include "trace.h"
#include "daemonize.h"

pid_t daemonize(char const *logfile)
{
	struct sigaction sa;
	pid_t pid;

	sa.sa_handler = SIG_IGN;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	if (sigaction(SIGCHLD, &sa, NULL) == -1)
		error("could not disable SIGCHLD: %s", strerror(errno));

	fflush(stdout);

	pid = fork();

	if (pid == 0) {
		setpgid(0, 0);

		/* we should close all open file descriptors but the
		 * three standard descriptors should be the only ones
		 * open at this point and they are replaced by freopen */

		if (!logfile)
		    logfile = "/dev/null";

		if (freopen("/dev/null", "r", stdin) == NULL)
		    error("could not reopen stdin\n");
		if (freopen(logfile, "a", stdout) == NULL)
		    error("could not reopen stdout\n");
		if (freopen(logfile, "a", stderr) == NULL)
		    error("could not reopen stderr\n");

		dup2(fileno(stdout), fileno(stderr));
		setlinebuf(stdout);

		/* FIXME: technically we should chdir to the fs root
		 * to avoid making random filesystems busy, but some
		 * pathnames may be relative and we open them later,
		 * so we don't do that for now */

		return 0;
	}

	if (pid == -1) {
		int err = errno;

		error("could not fork: %s", strerror(err));
		return -err;
	}

	return pid;
}

