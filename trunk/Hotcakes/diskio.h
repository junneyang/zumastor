#define _XOPEN_SOURCE 500 /* pread */

#include <unistd.h>
#include <errno.h>
#include "trace.h"


int diskio(int fd, void *data, size_t count, off_t offset, int writeflag)
{
	ssize_t retval;

	while (count) {
		if (writeflag)
			retval = pwrite(fd, data, count, offset);
		else
			retval = pread(fd, data, count, offset);

		if (retval == -1) {
			warn("%s failed %s", writeflag ? "write" : "read", strerror(errno));
			return -errno;
		}

		if (retval == 0) {
			warn("short %s", writeflag ? "write" : "read");
			return -ERANGE;
		}

		data += retval;
		offset += retval;
		count -= retval;
	}

	return 0;
}

