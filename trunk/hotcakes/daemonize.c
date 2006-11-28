#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include "trace.h"
#include "daemonize.h"

/* FIXME: handle log file rotations on SIGHUP */

pid_t daemonize(char const *logfile, char const *pidfile)
{
	struct sigaction ign_sa;
	pid_t pid;

	ign_sa.sa_handler = SIG_IGN;
	sigemptyset(&ign_sa.sa_mask);
	ign_sa.sa_flags = 0;

	if (sigaction(SIGCHLD, &ign_sa, NULL) == -1)
		warn("could not disable SIGCHLD: %s", strerror(errno));

	if (sigaction(SIGPIPE, &ign_sa, NULL) == -1)
		warn("could not disable SIGPIPE: %s", strerror(errno));

	fflush(stdout);

	pid = fork();

	if (pid == 0) {
		setpgid(0, 0);

		/* we should close all open file descriptors but the
		 * three standard descriptors should be the only ones
		 * open at this point and they are replaced by freopen */

		if (!logfile)
			logfile = "/dev/null";

		setvbuf(stdout, NULL, _IONBF, 0);
		setvbuf(stderr, NULL, _IONBF, 0);

		if (freopen("/dev/null", "r", stdin) == NULL)
			error("could not reopen stdin\n");
		if (freopen(logfile, "a", stderr) == NULL)
			error("could not reopen stderr\n");

		dup2(fileno(stderr), fileno(stdout));

		setvbuf(stdout, NULL, _IOLBF, 0);
		setvbuf(stderr, NULL, _IOLBF, 0);

		/* FIXME: technically we should chdir to the fs root
		 * to avoid making random filesystems busy, but some
		 * pathnames may be relative and we open them later,
		 * so we don't do that for now */

		if (pidfile) {
			FILE *fp;

			if (!(fp = fopen(pidfile, "w"))) {
				warn("could not open pid file \"%s\" for writing: %s", pidfile, strerror(errno));
			} else {
				if (fprintf(fp, "%lu\n", (unsigned long)pid) < 0)
					warn("could not write pid file \"%s\": %s", pidfile, strerror(errno));
				if (fclose(fp) < 0)
					warn("error while closing pid file \"%s\" after writing: %s", pidfile, strerror(errno));
			}
		}

		if (logfile)
		{
			time_t now;
			char *ctime_str;
		       
			now = time(NULL);
			ctime_str = ctime(&now);
			if (ctime_str[strlen(ctime_str)-1] == '\n')
				ctime_str[strlen(ctime_str)-1] = '\0';

			warn("starting at %s", ctime_str);
		}

		return 0;
	}

	if (pid == -1) {
		int err = errno;

		error("could not fork: %s", strerror(err));
		return -err;
	}

	return pid;
}

