#define _XOPEN_SOURCE 500 /* pwrite */
#include <unistd.h>
#include <errno.h> 
#include "trace.h"

/* Sane pread/pwrite wrapper */

int diskio(int fd, void *data, size_t count, off_t offset, int write)
{
	while (count) {
		ssize_t ret;

		if (write)
			ret = pwrite(fd, data, count, offset);
		else
			ret = pread(fd, data, count, offset);

		if (ret == -1)
			return -errno;

		if (ret == 0)
			return -ERANGE;

		data += ret;
		offset += ret;
		count -= ret;
	}

	return 0;
}

#if 0 // these should be wrappers -- daniel
int fdread(int fd, void *data, size_t count)
{
	ssize_t ret;

	while (count) {
		ret = read(fd, data, count);

		if (ret == -1) {
			warn("%s failed %s", "read", strerror(errno));
			return -errno;
		}

		if (ret == 0) {
			warn("short %s", "read");
			return -ERANGE;
		}

		data += ret;
		count -= ret;
	}

	return 0;
}

int fdwrite(int fd, void const *data, size_t count)
{
	ssize_t ret;

	while (count) {
		ret = write(fd, data, count);

		if (ret == -1) {
			warn("%s failed %s", "write", strerror(errno));
			return -errno;
		}

		if (ret == 0) {
			warn("short %s", "write");
			return -ERANGE;
		}

		data += ret;
		count -= ret;
	}

	return 0;
}
#endif