#define _XOPEN_SOURCE 500 /* pread */
#define _GNU_SOURCE /* strnlen  */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <popt.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <zlib.h>
#include "ddsnap.h"
#include "ddsnap.common.h"
#include "dm-ddsnap.h"
#include "trace.h"
#include "sock.h"
#include "delta.h"
#include "daemonize.h"

/* changelist and delta file header info */
#define MAGIC_SIZE 8
#define CHANGELIST_MAGIC_ID "rln"
#define DELTA_MAGIC_ID "jc"
#define MAGIC_NUM 0xbead0023

#define DEFAULT_REPLICATION_PORT 4321
#define TRUE 1
#define FALSE 0

#define XDELTA 1
#define RAW (1 << 1)
#define TEST (1 << 2)

struct cl_header {
	char magic[MAGIC_SIZE];
};

struct delta_header {
	char magic[MAGIC_SIZE];
	u64 chunk_num;
	u32 chunk_size;
	u32 mode;
};

struct delta_chunk_header {
	u32 magic_num;
	u32 data_length;
	u64 chunk_addr;
	u64 check_sum;
	u32 compress;
};

static int eek(void) {
	error("%s (%i)", strerror(errno), errno);
	return 1;
}

static u64 checksum(const unsigned char *data, u32 data_length) {
	u64 result = 0;
	u32 i;

	for (i = 0; i < data_length; i++)
		result = result + data[i];

	return result;
}

static int create_socket(char const *sockname) {

	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int addr_len = sizeof(addr) - sizeof(addr.sun_path) + strlen(sockname);
	int sock;

	if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		error("Can't get AF_UNIX socket: %s", strerror(errno));
	strncpy(addr.sun_path, sockname, sizeof(addr.sun_path));
	if (sockname[0] == '@')
		addr.sun_path[0] = 0;

	if (connect(sock, (struct sockaddr *)&addr, addr_len) == -1)
		error("Can't connect to control socket %s: %s", sockname, strerror(errno));

	return sock;
}


static struct change_list *read_changelist(int cl_fd)
{
	struct cl_header clh;

	if (read(cl_fd, &clh, sizeof(clh)) != sizeof(clh)) { /* FIXME: need something like diskio */
		error("Not a proper changelist file (too short).\n");
		return NULL;
	}

	if (strncmp(clh.magic, CHANGELIST_MAGIC_ID, MAGIC_SIZE) != 0) {
		printf("Not a proper changelist file (wrong magic in header: %s).\n", clh.magic);
		return NULL;
	}

	u32 chunksize_bits;

	if (read(cl_fd, &chunksize_bits, sizeof(u32)) != sizeof(u32)) { /* FIXME: need something like diskio */
		error("Not a proper changelist file (no chunksize).\n");
		return NULL;
	}

	struct change_list *cl;

	if ((cl = init_change_list(chunksize_bits)) == NULL)
		return NULL;

	u64 chunkaddr = 0;
	ssize_t len;

	trace_on(printf("reading changelist from file\n"););
	for (;;) {
		len = read(cl_fd, &chunkaddr, sizeof(u64)); /* FIXME: need something like diskio */

		if (len == 0)
			break;

		if (len != sizeof(u64)) {
			error("Incomplete chunk address.\n");
			break;
		}

		if (chunkaddr == -1)
			break;

		if (append_change_list(cl, chunkaddr) < 0)
			error("unable to append chunk address.\n");
	}

	if (chunkaddr != -1)
		warn("changelist file may be incomplete");

	trace_on(printf("done reading "U64FMT" chunk addresses\n", cl->count););

	return cl;
}

static int write_changelist(int change_fd, struct change_list const *cl)
{
	struct cl_header clh = { };

	strncpy(clh.magic, CHANGELIST_MAGIC_ID, sizeof(clh.magic));

	if (write(change_fd, &clh, sizeof(clh)) < 0)
		error("unable to write magic information to changelist file");

	if (write(change_fd, &cl->chunksize_bits, sizeof(cl->chunksize_bits)) < 0)
		error("unable to write chunk size to changelist file");

	if (write(change_fd, cl->chunks, cl->count * sizeof(cl->chunks[0])) < 0)
		error("unable to write changelist file");

	u64 marker = -1;

	if (write(change_fd, &marker, sizeof(marker)) < 0)
		error("unable to write changelist marker");

	return 0;
}

static int generate_delta_chunks(u32 mode, int level, struct change_list *cl, int deltafile, char const *dev1name, char const *dev2name, int progress) {
	int snapdev1, snapdev2;

	snapdev1 = open(dev1name, O_RDONLY);
	if (snapdev1 < 0) {
		int err = -errno;
		printf("Could not open snap device \"%s\" for reading: %s\n", dev1name, strerror(errno));
		return err;
	}

	snapdev2 = open(dev2name, O_RDONLY);
	if (snapdev2 < 0) {
		int err = -errno;
		printf("Could not open snap device \"%s\" for reading: %s\n", dev2name, strerror(errno));
		close(snapdev1);
		return err;
	}

	trace_on(printf("opened snapshot devices snap1=%d snap2=%d to create delta\n", snapdev1, snapdev2););

	/* Variable set up */

	u32 chunk_size;
	unsigned long comp_size;

	chunk_size = 1 << cl->chunksize_bits;
	comp_size = chunk_size*2;

	printf("dev1name: %s\n", dev1name);
	printf("dev2name: %s\n", dev2name);
	printf("mode: %u\n", mode);
	printf("level: %d\n", level);
	printf("chunksize bits: %u\t", cl->chunksize_bits);
	printf("chunksize: %u\n", chunk_size);
	printf("comp_size: %lu\n", comp_size);
	printf("chunk_count: "U64FMT"\n", cl->count);

	trace_on(printf("allocating memory\n"););

	unsigned char *chunk_data1, *chunk_data2, *delta_data, *comp_delta;

	chunk_data1 = (unsigned char *)malloc(chunk_size);
	chunk_data2 = (unsigned char *)malloc(chunk_size);
	delta_data  = (unsigned char *)malloc(chunk_size);
	comp_delta  = (unsigned char *)malloc(comp_size);

	struct delta_chunk_header dch = { .magic_num = MAGIC_NUM };
	u64 chunkaddr, chunk_num;
	u32 delta_size;
        int ret = 0;

	/* Chunk address followed by CHUNK_SIZE bytes of chunk data */
	for (chunk_num = 0; chunk_num < cl->count; chunk_num++) {
		if (progress) {
			printf("\rgenerating chunk "U64FMT"/"U64FMT" ("U64FMT"%%)", chunk_num + 1, cl->count, (chunk_num * 100) / (cl->count - 1));
			fflush(stdout);
		}

		chunkaddr = cl->chunks[chunk_num] << cl->chunksize_bits;

		trace_off(printf("reading data to calculate chunk "U64FMT" at "U64FMT" to delta\n", chunk_num, chunkaddr););

		/* read in and generate the necessary chunk information */
		if (diskio(snapdev1, chunk_data1, chunk_size, chunkaddr, 0) < 0) {
			printf("chunk_data1 for chunk "U64FMT" at "U64FMT" not read properly from snapdev1.\n", chunk_num, chunkaddr);
			goto out_error;
		}
		if (diskio(snapdev2, chunk_data2, chunk_size, chunkaddr, 0) < 0) {
			printf("chunk_data2 forchunk "U64FMT" at "U64FMT" not read properly from snapdev2.\n", chunk_num, chunkaddr);
			goto out_error;
		}

		/* 3 different modes, raw (raw snapshot2 chunk), xdelta (xdelta), test (xdelta, raw snapshot1 chunk & raw snapshot2 chunk) */
		if (mode == RAW) {
			memcpy(delta_data, chunk_data2, chunk_size);
			delta_size = chunk_size;
		} else {
			ret = create_delta_chunk(chunk_data1, chunk_data2, delta_data, chunk_size, (int *)&delta_size);

			/* If delta is larger than chunk_size, we want to just copy over the raw chunk */
			if (ret == BUFFER_SIZE_ERROR) {
				memcpy(delta_data, chunk_data2, chunk_size);
				delta_size = chunk_size;
			}
			else if (ret < 0) {
				printf("Delta for chunk "U64FMT" at "U64FMT" was not generated properly.\n", chunk_num, chunkaddr);
				goto out_error;
			}
			if (mode == TEST && ret != BUFFER_SIZE_ERROR && ret > 0) {
				/* sanity test for delta creation */
				unsigned char *delta_test = (unsigned char *)malloc(chunk_size);
				ret = apply_delta_chunk(chunk_data1, delta_test, delta_data, chunk_size, delta_size);

				if (ret != chunk_size) {
					free(delta_test);
					printf("Unable to create delta.\n");
					goto out_error;
				}
				
				if (checksum((const unsigned char *)delta_test, chunk_size) 
				    != checksum((const unsigned char *)chunk_data2, chunk_size))
					printf("checksum of delta_test does not match check_sum of chunk_data2");
				
				if (memcmp(delta_test, chunk_data2, chunk_size) != 0) {
					free(delta_test);
					printf("Generated delta does not match chunk on disk. \n");
					goto out_error;
				}
				printf("Able to generate delta\n");
				free(delta_test);
			}
		}
		
		/* zlib compression */
		comp_size = chunk_size*2;
		int comp_ret = compress2(comp_delta, &comp_size, delta_data, delta_size, level);
		if (comp_ret == Z_MEM_ERROR) {
			printf("Not enough buffer memory for compression.\n");
			goto out_error;
		}
		if (comp_ret == Z_BUF_ERROR) {
			printf("Not enough room in the output buffer for compression.\n");
			goto out_error;
		}
		if (comp_ret == Z_STREAM_ERROR) {
			printf("Parameter is invalid: level=%d delta_size=%u\n", level, delta_size);
			goto out_error;
		}
		
		dch.check_sum = checksum((const unsigned char *)chunk_data2, chunk_size);
		dch.chunk_addr = chunkaddr;

		if (comp_size < delta_size) {
			dch.compress = TRUE;
			dch.data_length = comp_size;
		} else {
			dch.compress = FALSE;
			dch.data_length = delta_size;
		}
		
		/* write the chunk header and chunk delta data to the delta file*/
		trace_off(printf("writing chunk "U64FMT" at "U64FMT" to delta\n", chunk_num, chunkaddr););
		if (write(deltafile, &dch, sizeof(dch)) != sizeof(dch)) { /* FIXME: need something like diskio */
			printf("delta_chunk_header was not written properly to deltafile.\n");
			goto out_error;
		}
		
		if (dch.compress == TRUE) {
			if (write(deltafile, comp_delta, comp_size) != comp_size) {
				printf("comp_delta was not written properly to deltafile.\n");
				goto out_error;
			}
		} else {
			if (write(deltafile, delta_data, delta_size) != delta_size) {
				printf("delta_data was not written properly to deltafile.\n");
				goto out_error;
			}
		}
		
                if (mode == TEST) {
			write(deltafile, chunk_data1, chunk_size);
			write(deltafile, chunk_data2, chunk_size);
                }
	}
	printf("\n");

	/* Make sure everything in changelist was properly transmitted */
	if (chunk_num != cl->count) {
		fprintf(stderr, "Changelist was not fully transmitted.\n");
		goto out_error;
	}

	close(snapdev1);
	close(snapdev2);

	trace_on(printf("all chunks written to delta\n"););

	return 0;

out_error:
	close(snapdev1);
	close(snapdev2);

	return -1; /* FIXME: use named error */
}

static int generate_delta(u32 mode, int level, struct change_list *cl, int deltafile, char const *dev1name, char const *dev2name)
{
	/* Delta header set-up */
	struct delta_header dh;

	strncpy(dh.magic, DELTA_MAGIC_ID, sizeof(dh.magic));
	dh.chunk_num = cl->count;
	dh.chunk_size = 1 << cl->chunksize_bits;
	dh.mode = mode;

	trace_on(fprintf(stderr, "writing delta file with chunk_num="U64FMT" chunk_size=%u mode=%u\n", dh.chunk_num, dh.chunk_size, dh.mode););
	if (write(deltafile, &dh, sizeof(dh)) < sizeof(dh))
		return -errno;

	int err;

	if ((err = generate_delta_chunks(mode, level, cl, deltafile, dev1name, dev2name, TRUE)) < 0)
		return err;

	return 0;
}

static int ddsnap_generate_delta(u32 mode, int level, char const *changelistname, char const *deltaname, char const *dev1name, char const *dev2name)
{
	int clfile = open(changelistname, O_RDONLY);
	if (clfile < 0) {
		fprintf(stderr, "Could not open changelist file \"%s\" for reading.\n", changelistname);
		return 1;
	}

	struct change_list *cl;

	if ((cl = read_changelist(clfile)) == NULL) {
		fprintf(stderr, "Unable to parse changelist file %s.\n", changelistname);
		close(clfile);
		return 1;
	}

	close(clfile);

	int deltafile = open(deltaname, O_CREAT|O_WRONLY|O_TRUNC, S_IRWXU);
	if (deltafile < 0) {
		fprintf(stderr, "Could not create delta file \"%s\"\n", deltaname);
		free_change_list(cl);
		return 1;
	}

	if (generate_delta(mode, level, cl, deltafile, dev1name, dev2name) < 0) {
		close(deltafile);
		free_change_list(cl);
		fprintf(stderr, "Could not write delta file \"%s\"\n", deltaname);
		return 1;
	}

	close(deltafile);
	free_change_list(cl);

	return 0;
}

static struct change_list *stream_changelist(int serv_fd, int snap1, int snap2)
{
	int err;

	if ((err = outbead(serv_fd, STREAM_CHANGE_LIST, struct stream_changelist, snap1, snap2))) {
		error("%s (%i)", strerror(-err), -err);
		return NULL;
	}

	struct head head;

	if (readpipe(serv_fd, &head, sizeof(head)))
		error("%s (%i)", strerror(errno), errno);

	trace_on(printf("reply = %x\n", head.code););

	if (head.code != REPLY_STREAM_CHANGE_LIST)
		error("received unexpected code=%u length=%u", head.code, head.length);

	struct changelist_stream cl_head;

	if (head.length != sizeof(cl_head))
		error("reply length mismatch: expected %u, actual %u", sizeof(cl_head), head.length);

	if (readpipe(serv_fd, &cl_head, sizeof(cl_head)))
		error("%s (%i)", strerror(errno), errno);

	struct change_list *cl;

	if ((cl = malloc(sizeof(struct change_list))) == NULL) {
		/* FIXME: need to read the data anyway to clear the socket */
		return NULL;
	}

	cl->count = cl_head.chunk_count;
	cl->chunksize_bits = cl_head.chunksize_bits;

	if (cl->chunksize_bits == 0) {
		error("invalid chunk size %u in REPLY_STREAM_CHANGE_LIST", cl->chunksize_bits);
		/* FIXME: need to read the data anyway to clear the socket */
		free(cl);
		return NULL;
	}

	if (cl->count == 0) {
		cl->chunks = NULL;
		return cl;
	}

	if ((cl->chunks = malloc(cl->count * sizeof(cl->chunks[0]))) == NULL) {
		/* FIXME: need to read the data anyway to clear the socket */
		free(cl);
		return NULL;
	}

	trace_on(printf("reading "U64FMT" chunk addresses (%u bits) from ddsnapd\n", cl->count, cl->chunksize_bits););
	if (readpipe(serv_fd, cl->chunks, cl->count * sizeof(cl->chunks[0])))
		error("%s (%i)", strerror(errno), errno);

	return cl;
}

static int ddsnap_send_delta(int serv_fd, int snap1, int snap2, char const *snapdev1, char const *snapdev2, int remsnap, u32 mode, int level, int ds_fd)
{
	struct change_list *cl;

	trace_on(printf("requesting changelist from snapshot %d to %d\n", snap1, snap2););

	if ((cl = stream_changelist(serv_fd, snap1, snap2)) == NULL) {
		fprintf(stderr, "could not receive change list for snapshots %d and %d\n", snap1, snap2);
		return 1;
	}

	trace_on(fprintf(stderr, "got changelist, sending upload request\n"););

	/* request approval for delta send */

	int err;

	if ((err = outbead(ds_fd, SEND_DELTA, struct send_delta, remsnap, cl->count, 1 << cl->chunksize_bits, mode))) {
		error("%s (%i)", strerror(-err), -err);
		return 1;
	}

	struct head head;

	trace_on(fprintf(stderr, "waiting for response\n"););

	if (readpipe(ds_fd, &head, sizeof(head)))
		return eek();

	trace_on(printf("reply = %x\n", head.code););

	if (head.code != SEND_DELTA_PROCEED)
		error("received unexpected code=%u length=%u", head.code, head.length);

	trace_on(fprintf(stderr, "sending delta\n"););

	/* stream delta */

	if (generate_delta_chunks(mode, level, cl, ds_fd, snapdev1, snapdev2, TRUE) < 0) {
		fprintf(stderr, "could not send delta for snapshot devices %s and %s downstream\n", snapdev1, snapdev2);
		return 1;
	}

	trace_on(fprintf(stderr, "waiting for response\n"););

	if (readpipe(ds_fd, &head, sizeof(head)))
		return eek();

	trace_on(printf("reply = %x\n", head.code););

	if (head.code != SEND_DELTA_DONE)
		error("received unexpected code=%u length=%u", head.code, head.length);

	/* success */

	trace_on(fprintf(stderr, "downstream server successfully applied delta to snapshot %d\n", remsnap););

	return 0;
}

static int apply_delta_chunks(int deltafile, u32 mode, u32 chunk_size, u64 chunk_count, char const *devname) {
	int snapdev;

	snapdev = open(devname, O_RDWR); /* FIXME: why not O_WRONLY? */
	if (snapdev < 0) {
		int err = -errno;
		fprintf(stderr, "could not open snapdev file \"%s\" for writing.\n", devname);
		return err;
	}

	unsigned char *chunk_data, *delta_data, *updated, *comp_delta;
        char *up_chunk1, *up_chunk2;

	chunk_data = (unsigned char *)malloc(chunk_size);
	delta_data = (unsigned char *)malloc(chunk_size*2);
	updated    = (unsigned char *)malloc(chunk_size);
	comp_delta = (unsigned char *)malloc(chunk_size);

        up_chunk1  = (char *)malloc(chunk_size);
        up_chunk2  = (char *)malloc(chunk_size);

        printf("device: %s\n", devname);
        printf("mode: %u\n", mode);
        printf("chunk_count: "U64FMT"\n", chunk_count);

	struct delta_chunk_header dch = { };
	unsigned long uncomp_size;
	u64 chunkaddr, chunk_num;
        int c1_chk_sum = 0;
	int ret = 0;

	for (chunk_num = 0; chunk_num < chunk_count; chunk_num++) {

		trace_on(printf("reading chunk "U64FMT" header\n", chunk_num););
		if (read(deltafile, &dch, sizeof(dch)) != sizeof(dch)) { /* FIXME: need something like diskio */
			fprintf(stderr, "Error reading chunk "U64FMT", expected "U64FMT" chunks.\n", chunk_num, chunk_count);
			close(snapdev);
			return -2; /* FIXME: use named error */
		}

		if (dch.magic_num != MAGIC_NUM) {
			fprintf(stderr, "Not a proper delta file (magic_num doesn't match).\n");
			close(snapdev);
			return -1;
		}

		trace_on(printf("data length is %u (buffer is %u)\n", dch.data_length, chunk_size);); errno = 0;

		if (dch.compress == TRUE) {
			if (read(deltafile, comp_delta, dch.data_length) != dch.data_length) { /* FIXME: need something like diskio */
				fprintf(stderr, "Could not properly read comp_delta from deltafile: %s.\n", strerror(errno));
				close(snapdev);
				return -1;
			}
		} else {
			if (read(deltafile, delta_data, dch.data_length) != dch.data_length) { /* FIXME: need something like diskio */
				fprintf(stderr, "Could not properly read delta_data from deltafile: %s.\n", strerror(errno));
				close(snapdev);
				return -1;
			}
		}

		chunkaddr = dch.chunk_addr;
		trace_on(printf("reading chunk "U64FMT" data at "U64FMT"\n", chunk_num, chunkaddr););
		if (diskio(snapdev, chunk_data, chunk_size, chunkaddr, 0) < 0) {
			fprintf(stderr, "Snapdev reading of downstream chunk1 has failed.\n");
			close(snapdev);
			return -1;
		}

		if (dch.compress == TRUE) {
			printf("data was compressed \n");

			/* zlib decompression */
			uncomp_size = chunk_size*2;
			int comp_ret = uncompress(delta_data, &uncomp_size, comp_delta, dch.data_length);
			if (comp_ret == Z_MEM_ERROR) {
				fprintf(stderr, "Not enough buffer memory for decompression.\n");
				close(snapdev);
				return -1;
			}
			if (comp_ret == Z_BUF_ERROR) {
				fprintf(stderr, "Not enough room in the output buffer for decompression.\n");
				close(snapdev);
				return -1;
			}
			if (comp_ret == Z_DATA_ERROR) {
				fprintf(stderr, "The input data was corrupted for decompression.\n");
				close(snapdev);
				return -1;
			}
		} else
			uncomp_size = dch.data_length;

                if (mode == RAW) {
			memcpy(updated, delta_data, chunk_size);
		} else {
			if (uncomp_size == chunk_size)
				memcpy(updated, delta_data, chunk_size);
			else
				ret = apply_delta_chunk(chunk_data, updated, delta_data, chunk_size, uncomp_size);
			
			printf("uncomp_size %lu & dch.data_length %u\n", uncomp_size, dch.data_length);
			printf("ret %d\n", ret);
			
			if (ret < 0) {
				printf("Delta for chunk address "U64FMT" was not applied properly.\n", chunkaddr);
				close(snapdev);
				return -1;
			}
			if (mode == TEST) {
				if (read(deltafile, up_chunk1, chunk_size) != chunk_size) {
					printf("up_chunk1 not read properly from deltafile. \n");
					return -1;
				}
				if (read(deltafile, up_chunk2, chunk_size) != chunk_size) {
					printf("up_chunk2 not read properly from deltafile. \n");
					return -1;
				}
				c1_chk_sum = checksum((const unsigned char *)chunk_data, chunk_size);
			}
			if (dch.check_sum != checksum((const unsigned char *)updated, chunk_size)) {
				printf("Check_sum failed for chunk address "U64FMT"\n", chunkaddr);
				if (mode == TEST) {
					/* sanity check: does the checksum of upstream chunk1 = checksum of downstream chunk1? */
					if (c1_chk_sum != checksum((const unsigned char *)up_chunk1, chunk_size)) {
						printf("check_sum of chunk1 doesn't match for address "U64FMT"\n", chunkaddr);
						if (dch.data_length == chunk_size)
							memcpy(updated, delta_data, chunk_size);
						else
							ret = apply_delta_chunk(up_chunk1, updated, delta_data, chunk_size, dch.data_length);
						
						if (ret < 0)
							printf("Delta for chunk address "U64FMT" with upstream chunk1 was not applied properly.\n", chunkaddr);
						
						if (dch.check_sum != checksum((const unsigned char *)updated, chunk_size)) {
							printf("Check_sum of apply delta onto upstream chunk1 failed for chunk address "U64FMT"\n", chunkaddr);
							memcpy(updated, up_chunk2, chunk_size);
						}
					} else {
						printf("apply delta doesn't work; check_sum of chunk1 matches for address "U64FMT"\n", chunkaddr);
						if (memcmp(chunk_data, up_chunk1, chunk_size) != 0)
							printf("chunk_data for chunk1 does not match. \n");
						else
							printf("chunk_data for chunk1 does matche up. \n");
						memcpy(updated, up_chunk2, chunk_size);
					}
				} else {
					close(snapdev);
					return -1;
				}
			}
                }
		
		if (diskio(snapdev, updated, chunk_size, chunkaddr, 1) < 0) {
			printf("updated was not written properly at "U64FMT" in snapdev.\n", chunkaddr);
			return -1;
		}
	}

	close(snapdev);
	return 0;
}

static int apply_delta(int deltafile, char const *devname)
{
	struct delta_header dh;

	if (read(deltafile, &dh, sizeof(dh)) != sizeof(dh)) {
		fprintf(stderr, "Not a proper delta file (too short).\n");
		return -1; /* FIXME: use named error */
	}

	/* Make sure it's a proper delta file */
	if (strncmp(dh.magic, DELTA_MAGIC_ID, MAGIC_SIZE) != 0) {
		fprintf(stderr, "Not a proper delta file (wrong magic in header).\n");
		return -1; /* FIXME: use named error */
	}

	if (dh.chunk_size == 0) {
		fprintf(stderr, "Not a proper delta file (zero chunk size).\n");
		return -1; /* FIXME: use named error */
	}

	int err;
       
	if ((err = apply_delta_chunks(deltafile, dh.mode, dh.chunk_size, dh.chunk_num, devname)) < 0)
		return err;

	return 0;
}

static int ddsnap_apply_delta(char const *deltaname, char const *devname)
{
	int deltafile;

	deltafile = open(deltaname, O_RDONLY);
	if (deltafile < 0) {
		fprintf(stderr, "Could not open delta file \"%s\" for reading.\n", deltaname);
		return 1;
	}

	if (apply_delta(deltafile, devname) < 0) {
		fprintf(stderr, "Could not apply delta file \"%s\" to snapdev \"%s\"\n", deltaname, devname);
		close(deltafile);
		return 1;
	}

	char test;

	if (read(deltafile, &test, 1) == 1) {
		fprintf(stderr, "Extra data at end of delta file\n");
	}

	close(deltafile);

	return 0;
}

static int list_snapshots(int serv_fd)
{
	int err;

	if ((err = outbead(serv_fd, LIST_SNAPSHOTS, struct create_snapshot, 0))) {
		error("%s (%i)", strerror(-err), -err);
		return 1;
	}

	struct head head;

	if (readpipe(serv_fd, &head, sizeof(head)))
		return eek();

	trace_on(printf("reply = %x\n", head.code););

	if (head.code != SNAPSHOT_LIST)
		error("received unexpected code=%u length=%u", head.code, head.length);

	if (head.length < sizeof(int32_t))
		error("reply length mismatch: expected >=%u, actual %u", sizeof(int32_t), head.length);

	int count;

	if (readpipe(serv_fd, &count, sizeof(int)))
		return eek();

	if (head.length != sizeof(int32_t) + count * sizeof(struct snapinfo))
		error("reply length mismatch: expected %u, actual %u", sizeof(int32_t) + count * sizeof(struct snapinfo), head.length);

	struct snapinfo * buffer = (struct snapinfo *)malloc(count * sizeof(struct snapinfo));

	if (readpipe(serv_fd, buffer, count * sizeof(struct snapinfo)))
		return eek();

	printf("Snapshot list:\n");

	int i;

	for (i = 0; i < count; i++) {
		time_t snap_time = (time_t)buffer[i].ctime;

		printf("Snapshot[%d]:\n", i);
		printf("\ttag= "U64FMT" \t", buffer[i].snap);
		printf("priority= %d \t", buffer[i].prio);
		printf("use count= %d \t", buffer[i].usecnt);
		printf("creation time= %s\n", ctime(&snap_time));
	}

	return 0;
}

static int ddsnap_generate_changelist(int serv_fd, char const *changelist_filename, int snap1, int snap2)
{
	int change_fd = open(changelist_filename, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);

	if (change_fd < 0)
		error("unable to open file %s: %s", changelist_filename, strerror(errno));

	struct change_list *cl;

	if ((cl = stream_changelist(serv_fd, snap1, snap2)) == NULL) {
		fprintf(stderr, "could not generate change list between snapshots %d and %d\n", snap1, snap2);
		return 1;
	}

	int err = write_changelist(change_fd, cl);

	close(change_fd);

	if (err < 0)
	    return 1;

	return 0;
}

static int delete_snapshot(int sock, int snap) {
	if (outbead(sock, DELETE_SNAPSHOT, struct create_snapshot, snap) < 0)
		return eek();

	struct head head;
	unsigned maxbuf = 500;
	char buf[maxbuf];

	if (readpipe(sock, &head, sizeof(head)))
		return eek();
	assert(head.length < maxbuf); // !!! don't die
	trace_on(printf("reply head.length = %x\n", head.length););
	if (readpipe(sock, buf, head.length))
		return eek();

	trace_on(printf("reply = %x\n", head.code););

	if (head.code != REPLY_DELETE_SNAPSHOT)
		error("received unexpected code: %.*s", head.length - 4, buf + 4);

	return 0;
}

static int create_snapshot(int sock, int snap) {
	if (outbead(sock, CREATE_SNAPSHOT, struct create_snapshot, snap) < 0)
		return eek();

	struct head head;
	unsigned maxbuf = 500;
	char buf[maxbuf];

	if (readpipe(sock, &head, sizeof(head)))
		return eek();
	assert(head.length < maxbuf); // !!! don't die
	if (readpipe(sock, buf, head.length))
		return eek();

	trace_on(printf("reply = %x\n", head.code););

	if (head.code != REPLY_CREATE_SNAPSHOT)
		error("received unexpected code: %.*s", head.length - 4, buf + 4);

	return 0;
}

static int set_priority(int sock, uint32_t tag_val, int8_t pri_val) {
	if (outbead(sock, SET_PRIORITY, struct snapinfo, tag_val, pri_val) < 0)
		return eek();

	struct head head;
	unsigned maxbuf = 500;
	char buf[maxbuf];

	if (readpipe(sock, &head, sizeof(head)))
		return eek();
	assert(head.length < maxbuf); // !!! don't die
	if (readpipe(sock, buf, head.length))
		return eek();

	trace_on(printf("reply = %x\n", head.code););

	if (head.code != REPLY_SET_PRIORITY)
		error("received unexpected code: %.*s", head.length - 4, buf + 4);

	return 0;
}

static int set_usecount(int sock, uint32_t tag_val, const char * op) {
	int usecnt = (strcmp(op, "inc") == 0) ? 1 : ((strcmp(op, "dec") == 0) ? -1 : 0);

	if (outbead(sock, SET_USECOUNT, struct snapinfo, tag_val, 0, usecnt) < 0)
		return eek();

	return 0;
}

static int ddsnap_daemon(int lsock, char const *snapdevstem)
{
	for (;;) {
		int csock;

		if ((csock = accept_socket(lsock)) < 0) {
			fprintf(stderr, "unable to accept connection: %s\n", strerror(errno));
			continue;
		}

		trace_on(fprintf(stderr, "got client connection\n"););

		pid_t pid;

		if ((pid = fork()) < 0) {
			fprintf(stderr, "unable to fork to service connection: %s\n", strerror(errno));
			goto cleanup_connection;
		}

		if (pid != 0) {
			/* parent -- wait for another connection */
			close(csock);
			continue;
		}

		trace_on(fprintf(stderr, "processing\n"););

		/* child */

		struct messagebuf message;
		int err;

		if ((err = readpipe(csock, &message.head, sizeof(message.head)))) {
			fprintf(stderr, "error reading upstream message header: %s\n", strerror(-err));
			goto cleanup_connection;
		}
		if (message.head.length > maxbody) {
			fprintf(stderr, "message body too long %d\n", message.head.length);
			goto cleanup_connection;
		}
		if ((err = readpipe(csock, &message.body, message.head.length))) {
			fprintf(stderr, "error reading upstream message body: %s\n", strerror(-err));
			goto cleanup_connection;
		}

		struct send_delta body;

		switch (message.head.code) {
		case SEND_DELTA:
			if (message.head.length < sizeof(body)) {
				fprintf(stderr, "incomplete SEND_DELTA request sent by client\n");
				outbead(csock, REPLY_ERROR, struct reply_error, REPLY_ERROR_OTHER, 0);
				goto cleanup_connection;
			}

			memcpy(&body, message.body, sizeof(body));

			if (body.snapid < -1 || body.snapid >= MAX_SNAPSHOTS) {
				fprintf(stderr, "invalid snapshot id %d in SEND_DELTA\n", body.snapid);
				outbead(csock, REPLY_ERROR, struct reply_error, REPLY_ERROR_OTHER, 0);
				goto cleanup_connection;
			}

			if (body.chunk_size == 0) {
				fprintf(stderr, "invalid chunk size %u in SEND_DELTA\n", body.chunk_size);
				outbead(csock, REPLY_ERROR, struct reply_error, REPLY_ERROR_OTHER, 0);
				goto cleanup_connection;
			}

			char *remotedev;

			if (!(remotedev = malloc(strlen(snapdevstem)+32+1))) {
				fprintf(stderr, "incomplete SEND_DELTA request sent by client\n");
				outbead(csock, REPLY_ERROR, struct reply_error, REPLY_ERROR_OTHER, 0);
				goto cleanup_connection;
			}
			sprintf(remotedev, "%s%d", snapdevstem, body.snapid);

			/* FIXME: verify snapshot exists */

			/* FIXME: In the future we should also lookup the client's address in a
			 * device permission table and check for replicatiosn already in progress.
			 */

			outbead(csock, SEND_DELTA_PROCEED, struct {});

			/* retrieve it */

			if (apply_delta_chunks(csock, body.delta_mode, body.chunk_size, body.chunk_count, remotedev) < 0) {
				fprintf(stderr, "unable to apply upstream delta to device %s\n", remotedev);
				outbead(csock, REPLY_ERROR, struct reply_error, REPLY_ERROR_OTHER, 0);
				goto cleanup_connection;
			}

			/* success */

			outbead(csock, SEND_DELTA_DONE, struct {});
			trace_on(fprintf(stderr, "applied streamed delta to %s\n", remotedev););
			exit(0);

		default:
			fprintf(stderr, "unexpected message type sent to snapshot replication server %d\n", message.head.code);
			outbead(csock, REPLY_ERROR, struct reply_error, REPLY_ERROR_OTHER, 0);
			goto cleanup_connection;
		}

	cleanup_connection:
		fprintf(stderr, "closing connection\n");
		close(csock);
	}

	return 0;
}

static void mainUsage(void)
{
	printf("usage: ddsnap [-?|--help|--usage] <subcommand>\n"
		"\n"
		"Available subcommands:\n"
		"	create-snap       Creates a snapshot\n"
		"	delete-snap       Deletes a snapshot\n"
		"	list              Returns a list of snapshots\n"
		"	set-priority      Sets the priority of a snapshot\n"
		"	set-usecount      Sets the use count of a snapshot\n"
		"	create-cl         Creates a changelist given 2 snapshots\n"
		"	create-delta      Creates a delta file given a changelist and 2 snapshots\n"
		"	apply-delta       Applies a delta file to the given device\n"
		"	send-delta        Sends a delta file downstream\n"
		"	daemon            Listens for upstream deltas\n");
}

static void cdUsage(poptContext optCon, int exitcode, char const *error, char const *addl) {
	poptPrintUsage(optCon, stderr, 0);
	if (error) fprintf(stderr, "%s: %s", error, addl);
	exit(exitcode);
}

int main(int argc, char *argv[]) {
	char const *command;
	int xd = FALSE, raw = FALSE, test = FALSE, gzip_level = 0;

	struct poptOption noOptions[] = {
		POPT_TABLEEND
	};
	struct poptOption cdOptions[] = {
		{ "xdelta", 'x', POPT_ARG_NONE, &xd, 0, "Delta file format: xdelta chunk", NULL },
		{ "raw", 'r', POPT_ARG_NONE, &raw, 0, "Delta file format: raw chunk from later snapshot", NULL },
		{ "test", 't', POPT_ARG_NONE, &test, 0, "Delta file format: xdelta chunk, raw chunk from earlier snapshot and raw chunk from later snapshot", NULL },
		{ "gzip", 'g', POPT_ARG_INT, &gzip_level, 0, "Compression via gzip (default level: 6)", "compression_level"},
		POPT_TABLEEND
	};

	poptContext mainCon;
	struct poptOption mainOptions[] = {
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Create snapshot\n\t Function: Creates a snapshot\n\t Usage: create-snap <sockname> <snapshot>" , NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Delete snapshot\n\t Function: Deletes a snapshot\n\t Usage: delete-snap <sockname> <snapshot>" , NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "List snapshots\n\t Function: Returns a list of snapshots\n\t Usage: list <sockname>" , NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Set priority\n\t Function: Sets the priority of a snapshot\n\t Usage: set-priority <sockname> <snap_tag> <new_priority_value>" , NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Set usecount\n\t Function: Sets the use count of a snapshot\n\t Usage: set-usecount <sockname> <snap_tag> <inc|dec>" , NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Create changelist\n\t Function: Creates a changelist given 2 snapshots\n\t Usage: create-cl <sockname> <changelist> <snapshot1> <snapshot2>" , NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &cdOptions, 0,
		  "Create delta\n\t Function: Creates a delta file given a changelist and 2 snapshots\n\t Usage: create-delta [OPTION...] <changelist> <deltafile> <snapshot1> <snapshot2>\n" , NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Apply delta\n\t Function: Applies a delta file to the given device\n\t Usage: apply-delta <deltafile> <dev>" , NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Send delta\n\t Function: Sends a delta file downstream\n\t Usage: send-delta <sockname> <snapshot1> <snapshot2> <snapdev1> <snapdev2> <remsnapshot> <host>[:<port>]" , NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Daemon\n\t Function: Listens for upstream deltas\n\t Usage: daemon <snapdevstem> [<host>[:<port>]]" , NULL },
		POPT_AUTOHELP
		POPT_TABLEEND
	};
	mainCon = poptGetContext(NULL, argc, (const char **)argv, mainOptions, 0);

	if (argc < 2) {
		poptPrintHelp(mainCon, stdout, 0);
		exit(1);
	}

	command = argv[1];

	if (strcmp(command, "--help") == 0 || strcmp(command, "-?") == 0) {
		poptPrintHelp(mainCon, stdout, 0);
		exit(1);
	}
	if (strcmp(command, "--usage") == 0) {
		mainUsage();
		exit(1);
	}

	//poptResetContext(mainCon);
	poptFreeContext(mainCon);

	if (strcmp(command, "create-snap") == 0) {
		if (argc != 4) {
			printf("Usage: %s create-snap <sockname> <snapshot>\n", argv[0]);
			return 1;
		}
		int sock = create_socket(argv[2]);
		int ret = create_snapshot(sock, atoi(argv[3]));
		close(sock);
		return ret;
	}
	if (strcmp(command, "delete-snap") == 0) {
		if (argc != 4) {
			printf("Usage: %s delete-snap <sockname> <snapshot>\n", argv[0]);
			return 1;
		}
		int sock = create_socket(argv[2]);
		int ret = delete_snapshot(sock, atoi(argv[3]));
		close(sock);
		return ret;
	}
	if (strcmp(command, "list") == 0) {
		if (argc != 3) {
			printf("Usage: %s list <sockname>\n", argv[0]);
			return 1;
		}
		int sock = create_socket(argv[2]);
		int ret = list_snapshots(sock);
		close(sock);
		return ret;
	}
	if (strcmp(command, "set-priority") == 0) {
		if (argc != 5) {
			printf("usage: %s set-priority <sockname> <snap_tag> <new_priority_value>\n", argv[0]);
			return 1;
		}
		int sock = create_socket(argv[2]);
		int ret = set_priority(sock, atoi(argv[3]), atoi(argv[4]));
		close(sock);
		return ret;
	}
	if (strcmp(command, "set-usecount") == 0) {
		if (argc != 5) {
			printf("usage: %s set-usecount <sockname> <snap_tag> <inc|dec>\n", argv[0]);
			return 1;
		}
		int sock = create_socket(argv[2]);
		int ret = set_usecount(sock, atoi(argv[3]), argv[4]);
		close(sock);
		return ret;
	}
	if (strcmp(command, "create-cl") == 0) {
		if (argc != 6) {
			printf("usage: %s create-cl <sockname> <changelist> <snapshot1> <snapshot2>\n", argv[0]);
			return 1;
		}
		int sock = create_socket(argv[2]);
		int ret = ddsnap_generate_changelist(sock, argv[3], atoi(argv[4]), atoi(argv[5]));
		close(sock);
		return ret;
	}
	if (strcmp(command, "create-delta") == 0) {
		char cdOpt;
		char const *changelist, *deltafile, *snapdev1, *snapdev2;

		poptContext cdCon;

		struct poptOption options[] = {
			{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &cdOptions, 0, NULL, NULL },
			POPT_AUTOHELP
			POPT_TABLEEND
		};

		cdCon = poptGetContext(NULL, argc-1, (const char **)&(argv[1]), options, 0);
		poptSetOtherOptionHelp(cdCon, "<changelist> <deltafile> <snapdev1> <snapdev2>");

		cdOpt = poptGetNextOpt(cdCon);

		if (argc < 3) {
			poptPrintUsage(cdCon, stderr, 0);
			exit(1);
		}

		if (cdOpt < -1) {
			/* an error occurred during option processing */
			fprintf(stderr, "%s: %s\n",
				poptBadOption(cdCon, POPT_BADOPTION_NOALIAS),
				poptStrerror(cdOpt));
			return 1;
		}

		/* Make sure the options are mutually exclusive */
		if (xd+raw+test > 1)
			cdUsage(cdCon, 1, "Too many chunk options were selected. \nPlease select only one", "-x, -r, -t\n");

		u32 mode = (xd ? XDELTA : (raw ? RAW : TEST));
		trace_on(fprintf(stderr, "xd=%d raw=%d test=%d mode=%d\n", xd, raw, test, mode););

		changelist = poptGetArg(cdCon);
		deltafile  = poptGetArg(cdCon);
		snapdev1   = poptGetArg(cdCon);
		snapdev2   = poptGetArg(cdCon);

		if (changelist == NULL)
			cdUsage(cdCon, 1, "Specify a changelist", ".e.g., cl01 \n");
		if (deltafile == NULL)
			cdUsage(cdCon, 1, "Specify a deltafile", ".e.g., df01 \n");
		if (snapdev1 == NULL)
			cdUsage(cdCon, 1, "Specify a snapdev1", ".e.g., /dev/mapper/snap0 \n");
		if (snapdev2 == NULL)
			cdUsage(cdCon, 1, "Specify a snapdev2", ".e.g., /dev/mapper/snap1 \n");
		if (!(poptPeekArg(cdCon) == NULL))
			cdUsage(cdCon, 1, "Too many arguments inputted", "\n");

		int ret = ddsnap_generate_delta(mode, gzip_level, changelist, deltafile, snapdev1, snapdev2);

		poptFreeContext(cdCon);
		return ret;
	}
	if (strcmp(command, "apply-delta") == 0) {
		if (argc != 4) {
			printf("usage: %s apply-delta <deltafile> <dev>\n", argv[0]);
			return 1;
		}
		return ddsnap_apply_delta(argv[2], argv[3]);
	}
	if (strcmp(command, "send-delta") == 0) {
		char *hostname;
		unsigned port;

		if (argc != 9) {
			printf("usage: %s send-delta <sockname> <snapshot1> <snapshot2> <snapdev1> <snapdev2> <remsnapshot> <host>[:<port>]\n", argv[0]);
			return 1;
		}

		hostname = strdup(argv[8]);

		if (strchr(hostname, ':')) {
			unsigned int len = strlen(hostname);
			port = parse_port(hostname, &len);
			hostname[len] = '\0';
		} else {
			port = DEFAULT_REPLICATION_PORT;
		}

		int sock = create_socket(argv[2]);

		int ds_fd = open_socket(hostname, port);
		if (ds_fd < 0) {
			printf("Error: unable to connect to downstream server %s port %u\n", hostname, port);
			return 1;
		}

		u32 mode = RAW;
		int level = 0;

		int ret = ddsnap_send_delta(sock, atoi(argv[3]), atoi(argv[4]), argv[5], argv[6], atoi(argv[7]), mode, level, ds_fd);
		close(ds_fd);
		close(sock);

		return ret;
	}
	if (strcmp(command, "daemon") == 0) {
		char *hostname;
		unsigned port;

		if (argc < 3 || argc > 4) {
			printf("usage: %s daemon <snapdevstem> [<host>[:<port>]]\n", argv[0]);
			return 1;
		}

		if (argc < 4) {
			hostname = strdup("0.0.0.0");
			port = DEFAULT_REPLICATION_PORT;
		} else {
			hostname = strdup(argv[3]);

			if (strchr(hostname, ':')) {
				unsigned int len = strlen(hostname);
				port = parse_port(hostname, &len);
				hostname[len] = '\0';
			} else {
				port = DEFAULT_REPLICATION_PORT;
			}
		}

		int sock = bind_socket(hostname, port);
		if (sock < 0) {
			printf("Error: unable to bind to %s port %u\n", hostname, port);
			return 1;
		}

		pid_t pid;

		pid = daemonize("/tmp/ddsnap.log");
		if (pid == -1)
			error("Error: could not daemonize\n");
		if (pid != 0) {
			trace_on(printf("pid = %lu\n", (unsigned long)pid););
			return 0;
		}

		return ddsnap_daemon(sock, argv[2]);

	}
	printf("Unrecognized command %s.\n", command);

	return 1;
}

