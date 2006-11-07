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
#include "buffer.h"
#include "daemonize.h"
#include "ddsnap.h"
#include "ddsnap.agent.h"
#include "ddsnap.common.h"
#include "ddsnapd.h"
#include "delta.h"
#include "diskio.h"
#include "dm-ddsnap.h"
#include "list.h"
#include "sock.h"
#include "trace.h"

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
#define OPT_COMP (1 << 3)

#define DEF_GZIP_COMP 0
#define MAX_GZIP_COMP 9

#define MAX_MEM_SIZE (1 << 20)

struct cl_header {
	char magic[MAGIC_SIZE];
};

struct delta_header {
	char magic[MAGIC_SIZE];
	u64 chunk_num;
	u32 chunk_size;
	u32 mode;
};

struct delta_extent_header {
	u32 magic_num;
	u32 setting;
	u64 data_length;
	u64 extent_addr;
	u64 check_sum;
	u32 compress;
	u64 num_of_chunks;
};

static int eek(void)
{
	error("%s (%i)", strerror(errno), errno);
	return 1;
}

static u64 checksum(const unsigned char *data, u32 data_length)
{
	u64 result = 0;
	u32 i;

	for (i = 0; i < data_length; i++)
		result = result + data[i];

	return result;
}

static int create_socket(char const *sockname)
{

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

static int parse_snapid(char const *snapstr, int *snapid)
{
	if (snapstr[0] == '\0')
		return -1;

	if (strcmp(snapstr, "-1") == 0) {
		*snapid = -1;
		return 0;
	}

	unsigned long num;
	char *endptr;

	num = strtoul(snapstr, &endptr, 10);

	if (*endptr != '\0')
		return -1;

	if (num >= MAX_SNAPSHOTS)
		return -1;

	*snapid = num;
	return 0;
}

static struct change_list *read_changelist(int cl_fd)
{
	struct cl_header clh;

	if (fdread(cl_fd, &clh, sizeof(clh)) < 0) {
		error("Not a proper changelist file (too short).\n");
		return NULL;
	}

	if (strncmp(clh.magic, CHANGELIST_MAGIC_ID, MAGIC_SIZE) != 0) {
		printf("Not a proper changelist file (wrong magic in header: %s).\n", clh.magic);
		return NULL;
	}

	u32 chunksize_bits;

	if (fdread(cl_fd, &chunksize_bits, sizeof(u32)) < 0) {
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
		len = read(cl_fd, &chunkaddr, sizeof(u64)); /* FIXME: need to handle short read but fdread() doesn't distinguish short read vs. no read EOF */

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

	if (fdwrite(change_fd, &clh, sizeof(clh)) < 0)
		error("unable to write magic information to changelist file");

	if (fdwrite(change_fd, &cl->chunksize_bits, sizeof(cl->chunksize_bits)) < 0)
		error("unable to write chunk size to changelist file");

	if (fdwrite(change_fd, cl->chunks, cl->count * sizeof(cl->chunks[0])) < 0)
		error("unable to write changelist file");

	u64 marker = -1;

	if (fdwrite(change_fd, &marker, sizeof(marker)) < 0)
		error("unable to write changelist marker");

	return 0;
}

u64 chunks_in_extent(struct change_list *cl, u64 pos, u32 chunk_size)
{
	u64 start_chunkaddr, cur_chunkaddr, num_of_chunks = 1;
	start_chunkaddr = cl->chunks[pos];
	cur_chunkaddr = cl->chunks[pos+1];

	while (cur_chunkaddr == start_chunkaddr + num_of_chunks && (num_of_chunks < (MAX_MEM_SIZE / chunk_size))) {
		num_of_chunks++;
		cur_chunkaddr = cl->chunks[pos + num_of_chunks];
	}
	return num_of_chunks;
}

static int generate_delta_extents(u32 mode, int level, struct change_list *cl, int deltafile, char const *dev1name, char const *dev2name, int progress) 
{
	int snapdev1, snapdev2;
	unsigned char *extent_data1, *extent_data2, *delta_data, *comp_delta, *ext2_comp_delta;
	u64 comp_size, extent_size, ext2_comp_size;
	
	snapdev1 = open(dev1name, O_RDONLY);
	if (snapdev1 < 0) {
		int err = -errno;
		printf("Could not open snap device \"%s\" for reading: %s\n", dev1name, strerror(-err));
		return err;
	}

	snapdev2 = open(dev2name, O_RDONLY);
	if (snapdev2 < 0) {
		int err = -errno;
		printf("Could not open snap device \"%s\" for reading: %s\n", dev2name, strerror(-err));
		close(snapdev1);
		return err;
	}

	trace_on(printf("opened snapshot devices snap1=%d snap2=%d to create delta\n", snapdev1, snapdev2););

	/* Variable set up */

	u32 chunk_size;
	chunk_size = 1 << cl->chunksize_bits;

	printf("dev1name: %s\n", dev1name);
	printf("dev2name: %s\n", dev2name);
	printf("mode: %u\n", mode);
	printf("level: %d\n", level);
	printf("chunksize bits: %u\t", cl->chunksize_bits);
	printf("chunksize: %u\n", chunk_size);
	printf("chunk_count: "U64FMT"\n", cl->count);

	trace_on(printf("allocating memory\n"););

	struct delta_extent_header deh = { .magic_num = MAGIC_NUM };
	u64 extent_addr, chunk_num, num_of_chunks = 0;
	u64 delta_size;
	int ret = 0;
	
	trace_off(printf("memory allocated\n"););
								
	/* Chunk address followed by CHUNK_SIZE bytes of chunk data */
	for (chunk_num = 0; chunk_num < cl->count;) {

		extent_addr = cl->chunks[chunk_num];
		extent_addr = extent_addr << cl->chunksize_bits;

		if (chunk_num == (cl->count - 1) ) 
			num_of_chunks = 1;
		else
			num_of_chunks = chunks_in_extent(cl, chunk_num, chunk_size);

		extent_size = chunk_size * num_of_chunks;
		delta_size = extent_size;
		comp_size = extent_size + 12 + (extent_size >> 9);
		ext2_comp_size = comp_size;

		extent_data1    = malloc(extent_size);
		extent_data2    = malloc(extent_size);
		delta_data      = malloc(extent_size);
		comp_delta      = malloc(comp_size);
		ext2_comp_delta = malloc(ext2_comp_size);
		
		trace_off(printf("\nReading data to calculate "U64FMT"th chunk starting at "U64FMT" to delta\n", num_of_chunks, extent_addr););

		/* read in and generate the necessary chunk information */
		if (diskread(snapdev1, extent_data1, extent_size, extent_addr) < 0) {
			printf("\nExtent_data1 for "U64FMT"th chunk starting at "U64FMT" not read properly from snapdev1.\n", num_of_chunks, extent_addr);
			goto out_error;
		}
		if (diskread(snapdev2, extent_data2, extent_size, extent_addr) < 0) {
			printf("\nExtent_data2 for "U64FMT"th chunk starting at "U64FMT" not read properly from snapdev2.\n", num_of_chunks, extent_addr);
			goto out_error;
		}

		/* 3 different modes, raw (raw snapshot2 chunk), xdelta (xdelta), test (xdelta, raw snapshot1 chunk & raw snapshot2 chunk) */
		if (mode == RAW) 
			memcpy(delta_data, extent_data2, extent_size);		
		else {
			ret = create_delta_chunk(extent_data1, extent_data2, delta_data, extent_size, (int *)&delta_size);

			/* If delta is larger than chunk_size, we want to just copy over the raw chunk */
			if (ret == BUFFER_SIZE_ERROR) {
				trace_off(printf("Buffer size error\n"););
				memcpy(delta_data, extent_data2, extent_size);
				delta_size = extent_size;	
			} else if (ret < 0) {
				printf("\nDelta for "U64FMT"th chunk starting at "U64FMT" was not generated properly.\n", num_of_chunks, extent_addr);
				goto out_error;
			}
			if (ret >= 0) {
				/* sanity test for delta creation */
				unsigned char *delta_test = (unsigned char *)malloc (extent_size);
				ret = apply_delta_chunk(extent_data1, delta_test, delta_data, extent_size, delta_size);

				if (ret != extent_size) {
					free(delta_test);
					printf("\nUnable to create delta.\n");
					goto out_error;
				}
				
//				if (checksum((const unsigned char *) delta_test, extent_size) 
//				    != checksum((const unsigned char *) extent_data2, extent_size))
//					printf("checksum of delta_test does not match check_sum of extent_data2");
				
				if (memcmp(delta_test, extent_data2, extent_size) != 0) {
					trace_off(printf("\nGenerated delta does not match extent on disk. \n"););
					memcpy(delta_data, extent_data2, extent_size);
					delta_size = extent_size;
				}
				trace_off(printf("\nAble to generate delta\n"););
				free(delta_test);
			}
		}
		
		/* zlib compression */
		int comp_ret = compress2(comp_delta, (unsigned long *) &comp_size, delta_data, delta_size, level);

		if (comp_ret == Z_MEM_ERROR) {
			printf("\nNot enough buffer memory for compression.\n");
			goto out_error;
		}
		if (comp_ret == Z_BUF_ERROR) {
			printf("\nNot enough room in the output buffer for compression.\n");
			goto out_error;
		}
		if (comp_ret == Z_STREAM_ERROR) {
			printf("\nParameter is invalid: level=%d delta_size=%Lu\n", level, delta_size);
			goto out_error;
		}
		
		trace_off(printf("Set up delta extent header\n"););
		deh.check_sum = checksum((const unsigned char *) extent_data2, extent_size);
		deh.extent_addr = extent_addr;
		deh.num_of_chunks = num_of_chunks;
		deh.setting = mode;
		deh.compress = FALSE;
		deh.data_length = delta_size;

		if (mode == OPT_COMP) {
			trace_off(printf("Within opt_comp mode\n"););

			int ext2_comp_ret = compress2(ext2_comp_delta, (unsigned long *) &ext2_comp_size, extent_data2, extent_size, level);

	                if (ext2_comp_ret == Z_MEM_ERROR) {
	                        printf("\nNot enough buffer memory for compression.\n");
				goto out_error;
	                }
	                if (ext2_comp_ret == Z_BUF_ERROR) {
	                        printf("\nNot enough room in the output buffer for compression.\n");
	                        goto out_error;
	                }
	                if (ext2_comp_ret == Z_STREAM_ERROR) {
        	                printf("\nParameter is invalid: level=%d delta_size=%Lu\n", level, delta_size);
                	        goto out_error;
			}
		}
		
		if (comp_size < delta_size) {
			deh.compress = TRUE;
			if (ext2_comp_size < comp_size) {
				deh.data_length = ext2_comp_size;
				deh.setting = RAW;
			} else { 			
				deh.data_length = comp_size;
				deh.setting = XDELTA;
			}
		}
		
		/* write the chunk header and chunk delta data to the delta file*/
		trace_off(printf("Writing chunk "U64FMT" at "U64FMT" to delta\n", chunk_num, extent_addr););
		if (fdwrite(deltafile, &deh, sizeof(deh)) < 0) {
			printf("\nDelta_extent_header was not written properly to deltafile.\n");
			goto out_error;
		}
		
		if (deh.compress == TRUE) {
			if (deh.setting == XDELTA) {
				if (fdwrite(deltafile, comp_delta, comp_size) < 0) {
					printf("\nComp_delta was not written properly to deltafile.\n");
					goto out_error;
				}
			} else {
				if (fdwrite(deltafile, ext2_comp_delta, ext2_comp_size) < 0) {
					printf("\nExt2_comp_delta was not written properly to deltafile.\n");
					goto out_error;
				}
			}
		} else {
			if (fdwrite(deltafile, delta_data, delta_size) < 0) {
				printf("\nDelta_data was not written properly to deltafile.\n");
				goto out_error;
			}
		}
		
                if (mode == TEST) {
			if (fdwrite(deltafile, extent_data1, extent_size) < 0) {
				printf("extent_data1 was not written properly to deltafile.\n");
				goto out_error;
			}
			if (fdwrite(deltafile, extent_data2, extent_size) < 0) {
				printf("extent_data2 was not written properly to deltafile.\n");
				goto out_error;
			}
                }

		chunk_num = chunk_num + num_of_chunks;

		if (progress) {
			printf("\rGenerating chunk "U64FMT"/"U64FMT" ("U64FMT"%%)", chunk_num, cl->count, (chunk_num * 100) / cl->count);
			fflush(stdout);
		}

		/* free memory */
		free(extent_data1);
		free(extent_data2);
		free(delta_data);
		free(comp_delta);
		free(ext2_comp_delta);

	}
	printf("\n");

	/* Make sure everything in changelist was properly transmitted */
	if (chunk_num != cl->count) {
		fprintf(stderr, "Changelist was not fully transmitted.\n");
		goto out_error;
	}

	close(snapdev1);
	close(snapdev2);

	trace_on(printf("All chunks written to delta\n"););

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

	int err;

	trace_on(fprintf(stderr, "writing delta file with chunk_num="U64FMT" chunk_size=%u mode=%u\n", dh.chunk_num, dh.chunk_size, dh.mode););
	if ((err = fdwrite(deltafile, &dh, sizeof(dh))) < 0)
		return err;

	if ((err = generate_delta_extents(mode, level, cl, deltafile, dev1name, dev2name, TRUE)) < 0)
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

	if ((err = readpipe(serv_fd, &head, sizeof(head))) < 0)
		error("%s (%i)", strerror(-err), -err);

	trace_on(printf("reply = %x\n", head.code););

	if (head.code != REPLY_STREAM_CHANGE_LIST)
		error("received unexpected code=%u length=%u", head.code, head.length);

	struct changelist_stream cl_head;

	if (head.length != sizeof(cl_head))
		error("reply length mismatch: expected %u, actual %u", sizeof(cl_head), head.length);

	if ((err = readpipe(serv_fd, &cl_head, sizeof(cl_head))) < 0)
		error("%s (%i)", strerror(-err), -err);

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
	if ((err = readpipe(serv_fd, cl->chunks, cl->count * sizeof(cl->chunks[0]))) < 0)
		error("%s (%i)", strerror(-err), -err);

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

	if (readpipe(ds_fd, &head, sizeof(head)) < 0)
		return eek();

	trace_on(printf("reply = %x\n", head.code););

	if (head.code != SEND_DELTA_PROCEED)
		error("received unexpected code=%u length=%u", head.code, head.length);

	trace_on(fprintf(stderr, "sending delta\n"););

	/* stream delta */

	if (generate_delta_extents(mode, level, cl, ds_fd, snapdev1, snapdev2, TRUE) < 0) {
		fprintf(stderr, "could not send delta for snapshot devices %s and %s downstream\n", snapdev1, snapdev2);
		return 1;
	}

	trace_on(fprintf(stderr, "waiting for response\n"););

	if (readpipe(ds_fd, &head, sizeof(head)) < 0)
		return eek();

	trace_on(printf("reply = %x\n", head.code););

	if (head.code != SEND_DELTA_DONE)
		error("received unexpected code=%u length=%u", head.code, head.length);

	/* success */

	trace_on(fprintf(stderr, "downstream server successfully applied delta to snapshot %d\n", remsnap););

	return 0;
}

static int apply_delta_extents(int deltafile, u32 mode, u32 chunk_size, u64 chunk_count, char const *devname, int progress)
{
	int snapdev;

	snapdev = open(devname, O_RDWR); /* FIXME: why not O_WRONLY? */
	if (snapdev < 0) {
		int err = -errno;
		fprintf(stderr, "could not open snapdev file \"%s\" for writing: %s.\n", devname, strerror(-err));
		return err;
	}

	unsigned char *extent_data, *delta_data, *updated, *comp_delta;
        char *up_extent1, *up_extent2;

        printf("device: %s\n", devname);
        printf("mode: %u\n", mode);
        printf("chunk_count: "U64FMT"\n", chunk_count);

	struct delta_extent_header deh = { };
	u64 uncomp_size, extent_size;
	u64 extent_addr, chunk_num;
        int c1_chk_sum = 0;
	int ret = 0;
	int err;

	for (chunk_num = 0; chunk_num < chunk_count;) {

		trace_off(printf("reading chunk "U64FMT" header\n", chunk_num););
		if ((err = fdread(deltafile, &deh, sizeof(deh))) < 0) {
			fprintf(stderr, "Error reading extent "U64FMT", expected "U64FMT" chunks: %s\n", chunk_num, chunk_count, strerror(-err));
			close(snapdev);
			return err;
		}

		if (deh.magic_num != MAGIC_NUM) {
			fprintf(stderr, "\nNot a proper delta file (magic_num doesn't match).\n");
			close(snapdev);
			return -1;
		}

		extent_size = (deh.num_of_chunks) * chunk_size;
		uncomp_size = extent_size;
		extent_data = (unsigned char *)malloc (extent_size);
		delta_data   = (unsigned char *)malloc (extent_size + 12 + (extent_size >> 9));
		updated      = (unsigned char *)malloc (extent_size);
		comp_delta   = (unsigned char *)malloc (extent_size);
		up_extent1  = (char *)malloc (extent_size);
		up_extent2  = (char *)malloc (extent_size);
		
		extent_addr = deh.extent_addr;

		trace_off(printf("data length is %Lu (buffer is %Lu)\n", deh.data_length, extent_size););

		if (deh.compress == TRUE) {
			if ((err = fdread(deltafile, comp_delta, deh.data_length)) < 0) {
				fprintf(stderr, "\nCould not properly read comp_delta from deltafile: %s.\n", strerror(-err));
				close(snapdev);
				return -1;
			}
		} else {
			if ((err = fdread(deltafile, delta_data, deh.data_length)) < 0) {
				fprintf(stderr, "\nCould not properly read delta_data from deltafile: %s.\n", strerror(-err));
				close(snapdev);
				return -1;
			}
		}

		trace_off(printf("reading extent "U64FMT" data at "U64FMT"\n", chunk_num, extent_addr););
		if (diskread(snapdev, extent_data, extent_size, extent_addr) < 0) {
			fprintf(stderr, "\nSnapdev reading of downstream extent has failed.\n");
			close(snapdev);
			return -1;
		}

		if (deh.compress == TRUE) {
			trace_off(printf("data was compressed \n"););
			/* zlib decompression */
			int comp_ret = uncompress(delta_data, (unsigned long *) &uncomp_size, comp_delta, deh.data_length);
			if (comp_ret == Z_MEM_ERROR) {
				fprintf(stderr, "\nNot enough buffer memory for decompression.\n");
				close(snapdev);
				return -1;
			}
			if (comp_ret == Z_BUF_ERROR) {
				fprintf(stderr, "\nNot enough room in the output buffer for decompression.\n");
				close(snapdev);
				return -1;
			}
			if (comp_ret == Z_DATA_ERROR) {
				fprintf(stderr, "\nThe input data was corrupted for decompression.\n");
				close(snapdev);
				return -1;
			}
		} else
			uncomp_size = deh.data_length;

                if (deh.setting == RAW) 
			memcpy(updated, delta_data, extent_size);
		else {
			if (uncomp_size == extent_size)
				memcpy(updated, delta_data, extent_size);
			else 
				ret = apply_delta_chunk(extent_data, updated, delta_data, extent_size, uncomp_size);
			
			trace_off(printf("uncomp_size %Lu & deh.data_length %Lu\n", uncomp_size, deh.data_length););
			trace_off(printf("ret %d\n", ret););
			
			if (ret < 0) {
				printf("\nDelta for extent with start address of "U64FMT" was not applied properly.\n", extent_addr);
				close(snapdev);
				return -1;
			}
			if (mode == TEST) {
				if (fdread(deltafile, up_extent1, extent_size) < 0) {
					printf("\nUp_extent1 not read properly from deltafile.\n");
					return -1;
				}
				if (fdread(deltafile, up_extent2, extent_size) < 0) {
					printf("\nUp_extent2 not read properly from deltafile.\n");
					return -1;
				}
				c1_chk_sum = checksum((const unsigned char *) extent_data, extent_size);
			}
			if (deh.check_sum != checksum((const unsigned char *) updated, extent_size)) {
				printf("Check_sum failed for chunk address "U64FMT"\n", extent_addr);
				if (mode == TEST) {
					/* sanity check: does the checksum of upstream extent1 = checksum of downstream extent1? */
					if (c1_chk_sum != checksum((const unsigned char *) up_extent1, extent_size)) {
						printf("check_sum of extent1 doesn't match for address "U64FMT"\n", extent_addr);
						if (deh.data_length == extent_size)
							memcpy(updated, delta_data, extent_size);
						else
							ret = apply_delta_chunk(up_extent1, updated, delta_data, extent_size, deh.data_length);
						
						if (ret < 0)
							printf("Delta for extent address "U64FMT" with upstream extent1 was not applied properly.\n", extent_addr);
						
						if (deh.check_sum != checksum((const unsigned char *) updated, extent_size)) {
							printf("Check_sum of apply delta onto upstream extent1 failed for chunk address "U64FMT"\n", extent_addr);
							memcpy(updated, up_extent2, extent_size);
						}
					} else {
						printf("apply delta doesn't work; check_sum of extent1 matches for address "U64FMT"\n", extent_addr);
						if (memcmp(extent_data, up_extent1, extent_size) != 0)
							printf("extent_data for extent1 does not match. \n");
						else
							printf("chunk_data for extent1 does matche up. \n");
						memcpy(updated, up_extent2, extent_size);
					}
				} else {
					close(snapdev);
					return -1;
				}
			}
                }
		
		if (diskwrite(snapdev, updated, extent_size, extent_addr) < 0) {
			printf("\nUpdated was not written properly at "U64FMT" in snapdev.\n", extent_addr);
			return -1;
		}

		chunk_num = chunk_num + deh.num_of_chunks;

		if (progress) {
			printf("\rApplying chunk "U64FMT"/"U64FMT" ("U64FMT"%%)", chunk_num, chunk_count, (chunk_num * 100) / chunk_count);
			fflush(stdout);
		}

		/* free memory */
		free(extent_data);
		free(delta_data);
		free(updated);
		free(comp_delta);
		free(up_extent1);
		free(up_extent2);
	}

	close(snapdev);

	trace_on(printf("\nAll extents applied to %s\n", devname););
	return 0;
}

static int apply_delta(int deltafile, char const *devname)
{
	struct delta_header dh;

	if (fdread(deltafile, &dh, sizeof(dh)) < 0) {
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
       
	if ((err = apply_delta_extents(deltafile, dh.mode, dh.chunk_size, dh.chunk_num, devname, TRUE)) < 0)
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

	if (readpipe(serv_fd, &head, sizeof(head)) < 0)
		return eek();

	trace_on(printf("reply = %x\n", head.code););

	if (head.code != SNAPSHOT_LIST)
		error("received unexpected code=%u length=%u", head.code, head.length);

	if (head.length < sizeof(int32_t))
		error("reply length mismatch: expected >=%u, actual %u", sizeof(int32_t), head.length);

	int count;

	if (readpipe(serv_fd, &count, sizeof(int)) < 0)
		return eek();

	if (head.length != sizeof(int32_t) + count * sizeof(struct snapinfo))
		error("reply length mismatch: expected %u, actual %u", sizeof(int32_t) + count * sizeof(struct snapinfo), head.length);

	struct snapinfo * buffer = (struct snapinfo *)malloc(count * sizeof(struct snapinfo));

	if (readpipe(serv_fd, buffer, count * sizeof(struct snapinfo)) < 0)
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

static int delete_snapshot(int sock, int snap)
{
	if (outbead(sock, DELETE_SNAPSHOT, struct create_snapshot, snap) < 0)
		return eek();

	struct head head;
	unsigned maxbuf = 500;
	char buf[maxbuf];

	if (readpipe(sock, &head, sizeof(head)) < 0)
		return eek();
	assert(head.length < maxbuf); // !!! don't die
	trace_on(printf("reply head.length = %x\n", head.length););
	if (readpipe(sock, buf, head.length) < 0)
		return eek();

	trace_on(printf("reply = %x\n", head.code););

	if (head.code != REPLY_DELETE_SNAPSHOT)
		error("received unexpected code: %.*s", head.length - 4, buf + 4);

	return 0;
}

static int create_snapshot(int sock, int snap)
{
	if (outbead(sock, CREATE_SNAPSHOT, struct create_snapshot, snap) < 0)
		return eek();

	struct head head;
	unsigned maxbuf = 500;
	char buf[maxbuf];

	if (readpipe(sock, &head, sizeof(head)) < 0)
		return eek();
	assert(head.length < maxbuf); // !!! don't die
	if (readpipe(sock, buf, head.length) < 0)
		return eek();

	trace_on(printf("reply = %x\n", head.code););

	if (head.code != REPLY_CREATE_SNAPSHOT)
		error("received unexpected code: %.*s", head.length - 4, buf + 4);

	return 0;
}

static int set_priority(int sock, uint32_t tag_val, int8_t pri_val)
{
	if (outbead(sock, SET_PRIORITY, struct snapinfo, tag_val, pri_val) < 0)
		return eek();

	struct head head;
	unsigned maxbuf = 500;
	char buf[maxbuf];

	if (readpipe(sock, &head, sizeof(head)) < 0)
		return eek();
	assert(head.length < maxbuf); // !!! don't die
	if (readpipe(sock, buf, head.length) < 0)
		return eek();

	trace_on(printf("reply = %x\n", head.code););

	if (head.code != REPLY_SET_PRIORITY)
		error("received unexpected code: %.*s", head.length - 4, buf + 4);

	return 0;
}

static int set_usecount(int sock, uint32_t tag_val, const char * op)
{
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
			fprintf(stderr, "unable to accept connection: %s\n", strerror(-csock));
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

		if ((err = readpipe(csock, &message.head, sizeof(message.head))) < 0) {
			fprintf(stderr, "error reading upstream message header: %s\n", strerror(-err));
			goto cleanup_connection;
		}
		if (message.head.length > maxbody) {
			fprintf(stderr, "message body too long %d\n", message.head.length);
			goto cleanup_connection;
		}
		if ((err = readpipe(csock, &message.body, message.head.length)) < 0) {
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

			if (apply_delta_extents(csock, body.delta_mode, body.chunk_size, body.chunk_count, remotedev, TRUE) < 0) {
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
	       "        init              Initializes snapshot storage device\n"
	       "        agent-server      Starts up the snapshot agent\n"
	       "        snap-server       Starts up the snapshot server\n"
	       "	create-snap       Creates a snapshot\n"
	       "	delete-snap       Deletes a snapshot\n"
	       "	list              Returns a list of snapshots\n"
	       "	set-priority      Sets the priority of a snapshot\n"
	       "	set-usecount      Sets the use count of a snapshot\n"
	       "	create-cl         Creates a changelist given 2 snapshots\n"
	       "	create-delta      Creates a delta file given a changelist and 2 snapshots\n"
	       "	apply-delta       Applies a delta file to the given device\n"
	       "	send-delta        Sends a delta file downstream\n"
	       "	delta-server      Listens for upstream deltas\n");
}

static void cdUsage(poptContext optCon, int exitcode, char const *error, char const *addl)
{
	poptPrintUsage(optCon, stderr, 0);
	if (error) fprintf(stderr, "%s: %s", error, addl);
	exit(exitcode);
}

int main(int argc, char *argv[])
{
	char const *command;

	struct poptOption noOptions[] = {
		POPT_TABLEEND
	};

	char *js_str = NULL, *bs_str = NULL;
	int yes = FALSE;
	struct poptOption initOptions[] = {
		{ "yes", 'y', POPT_ARG_NONE, &yes, 0, "Answer yes to all prompts", NULL},
		{ "journal_size", 'j', POPT_ARG_STRING, &js_str, 0, "User specified journal size, i.e. 400k (default: 100 * chunk_size)", "desired journal size" },
		{ "block_size", 'b', POPT_ARG_STRING, &bs_str, 0, "User specified block size, has to be a power of two, i.e. 8k (default: 4k)", "desired block size" },
	     POPT_TABLEEND
	};

	int nobg = 0;
	struct poptOption serverOptions[] = {
		{ "foreground", 'f', POPT_ARG_NONE, &nobg, 0, "do not daemonize server", NULL },
		POPT_TABLEEND
	};

	int xd = FALSE, raw = FALSE, test = FALSE, gzip_level = DEF_GZIP_COMP, opt_comp = FALSE;
	struct poptOption cdOptions[] = {
		{ "xdelta", 'x', POPT_ARG_NONE, &xd, 0, "Delta file format: xdelta chunk", NULL },
		{ "raw", 'r', POPT_ARG_NONE, &raw, 0, "Delta file format: raw chunk from later snapshot", NULL },
		{ "optcomp", 'o', POPT_ARG_NONE, &opt_comp, 0, "Delta file format: optimal compression (slowest)", NULL},
		{ "test", 't', POPT_ARG_NONE, &test, 0, "Delta file format: xdelta chunk, raw chunk from earlier snapshot and raw chunk from later snapshot", NULL },
		{ "gzip", 'g', POPT_ARG_INT, &gzip_level, 0, "Compression via gzip", "compression_level"},
		POPT_TABLEEND
	};

	poptContext mainCon;
	struct poptOption mainOptions[] = {
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &initOptions, 0,
		  "Initialize\n\t Function: Initializes snapshot storage device\n\t Usage: init [OPTION...] <dev/snapshot> <dev/origin> [dev/meta]" , NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &serverOptions, 0,
		  "Agent Server\n\t Function: Starts up the snapshot agent\n\t Usage: agent-server [OPTION...] <agent_socket>" , NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &serverOptions, 0,
		  "Snapshot Server\n\t Function: Starts up the snapshot server\n\t Usage: snap-server [OPTION...] <dev/snapshot> <dev/origin> [dev/meta] <agent_socket> <server_socket>" , NULL },
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
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &cdOptions, 0,
		  "Send delta\n\t Function: Sends a delta file downstream\n\t Usage: send-delta [OPTION...] <sockname> <snapshot1> <snapshot2> <snapdev1> <snapdev2> <remsnapshot> <host>[:<port>]\n" , NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Delta Server\n\t Function: Listens for upstream deltas\n\t Usage: delta-server <snapdevstem> [<host>[:<port>]]" , NULL },
		POPT_AUTOHELP
		POPT_TABLEEND
	};
	mainCon = poptGetContext(NULL, argc, (const char **)argv, mainOptions, 0);

	if (argc < 2) {
		poptPrintHelp(mainCon, stdout, 0);
		poptFreeContext(mainCon);
		exit(1);
	}

	command = argv[1];

	if (strcmp(command, "--help") == 0 || strcmp(command, "-?") == 0) {
		poptPrintHelp(mainCon, stdout, 0);
		poptFreeContext(mainCon);
		exit(1);
	}

	poptFreeContext(mainCon);

	if (strcmp(command, "--usage") == 0) {
		mainUsage();
		exit(1);
	}

	if (strcmp(command, "init") == 0) {
		
		char const *snapdev, *origdev, *metadev;
		u32 bs_bits = SECTOR_BITS + SECTORS_PER_BLOCK, js_bytes = DEFAULT_JOURNAL_SIZE;				

		struct poptOption options[] = {
			{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &initOptions, 0, NULL, NULL },
			POPT_AUTOHELP
			POPT_TABLEEND
		};

		poptContext initCon = poptGetContext(NULL, argc-1, (const char **)&(argv[1]), options, 0);
		poptSetOtherOptionHelp(initCon, "<dev/snapshot> <dev/origin> [dev/meta]");

		char initOpt = poptGetNextOpt(initCon);

		if (argc < 3) {
			poptPrintUsage(initCon, stderr, 0);
			exit(1);
		}

		if (initOpt < -1) {
			fprintf(stderr, "%s: %s: %s\n", command, poptBadOption(initCon, POPT_BADOPTION_NOALIAS), poptStrerror(initOpt));
			poptFreeContext(initCon);
			return 1;
		}

		snapdev = poptGetArg(initCon);
		if (!snapdev) {
			fprintf(stderr, "%s: snapshot device must be specified\n", command);
			poptPrintUsage(initCon, stderr, 0);
			poptFreeContext(initCon);
			return 1;
		}
		
		origdev = poptGetArg(initCon);
		if (!origdev) {
			fprintf(stderr, "%s: origin device must be specified\n", command);
			poptPrintUsage(initCon, stderr, 0);
			poptFreeContext(initCon);
			return 1;
		}
		
		metadev = poptGetArg(initCon); /* ok if NULL */
		
		if (poptPeekArg(initCon) != NULL) {
			fprintf(stderr, "%s: too many arguments\n", command);
			poptPrintUsage(initCon, stderr, 0);
			poptFreeContext(initCon);
			return 1;
		}

		struct superblock *sb;
		posix_memalign((void **)&sb, 1 << SECTOR_BITS, SB_SIZE);
		memset(sb, 0, SB_SIZE);
		
		init_buffers();
		
		if ((sb->snapdev = open(snapdev, O_RDWR | O_DIRECT)) == -1)
			error("Could not open snapshot store %s: %s", snapdev, strerror(errno));
		
		if ((sb->orgdev = open(origdev, O_RDONLY | O_DIRECT)) == -1)
			error("Could not open origin volume %s: %s", origdev, strerror(errno));
		
		sb->metadev = sb->snapdev;
		if (metadev && (sb->metadev = open(metadev, O_RDWR)) == -1) /* can I do an O_DIRECT on the ramdevice? */
			error("Could not open meta volume %s: %s", metadev, strerror(errno));

		poptFreeContext(initCon);
		
		trace_off(printf("js_bytes was %d & bs_bits was %d\n", js_bytes, bs_bits););

		if (bs_str != NULL) {
			bs_bits = strtobits(bs_str);
			if (bs_bits == INPUT_ERROR) {
				poptPrintUsage(initCon, stderr, 0);
				fprintf(stderr, "Invalid block size input. Try 64k\n");
				exit(1);
			}
		}

		if (js_str != NULL) {
			if (js_bytes == INPUT_ERROR) {
				poptPrintUsage(initCon, stderr, 0);
				fprintf(stderr, "Invalid journal size input. Try 400k\n");
				exit(1);
			}
		}

		trace_off(printf("js_bytes is %d & bs_bits is %d\n", js_bytes, bs_bits););

# ifdef MKDDSNAP_TEST
		init_snapstore(sb, js_bytes, bs_bits);
		create_snapshot(sb, 0);
		
		int i;
		for (i = 0; i < 100; i++) {
			make_unique(sb, i, 0);
		}
		
		flush_buffers();
		evict_buffers();
		warn("delete...");
		delete_tree_range(sb, 1, 0, 5);
		show_buffers();
		warn("dirty buffers = %i", dirty_buffer_count);
		show_tree(sb);
		return 0;
# endif /* end of test code */
		
		return init_snapstore(sb, js_bytes, bs_bits);
	}
	if (strcmp(command, "agent-server") == 0) {
		char const *sockname;
		int listenfd;	

		struct poptOption options[] = {
			{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &serverOptions, 0, NULL, NULL },
			POPT_AUTOHELP
			POPT_TABLEEND
		};

		poptContext agentCon = poptGetContext(NULL, argc-1, (const char **)&(argv[1]), options, 0);
		poptSetOtherOptionHelp(agentCon, "<agent_socket>");

		char agentOpt = poptGetNextOpt(agentCon);

		if (agentOpt < -1) {
			fprintf(stderr, "%s: %s: %s\n", command, poptBadOption(agentCon, POPT_BADOPTION_NOALIAS), poptStrerror(agentOpt));
			poptFreeContext(agentCon);
			return 1;
		}

		sockname = poptGetArg(agentCon);
		if (sockname == NULL) {
			fprintf(stderr, "%s: socket name for ddsnap agent must be specified\n", command);
			poptPrintUsage(agentCon, stderr, 0);
			poptFreeContext(agentCon);
			return 1;
		}
		if (poptPeekArg(agentCon) != NULL) {
			fprintf(stderr, "%s: only one socket name may be specified\n", command);
			poptPrintUsage(agentCon, stderr, 0);
			poptFreeContext(agentCon);
			return 1;
		}

		poptFreeContext(agentCon);

		if (monitor_setup(sockname, &listenfd) < 0)
			error("Could not setup ddsnap agent server\n");

		if (!nobg) {
			pid_t pid;

			pid = daemonize("/tmp/ddsnap.agent.log");
			if (pid == -1)
				error("Could not daemonize\n");
			if (pid != 0) {
				trace_on(printf("pid = %lu\n", (unsigned long)pid););
				return 0;
			}
		}
		
		if (monitor(listenfd, &(struct context){ .polldelay = -1 }) < 0)
			error("Could not start ddsnap agent server\n");
		
		return 0; /* not reached */
		
	}
	if (strcmp(command, "snap-server") == 0) {
		char const *snapdev, *origdev, *metadev;
		char const *agent_sockname, *server_sockname;
		
		struct poptOption options[] = {
			{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &serverOptions, 0, NULL, NULL },
			POPT_AUTOHELP
			POPT_TABLEEND
		};

		poptContext serverCon = poptGetContext(NULL, argc-1, (const char **)&(argv[1]), options, 0);
		poptSetOtherOptionHelp(serverCon, "<dev/snapshot> <dev/origin> [dev/meta] <agent_socket> <server_socket>");

		char serverOpt = poptGetNextOpt(serverCon);

		if (argc < 3) {
			poptPrintUsage(serverCon, stderr, 0);
			exit(1);
		}

		if (serverOpt < -1) {
			fprintf(stderr, "%s: %s: %s\n", command, poptBadOption(serverCon, POPT_BADOPTION_NOALIAS), poptStrerror(serverOpt));
			poptFreeContext(serverCon);
			return 1;
		}

		snapdev = poptGetArg(serverCon);
		if (!snapdev) {
			fprintf(stderr, "%s: snapshot device must be specified\n", command);
			poptPrintUsage(serverCon, stderr, 0);
			poptFreeContext(serverCon);
			return 1;
		}
		
		origdev = poptGetArg(serverCon);
		if (!origdev) {
			fprintf(stderr, "%s: origin device must be specified\n", command);
			poptPrintUsage(serverCon, stderr, 0);
		poptFreeContext(serverCon);
		return 1;
		}	
	
		char const *extra_arg[3];

		extra_arg[0] = poptGetArg(serverCon);
		extra_arg[1] = poptGetArg(serverCon);
		extra_arg[2] = poptGetArg(serverCon);

		if (!extra_arg[0] || !extra_arg[1]) {
			fprintf(stderr, "%s: agent and server socket names must both be specified\n", command);
			poptPrintUsage(serverCon, stderr, 0);
			poptFreeContext(serverCon);
			return 1;
		}
 
		if (extra_arg[2]) {
			metadev = extra_arg[0];
			agent_sockname = extra_arg[1];
			server_sockname = extra_arg[2];
		} else {
			metadev = NULL;
			agent_sockname = extra_arg[0];
			server_sockname = extra_arg[1];
		}
		
		if (poptPeekArg(serverCon) != NULL) {
			fprintf(stderr, "%s: too many arguments\n", command);
			poptPrintUsage(serverCon, stderr, 0);
			poptFreeContext(serverCon);
			return 1;
		}
		
		struct superblock *sb;
		posix_memalign((void **)&sb, 1 << SECTOR_BITS, SB_SIZE);
		memset(sb, 0, SB_SIZE);

		init_buffers();
		
		if ((sb->snapdev = open(snapdev, O_RDWR | O_DIRECT)) == -1)
			error("Could not open snapshot store %s: %s", snapdev, strerror(errno));
		
		if ((sb->orgdev = open(origdev, O_RDONLY | O_DIRECT)) == -1)
			error("Could not open origin volume %s: %s", origdev, strerror(errno));
		
		sb->metadev = sb->snapdev;
		if (metadev && (sb->metadev = open(metadev, O_RDWR)) == -1) /* can I do an O_DIRECT on the ramdevice? */
			error("Could not open meta volume %s: %s", metadev, strerror(errno));
		
		poptFreeContext(serverCon);
		
		
		int listenfd, getsigfd, agentfd;
		
		if (snap_server_setup(agent_sockname, server_sockname, &listenfd, &getsigfd, &agentfd) < 0)
			error("Could not setup snapshot server\n");
		if (!nobg) {
			pid_t pid;
			
			pid = daemonize("/tmp/ddsnapd.log");
			if (pid == -1)
				error("Could not daemonize\n");
			if (pid != 0) {
				trace_on(printf("pid = %lu\n", (unsigned long)pid););
				return 0;
			}
		}
		if (snap_server(sb, server_sockname, listenfd, getsigfd, agentfd) < 0)
			error("Could not start snapshot server\n");
	
		return 0; /* not reached */
	}
	if (strcmp(command, "create-snap") == 0) {
		if (argc != 4) {
			printf("Usage: %s create-snap <sockname> <snapshot>\n", argv[0]);
			return 1;
		}

		int snapid;

		if (parse_snapid(argv[3], &snapid) < 0) {
			fprintf(stderr, "%s: invalid snapshot id %s\n", argv[0], argv[3]);
			return 1;
		}

		int sock = create_socket(argv[2]);

		int ret = create_snapshot(sock, snapid);
		close(sock);
		return ret;
	}
	if (strcmp(command, "delete-snap") == 0) {
		if (argc != 4) {
			printf("Usage: %s delete-snap <sockname> <snapshot>\n", argv[0]);
			return 1;
		}

		int snapid;

		if (parse_snapid(argv[3], &snapid) < 0) {
			fprintf(stderr, "%s: invalid snapshot id %s\n", argv[0], argv[3]);
			return 1;
		}

		int sock = create_socket(argv[2]);

		int ret = delete_snapshot(sock, snapid);
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

		int snapid;

		if (parse_snapid(argv[3], &snapid) < 0) {
			fprintf(stderr, "%s: invalid snapshot id %s\n", argv[0], argv[3]);
			return 1;
		}

		int sock = create_socket(argv[2]);

		int ret = set_priority(sock, snapid, atoi(argv[4]));
		close(sock);
		return ret;
	}
	if (strcmp(command, "set-usecount") == 0) {
		if (argc != 5) {
			printf("usage: %s set-usecount <sockname> <snap_tag> <inc|dec>\n", argv[0]);
			return 1;
		}

		int snapid;

		if (parse_snapid(argv[3], &snapid) < 0) {
			fprintf(stderr, "%s: invalid snapshot id %s\n", argv[0], argv[3]);
			return 1;
		}

		int sock = create_socket(argv[2]);

		int ret = set_usecount(sock, snapid, argv[4]);
		close(sock);
		return ret;
	}
	if (strcmp(command, "create-cl") == 0) {
		if (argc != 6) {
			printf("usage: %s create-cl <sockname> <changelist> <snapshot1> <snapshot2>\n", argv[0]);
			return 1;
		}

		int snapid1, snapid2;

		if (parse_snapid(argv[4], &snapid1) < 0) {
			fprintf(stderr, "%s: invalid snapshot id %s\n", argv[0], argv[4]);
			return 1;
		}

		if (parse_snapid(argv[5], &snapid2) < 0) {
			fprintf(stderr, "%s: invalid snapshot id %s\n", argv[0], argv[5]);
			return 1;
		}

		int sock = create_socket(argv[2]);

		int ret = ddsnap_generate_changelist(sock, argv[3], snapid1, snapid2);
		close(sock);
		return ret;
	}
	if (strcmp(command, "create-delta") == 0) {
		char cdOpt;
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
			fprintf(stderr, "%s: %s: %s\n",
				command,
				poptBadOption(cdCon, POPT_BADOPTION_NOALIAS),
				poptStrerror(cdOpt));
			poptFreeContext(cdCon);
			return 1;
		}

		/* Make sure the options are mutually exclusive */
		if (xd+raw+test > 1)
			cdUsage(cdCon, 1, "Too many chunk options were selected. \nPlease select only one", "-x, -r, -t\n");

		u32 mode = (test ? TEST : (raw ? RAW : (xd? XDELTA : OPT_COMP)));
		if (opt_comp)
			gzip_level = MAX_GZIP_COMP;
		
		trace_on(fprintf(stderr, "xd=%d raw=%d test=%d opt_comp=%d mode=%u gzip_level=%d\n", xd, raw, test, opt_comp, mode, gzip_level););

		char const *changelist, *deltafile, *snapdev1, *snapdev2;

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
		poptContext cdCon;

		struct poptOption options[] = {
			{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &cdOptions, 0, NULL, NULL },
			POPT_AUTOHELP
			POPT_TABLEEND
		};

		cdCon = poptGetContext(NULL, argc-1, (const char **)&(argv[1]), options, 0);
		poptSetOtherOptionHelp(cdCon, "<sockname> <snapshot1> <snapshot2> <snapdev1> <snapdev2> <remsnapshot> <host>[:<port>]");

		char c;

		while ((c = poptGetNextOpt(cdCon)) >= 0);
		if (c < -1) {
			fprintf(stderr, "%s: %s: %s\n", argv[0], poptBadOption(cdCon, POPT_BADOPTION_NOALIAS), poptStrerror(c));
			poptFreeContext(cdCon);
			return 1;
		}

		/* Make sure the options are mutually exclusive */
		if (xd+raw+test+opt_comp > 1) {
			fprintf(stderr, "%s: Too many chunk options were selected.\nPlease select only one: -x, -r, -t, -o\n", argv[0]);
			poptPrintUsage(cdCon, stderr, 0);
			poptFreeContext(cdCon);
			return 1;
		}

		u32 mode = (test ? TEST : (raw ? RAW : (xd ? XDELTA : OPT_COMP)));
		if (opt_comp)
			gzip_level = MAX_GZIP_COMP;

		trace_on(fprintf(stderr, "xd=%d raw=%d test=%d opt_comp=%d mode=%u gzip_level=%d\n", xd, raw, test, opt_comp, mode, gzip_level););

		char const *sockname, *snapid1str, *snapid2str, *snapdev1, *snapdev2, *snapidremstr, *hoststr;

		sockname  = poptGetArg(cdCon);
		snapid1str = poptGetArg(cdCon);
		snapid2str = poptGetArg(cdCon);
		snapdev1 = poptGetArg(cdCon);
		snapdev2 = poptGetArg(cdCon);
		snapidremstr = poptGetArg(cdCon);
		hoststr = poptGetArg(cdCon);

		if (hoststr == NULL)
			cdUsage(cdCon, 1, argv[0], "Not enough arguments to send-delta\n");
		if (!(poptPeekArg(cdCon) == NULL))
			cdUsage(cdCon, 1, argv[0], "Too many arguments to send-delta\n");

		poptFreeContext(cdCon);

		char *hostname;
		unsigned port;

		hostname = strdup(hoststr);

		if (strchr(hostname, ':')) {
			unsigned int len = strlen(hostname);
			port = parse_port(hostname, &len);
			hostname[len] = '\0';
		} else {
			port = DEFAULT_REPLICATION_PORT;
		}

		int snapid1, snapid2, remsnapid;

		if (parse_snapid(snapid1str, &snapid1) < 0) {
			fprintf(stderr, "%s: invalid snapshot id %s\n", argv[0], snapid1str);
			return 1;
		}

		if (parse_snapid(snapid2str, &snapid2) < 0) {
			fprintf(stderr, "%s: invalid snapshot id %s\n", argv[0], snapid2str);
			return 1;
		}

		if (parse_snapid(snapidremstr, &remsnapid) < 0) {
			fprintf(stderr, "%s: invalid snapshot id %s\n", argv[0], snapidremstr);
			return 1;
		}

		int sock = create_socket(sockname);

		int ds_fd = open_socket(hostname, port);
		if (ds_fd < 0) {
			fprintf(stderr, "%s: unable to connect to downstream server %s port %u\n", argv[0], hostname, port);
			return 1;
		}

		int ret = ddsnap_send_delta(sock, snapid1, snapid2, snapdev1, snapdev2, remsnapid, mode, gzip_level, ds_fd);
		close(ds_fd);
		close(sock);

		return ret;
	}
	if (strcmp(command, "delta-server") == 0) {
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
			fprintf(stderr, "%s: unable to bind to %s port %u\n", argv[0], hostname, port);
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

	fprintf(stderr, "%s: unrecognized subcommand: %s.\n", argv[0], command);
	
	return 1;
}

