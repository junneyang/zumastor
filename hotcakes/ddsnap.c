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
#include "build.h"

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

struct cl_header
{
	char magic[MAGIC_SIZE];
	u32 chunksize_bits;
	u32 src_snap;
	u32 tgt_snap;
};

struct delta_header
{
	char magic[MAGIC_SIZE];
	u64 chunk_num;
	u32 chunk_size;
	u32 src_snap;
	u32 tgt_snap;
	u32 mode;
};

struct delta_extent_header
{
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

static int parse_snaptag(char const *snapstr, u32 *snaptag)
{
	if (snapstr[0] == '\0')
		return -1;

	unsigned long num;
	char *endptr;

	num = strtoul(snapstr, &endptr, 10);

	if (*endptr != '\0')
		return -1;

	if (num >= (u32)~0UL)
		return -1;

	*snaptag = num;
	return 0;
}

static struct change_list *read_changelist(int cl_fd)
{
	struct cl_header clh;

	if (fdread(cl_fd, &clh, sizeof(clh)) < 0) {
		warn("Not a proper changelist file (too short for header)\n");
		return NULL;
	}

	if (strncmp(clh.magic, CHANGELIST_MAGIC_ID, MAGIC_SIZE) != 0) {
		warn("Not a proper changelist file (wrong magic in header: %s)\n", clh.magic);
		return NULL;
	}

	struct change_list *cl;

	if ((cl = init_change_list(clh.chunksize_bits, clh.src_snap, clh.tgt_snap)) == NULL)
		return NULL;

	u64 chunkaddr = 0;
	ssize_t len;

	trace_on(printf("reading changelist for snapshots %u and %u from file\n", clh.src_snap, clh.tgt_snap););
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
	struct cl_header clh;
	int err;

	strncpy(clh.magic, CHANGELIST_MAGIC_ID, sizeof(clh.magic));
	clh.chunksize_bits = cl->chunksize_bits;
	clh.src_snap = cl->src_snap;
	clh.tgt_snap = cl->tgt_snap;

	if ((err = fdwrite(change_fd, &clh, sizeof(clh))) < 0) {
		warn("unable to write magic information to changelist file: %s", strerror(-err));
		return -1;
	}

	if ((err = fdwrite(change_fd, cl->chunks, cl->count * sizeof(cl->chunks[0]))) < 0) {
		warn("unable to write changelist file: %s", strerror(-err));
		return -1;
	}

	u64 marker = -1;

	if ((err = fdwrite(change_fd, &marker, sizeof(marker))) < 0)
		error("unable to write changelist marker: %s", strerror(-err));

	return 0;
}

static u64 chunks_in_extent(struct change_list *cl, u64 pos, u32 chunk_size)
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
	int err;
	
	snapdev1 = open(dev1name, O_RDONLY);
	if (snapdev1 < 0) {
		err = -errno;
		goto gen_open1_error;
	}

	snapdev2 = open(dev2name, O_RDONLY);
	if (snapdev2 < 0) {
		err = -errno;
		goto gen_open2_error;
	}

	trace_on(printf("opened snapshot devices snap1=%d snap2=%d to create delta\n", snapdev1, snapdev2););

	/* Variable set up */

	u32 chunk_size = 1 << cl->chunksize_bits;

	printf("dev1name: %s\n", dev1name);
	printf("dev2name: %s\n", dev2name);
	printf("mode: %u\n", mode);
	printf("level: %d\n", level);
	printf("chunksize bits: %u\t", cl->chunksize_bits);
	printf("chunksize: %u\n", chunk_size);
	printf("chunk_count: "U64FMT"\n", cl->count);

	u64 comp_size, extent_size, ext2_comp_size;
	unsigned char *extent_data1, *extent_data2, *delta_data, *comp_delta, *ext2_comp_delta;

	struct delta_extent_header deh = { .magic_num = MAGIC_NUM };
	u64 extent_addr, chunk_num, num_of_chunks = 0;
	u64 delta_size;
	
	trace_on(printf("starting delta generation\n"););

	/* Chunk address followed by CHUNK_SIZE bytes of chunk data */
	for (chunk_num = 0; chunk_num < cl->count;) {
		extent_addr = cl->chunks[chunk_num] << cl->chunksize_bits;

		if (chunk_num == (cl->count - 1) )
			num_of_chunks = 1;
		else
			num_of_chunks = chunks_in_extent(cl, chunk_num, chunk_size);

		extent_size = chunk_size * num_of_chunks;
		delta_size = extent_size;
		comp_size = extent_size + 12 + (extent_size >> 9);
		ext2_comp_size = comp_size;

		trace_off(if (progress) printf("\n"););
		trace_off(printf("extent starting at chunk %llu (chunk address 0x%016llx, byte offset 0x%016llx), %llu chunks (%llu bytes)\n", chunk_num, cl->chunks[chunk_num], extent_addr, num_of_chunks, extent_size););

		extent_data1    = malloc(extent_size);
		extent_data2    = malloc(extent_size);
		delta_data      = malloc(extent_size);
		comp_delta      = malloc(comp_size);
		ext2_comp_delta = malloc(ext2_comp_size);

		if (!extent_data1 || !extent_data2 || !delta_data || !comp_delta || !ext2_comp_delta)
			goto gen_alloc_error;
		
		trace_off(printf("reading data to calculate delta\n"););

		/* read in and generate the necessary chunk information */
		if ((err = diskread(snapdev1, extent_data1, extent_size, extent_addr)) < 0)
			goto gen_readsnap1_error;
		if ((err = diskread(snapdev2, extent_data2, extent_size, extent_addr)) < 0)
			goto gen_readsnap2_error;

		/* 3 different modes, raw (raw snapshot2 chunk), xdelta (xdelta), test (xdelta, raw snapshot1 chunk & raw snapshot2 chunk) */
		if (mode == RAW)
			memcpy(delta_data, extent_data2, extent_size);
		else {
			int ret = create_delta_chunk(extent_data1, extent_data2, delta_data, extent_size, (int *)&delta_size);

			/* If delta is larger than chunk_size, we want to just copy over the raw chunk */
			if (ret == BUFFER_SIZE_ERROR) {
				trace_off(printf("buffer size error\n"););
				memcpy(delta_data, extent_data2, extent_size);
				delta_size = extent_size;
			} else if (ret < 0)
				goto gen_create_error;
			if (ret >= 0) {
				/* sanity test for delta creation */
				unsigned char *delta_test = malloc(extent_size);
				ret = apply_delta_chunk(extent_data1, delta_test, delta_data, extent_size, delta_size);

				if (ret != extent_size) {
					free(delta_test);
					goto gen_applytest_error;
				}
				
//				if (checksum((const unsigned char *) delta_test, extent_size)
//				    != checksum((const unsigned char *) extent_data2, extent_size))
//					printf("checksum of delta_test does not match check_sum of extent_data2");
				
				if (memcmp(delta_test, extent_data2, extent_size) != 0) {
					trace_off(printf("generated delta does not match extent on disk.\n"););
					memcpy(delta_data, extent_data2, extent_size);
					delta_size = extent_size;
				}
				trace_off(printf("able to generate delta\n"););
				free(delta_test);
			}
		}

		/* zlib compression */
		int comp_ret = compress2(comp_delta, (unsigned long *) &comp_size, delta_data, delta_size, level);

		if (comp_ret == Z_MEM_ERROR)
			goto gen_compmem_error;
		if (comp_ret == Z_BUF_ERROR)
			goto gen_compbuf_error;
		if (comp_ret == Z_STREAM_ERROR)
			goto gen_compstream_error;

		trace_off(printf("set up delta extent header\n"););
		deh.check_sum = checksum((const unsigned char *) extent_data2, extent_size);
		deh.extent_addr = extent_addr;
		deh.num_of_chunks = num_of_chunks;
		deh.setting = mode;
		deh.compress = FALSE;
		deh.data_length = delta_size;

		if (mode == OPT_COMP) {
			trace_off(printf("within opt_comp mode\n"););

			int ext2_comp_ret = compress2(ext2_comp_delta, (unsigned long *) &ext2_comp_size, extent_data2, extent_size, level);

			if (ext2_comp_ret == Z_MEM_ERROR)
				goto gen_compmem_error;
			if (ext2_comp_ret == Z_BUF_ERROR)
				goto gen_compbuf_error;
			if (ext2_comp_ret == Z_STREAM_ERROR)
				goto gen_compstream_error;
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
		trace_off(printf("writing delta for extent starting at chunk "U64FMT", address "U64FMT"\n", chunk_num, extent_addr););
		if ((err = fdwrite(deltafile, &deh, sizeof(deh))) < 0)
			goto gen_writehead_error;

		if (deh.compress == TRUE) {
			if (deh.setting == XDELTA) {
				if ((err = fdwrite(deltafile, comp_delta, comp_size)) < 0)
					goto gen_writedata_error;
			} else {
				if ((err = fdwrite(deltafile, ext2_comp_delta, ext2_comp_size)) < 0)
					goto gen_writedata_error;
			}
		} else {
			if ((err = fdwrite(deltafile, delta_data, delta_size)) < 0)
				goto gen_writedata_error;
		}

                if (mode == TEST) {
			if ((err = fdwrite(deltafile, extent_data1, extent_size)) < 0)
				goto gen_writetest_error;
			if ((err = fdwrite(deltafile, extent_data2, extent_size)) < 0)
				goto gen_writetest_error;
                }

		free(ext2_comp_delta);
		free(comp_delta);
		free(delta_data);
		free(extent_data2);
		free(extent_data1);

		chunk_num = chunk_num + num_of_chunks;

		if (progress) {
#ifdef DEBUG_GEN
			printf("Generating chunk "U64FMT"/"U64FMT" ("U64FMT"%%)\n", chunk_num, cl->count, (chunk_num * 100) / cl->count);
#else
			printf("\rGenerating chunk "U64FMT"/"U64FMT" ("U64FMT"%%)", chunk_num, cl->count, (chunk_num * 100) / cl->count);
			fflush(stdout);
#endif
		}
	}

	close(snapdev2);
	close(snapdev1);

	if (progress)
		printf("\n");

	/* Make sure everything in changelist was properly transmitted */
	if (chunk_num != cl->count) {
		warn("changelist was not fully transmitted");
		return -ERANGE;
	}

	trace_on(printf("All chunks written to delta\n"););

	return 0;


	/* error messages */

gen_open1_error:
	warn("could not open snapshot device \"%s\" for reading: %s", dev1name, strerror(-err));
	goto gen_cleanup;

gen_open2_error:
	warn("could not open snapshot device \"%s\" for reading: %s", dev2name, strerror(-err));
	goto gen_close1_cleanup;

gen_alloc_error:
	err = -ENOMEM;
	if (progress)
		printf("\n");
	warn("memory allocation failed while generating a "U64FMT" chunk extent starting at offset "U64FMT": %s", num_of_chunks, extent_addr, strerror(-err));
	goto gen_closeall_cleanup;

gen_readsnap1_error:
	if (progress)
		printf("\n");
	warn("read of "U64FMT" chunk extent starting at offset "U64FMT" in snapshot device \"%s\" failed: %s", num_of_chunks, extent_addr, dev1name, strerror(-err));
	goto gen_free_cleanup;

gen_readsnap2_error:
	if (progress)
		printf("\n");
	warn("read of "U64FMT" chunk extent starting at offset "U64FMT" in snapshot device \"%s\" failed: %s", num_of_chunks, extent_addr, dev2name, strerror(-err));
	goto gen_free_cleanup;

gen_create_error:
	err = -ERANGE;
	if (progress)
		printf("\n");
	warn("unable to create delta for "U64FMT" chunk extent starting at offset "U64FMT, num_of_chunks, extent_addr);
	goto gen_free_cleanup;

gen_applytest_error:
	err = -ERANGE;
	if (progress)
		printf("\n");
	warn("test application of delta for "U64FMT" chunk extent starting at offset "U64FMT" failed", num_of_chunks, extent_addr);
	goto gen_free_cleanup;

gen_compmem_error:
	err = -ENOMEM;
	if (progress)
		printf("\n");
	warn("not enough buffer memory for compression of delta for "U64FMT" chunk extent starting at offset "U64FMT, num_of_chunks, extent_addr);
	goto gen_free_cleanup;

gen_compbuf_error:
	err = -ERANGE;
	if (progress)
		printf("\n");
	warn("not enough room in the output buffer for compression of delta for "U64FMT" chunk extent starting at offset "U64FMT, num_of_chunks, extent_addr);
	goto gen_free_cleanup;

gen_compstream_error:
	err = -ERANGE;
	if (progress)
		printf("\n");
	warn("invalid compression parameter level=%d delta_size=%llu in delta for "U64FMT" chunk extent starting at offset "U64FMT, level, delta_size, num_of_chunks, extent_addr);
	goto gen_free_cleanup;

gen_writehead_error:
	if (progress)
		printf("\n");
	warn("unable to write delta header for "U64FMT" chunk extent starting at offset "U64FMT": %s", num_of_chunks, extent_addr, strerror(-err));
	goto gen_free_cleanup;

gen_writedata_error:
	if (progress)
		printf("\n");
	warn("unable to write delta data for "U64FMT" chunk extent starting at offset "U64FMT": %s", num_of_chunks, extent_addr, strerror(-err));
	goto gen_free_cleanup;

gen_writetest_error:
	if (progress)
		printf("\n");
	warn("unable to write delta test data for "U64FMT" chunk extent starting at offset "U64FMT": %s", num_of_chunks, extent_addr, strerror(-err));
	goto gen_free_cleanup;


	/* error cleanup */

gen_free_cleanup:
	if (ext2_comp_delta)
		free(ext2_comp_delta);
	if (comp_delta)
		free(comp_delta);
	if (delta_data)
		free(delta_data);
	if (extent_data2)
		free(extent_data2);
	if (extent_data1)
		free(extent_data1);

gen_closeall_cleanup:
	close(snapdev2);

gen_close1_cleanup:
	close(snapdev1);

gen_cleanup:
	return err;
}

static int generate_delta(u32 mode, int level, struct change_list *cl, int deltafile, char const *devstem)
{
	/* Delta header set-up */
	struct delta_header dh;

	strncpy(dh.magic, DELTA_MAGIC_ID, sizeof(dh.magic));
	dh.chunk_num = cl->count;
	dh.chunk_size = 1 << cl->chunksize_bits;
	dh.src_snap = cl->src_snap;
	dh.tgt_snap = cl->tgt_snap;
	dh.mode = mode;

	char *dev1name;
	char *dev2name;

	int err;

	if (!(dev1name = malloc(strlen(devstem) + 32))) {
		warn("unable to allocate memory for dev1name\n");
		err = -ENOMEM;
		goto delta_cleanup;
	}
	sprintf(dev1name, "%s%u", devstem, dh.src_snap);

	if (!(dev2name = malloc(strlen(devstem) + 32))) {
		warn("unable to allocate memory for dev2name\n");
		err = -ENOMEM;
		goto delta_free1_cleanup;
	}
	sprintf(dev2name, "%s%u", devstem, dh.tgt_snap);

	trace_on(fprintf(stderr, "writing delta file with chunk_num="U64FMT" chunk_size=%u mode=%u\n", dh.chunk_num, dh.chunk_size, dh.mode););
	if ((err = fdwrite(deltafile, &dh, sizeof(dh))) < 0)
		goto delta_freeall_cleanup;

	if ((err = generate_delta_extents(mode, level, cl, deltafile, dev1name, dev2name, TRUE)) < 0)
		goto delta_freeall_cleanup;

delta_freeall_cleanup:
	free(dev2name);

delta_free1_cleanup:
	free(dev1name);

delta_cleanup:
	return err;
}

static int ddsnap_generate_delta(u32 mode, int level, char const *changelistname, char const *deltaname, char const *devstem)
{
	int clfile = open(changelistname, O_RDONLY);
	if (clfile < 0) {
		warn("could not open changelist file \"%s\" for reading: %s", changelistname, strerror(errno));
		return 1;
	}

	struct change_list *cl;

	if ((cl = read_changelist(clfile)) == NULL) {
		warn("unable to parse changelist file \"%s\"", changelistname);
		close(clfile);
		return 1;
	}

	close(clfile);

	int deltafile = open(deltaname, O_CREAT|O_WRONLY|O_TRUNC, S_IRWXU);
	if (deltafile < 0) {
		warn("could not create delta file \"%s\": %s", deltaname, strerror(errno));
		free_change_list(cl);
		return 1;
	}

	if (generate_delta(mode, level, cl, deltafile, devstem) < 0) {
		warn("could not write delta file \"%s\"", deltaname);
		close(deltafile);
		free_change_list(cl);
		return 1;
	}

	close(deltafile);
	free_change_list(cl);

	return 0;
}

static struct change_list *stream_changelist(int serv_fd, u32 src_snap, u32 tgt_snap)
{
	int err;

	if ((err = outbead(serv_fd, STREAM_CHANGE_LIST, struct stream_changelist, src_snap, tgt_snap))) {
		error("%s (%i)", strerror(-err), -err);
		return NULL;
	}

	struct head head;

	if ((err = readpipe(serv_fd, &head, sizeof(head))) < 0)
		error("%s (%i)", strerror(-err), -err);

	trace_on(printf("reply = %x\n", head.code););

	if (head.code != REPLY_STREAM_CHANGE_LIST) {
		if (head.code == REPLY_ERROR) {
			warn("unable to obtain changelist between snapshot %u and %u", src_snap, tgt_snap);
			return NULL;
		}
		error("received unexpected code=%x length=%u", head.code, head.length);
	}

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
	cl->src_snap = src_snap;
	cl->tgt_snap = tgt_snap;

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

static int ddsnap_send_delta(int serv_fd, u32 src_snap, u32 tgt_snap, char const *snapdev1, char const *snapdev2, u32 remsnap, u32 mode, int level, int ds_fd)
{
	struct change_list *cl;

	trace_on(printf("requesting changelist from snapshot %u to %u\n", src_snap, tgt_snap););

	if ((cl = stream_changelist(serv_fd, src_snap, tgt_snap)) == NULL) {
		warn("could not receive change list for snapshots %u and %u", src_snap, tgt_snap);
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

	if (head.code != SEND_DELTA_PROCEED) {
		if (head.code == REPLY_ERROR) {
			warn("downstream system refused delta");
			return 1;
		}
		error("received unexpected code=%x length=%u", head.code, head.length);
	}

	trace_on(fprintf(stderr, "sending delta\n"););

	/* stream delta */

	if (generate_delta_extents(mode, level, cl, ds_fd, snapdev1, snapdev2, TRUE) < 0) {
		warn("could not send delta downstream for snapshot devices %s and %s", snapdev1, snapdev2);
		return 1;
	}

	trace_on(fprintf(stderr, "waiting for response\n"););

	if (readpipe(ds_fd, &head, sizeof(head)) < 0)
		return eek();

	trace_on(printf("reply = %x\n", head.code););

	if (head.code != SEND_DELTA_DONE) {
		if (head.code == REPLY_ERROR) {
			warn("downstream system unable to apply delta");
			return 1;
		}
		error("received unexpected code=%x length=%u", head.code, head.length);
	}

	/* success */

	trace_on(fprintf(stderr, "downstream server successfully applied delta to snapshot %u\n", remsnap););

	return 0;
}

static int apply_delta_extents(int deltafile, u32 mode, u32 chunk_size, u64 chunk_count, char const *dev1name, char const *dev2name, int progress)
{
	int snapdev1, snapdev2;
	int err;

	snapdev1 = open(dev1name, O_RDONLY);
	if (snapdev1 < 0) {
		err = -errno;
		goto apply_open1_error;
	}

	snapdev2 = open(dev2name, O_WRONLY);
	if (snapdev2 < 0) {
		err = -errno;
		goto apply_open2_error;
	}

        printf("src device: %s\n", dev1name);
        printf("dst device: %s\n", dev2name);
        printf("mode: %u\n", mode);
        printf("chunk_count: "U64FMT"\n", chunk_count);

	unsigned char *extent_data, *delta_data, *updated, *comp_delta;
        char *up_extent1, *up_extent2;

	struct delta_extent_header deh;
	u64 uncomp_size, extent_size;
	u64 extent_addr, chunk_num;

	for (chunk_num = 0; chunk_num < chunk_count;) {
		trace_off(printf("reading chunk "U64FMT" header\n", chunk_num););

		if ((err = fdread(deltafile, &deh, sizeof(deh))) < 0)
			goto apply_headerread_error;

		if (deh.magic_num != MAGIC_NUM)
			goto apply_magic_error;

		extent_size = deh.num_of_chunks * chunk_size;
		uncomp_size = extent_size;

		updated     = malloc(extent_size);
		extent_data = malloc(extent_size);
		delta_data  = malloc(extent_size + 12 + (extent_size >> 9));
		comp_delta  = malloc(extent_size);
		up_extent1  = malloc(extent_size);
		up_extent2  = malloc(extent_size);

		if (!updated || !extent_data || !delta_data || !comp_delta || !up_extent1 || !up_extent2)
			goto apply_alloc_error;

		extent_addr = deh.extent_addr;

		trace_off(printf("extent data length is %llu (extent buffer is %llu)\n", deh.data_length, extent_size););

		if (deh.compress == TRUE) {
			if ((err = fdread(deltafile, comp_delta, deh.data_length)) < 0)
				goto apply_deltaread_error;
		} else {
			if ((err = fdread(deltafile, delta_data, deh.data_length)) < 0)
				goto apply_deltaread_error;
		}

		if (deh.compress == TRUE) {
			trace_off(printf("data was compressed \n"););
			/* zlib decompression */
			int comp_ret = uncompress(delta_data, (unsigned long *) &uncomp_size, comp_delta, deh.data_length);
			if (comp_ret == Z_MEM_ERROR)
				goto apply_compmem_error;
			if (comp_ret == Z_BUF_ERROR)
				goto apply_compbuf_error;
			if (comp_ret == Z_DATA_ERROR)
				goto apply_compdata_error;
		} else
			uncomp_size = deh.data_length;

                if (deh.setting == RAW)
			memcpy(updated, delta_data, extent_size);
		else {
			trace_off(printf("uncomp_size %llu & deh.data_length %llu\n", uncomp_size, deh.data_length););

			if (uncomp_size == extent_size)
				memcpy(updated, delta_data, extent_size);
			else {
				if ((err = diskread(snapdev1, extent_data, extent_size, extent_addr)) < 0)
					goto apply_devread_error;

				trace_off(printf("read %llx chunk delta extent data starting at chunk "U64FMT"/offset "U64FMT" from \"%s\"\n", deh.num_of_chunks, chunk_num, extent_addr, dev1name););

				int apply_ret = apply_delta_chunk(extent_data, updated, delta_data, extent_size, uncomp_size);
				trace_off(printf("apply_ret %d\n", apply_ret););
				if (apply_ret < 0)
					goto apply_chunk_error;
			}

			if (deh.check_sum != checksum((const unsigned char *)updated, extent_size)) {
				if (mode != TEST)
					goto apply_checksum_error;

				if (progress)
					printf("\n");
				printf("Check_sum failed for chunk address "U64FMT"\n", extent_addr);

				if ((err = fdread(deltafile, up_extent1, extent_size)) < 0)
					goto apply_testread_error;
				if ((err = fdread(deltafile, up_extent2, extent_size)) < 0)
					goto apply_testread_error;
				int c1_chk_sum = checksum((const unsigned char *)extent_data, extent_size);

				/* sanity check: does the checksum of upstream extent1 = checksum of downstream extent1? */
				if (c1_chk_sum != checksum((const unsigned char *)up_extent1, extent_size)) {
					printf("check_sum of extent1 doesn't match for address "U64FMT"\n", extent_addr);
					if (deh.data_length == extent_size)
						memcpy(updated, delta_data, extent_size);
					else {
						int apply_ret = apply_delta_chunk(up_extent1, updated, delta_data, extent_size, deh.data_length);
						if (apply_ret < 0)
							printf("Delta for extent address "U64FMT" with upstream extent1 was not applied properly.\n", extent_addr);
					}
					
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
			}
                }
		
		free(up_extent1);
		free(up_extent2);
		free(comp_delta);
		free(extent_data);
		free(delta_data);

		if ((err = diskwrite(snapdev2, updated, extent_size, extent_addr)) < 0)
			goto apply_write_error;

		free(updated);

		chunk_num = chunk_num + deh.num_of_chunks;

		if (progress) {
			printf("\rApplied chunk "U64FMT"/"U64FMT" ("U64FMT"%%)", chunk_num, chunk_count, (chunk_num * 100) / chunk_count);
			fflush(stdout);
		}
	}

	if (progress)
		printf("\n");

	close(snapdev2);
	close(snapdev1);

	trace_on(printf("All extents applied to %s\n", dev2name););
	return 0;


	/* error messages */

apply_open1_error:
	warn("could not open snapdev file \"%s\" for reading: %s.", dev1name, strerror(-err));
	goto apply_cleanup;

apply_open2_error:
	warn("could not open snapdev file \"%s\" for writing: %s.", dev2name, strerror(-err));
	goto apply_closesrc_cleanup;

apply_headerread_error:
	if (progress)
		printf("\n");
	warn("could not read header for extent starting at chunk "U64FMT" of "U64FMT" total chunks: %s", chunk_num, chunk_count, strerror(-err));
	goto apply_closeall_cleanup;

apply_magic_error:
	err = -ERANGE;
	if (progress)
		printf("\n");
	warn("wrong magic in header for extent starting at chunk "U64FMT" of "U64FMT" total chunks", chunk_num, chunk_count);
	goto apply_closeall_cleanup;

apply_alloc_error:
	err = -ENOMEM;
	if (progress)
		printf("\n");
	warn("memory allocation failed while applying "U64FMT" chunk extent starting at offset "U64FMT": %s", deh.num_of_chunks, extent_addr, strerror(-err));
	goto apply_closeall_cleanup;

apply_deltaread_error:
	if (progress)
		printf("\n");
	warn("could not properly read delta data for extent at offset "U64FMT": %s", extent_addr, strerror(-err));
	goto apply_freeall_cleanup;

apply_devread_error:
	if (progress)
		printf("\n");
	warn("could not read "U64FMT" chunk extent at offset "U64FMT" from downstream snapshot device \"%s\": %s", deh.num_of_chunks, extent_addr, dev1name, strerror(-err));
	goto apply_freeall_cleanup;

apply_compmem_error:
	if (progress)
		printf("\n");
	warn("not enough buffer memory for decompression of delta for "U64FMT" chunk extent starting at offset "U64FMT, deh.num_of_chunks, extent_addr);
	goto apply_freeall_cleanup;

apply_compbuf_error:
	if (progress)
		printf("\n");
	warn("not enough room in the output buffer for decompression of delta for "U64FMT" chunk extent starting at offset "U64FMT, deh.num_of_chunks, extent_addr);
	goto apply_freeall_cleanup;

apply_compdata_error:
	if (progress)
		printf("\n");
	warn("compressed data corrupted in delta for "U64FMT" chunk extent starting at offset "U64FMT, deh.num_of_chunks, extent_addr);
	goto apply_freeall_cleanup;

apply_chunk_error:
	err = -ERANGE; /* FIXME: find better error */
	if (progress)
		printf("\n");
	warn("delta could not be applied for "U64FMT" chunk extent with start address of "U64FMT, deh.num_of_chunks, extent_addr);
	goto apply_freeall_cleanup;

apply_checksum_error:
	err = -ERANGE;
	if (progress)
		printf("\n");
	warn("checksum failed for "U64FMT" chunk extent with start address of "U64FMT, deh.num_of_chunks, extent_addr);
	goto apply_freeall_cleanup;

apply_testread_error:
	if (progress)
		printf("\n");
	warn("could not read test data for "U64FMT" chunk extent with start address of "U64FMT" from delta stream: %s", deh.num_of_chunks, extent_addr, strerror(-err));
	goto apply_freeall_cleanup;

apply_write_error:
	if (progress)
		printf("\n");
	warn("updated extent could not be written at start address "U64FMT" in snapshot device \"%s\": %s", extent_addr, dev2name, strerror(-err));
	goto apply_freeupdate_cleanup;


	/* error cleanup */

apply_freeall_cleanup:
	if (up_extent2)
		free(up_extent2);
	if (up_extent1)
		free(up_extent1);
	if (comp_delta)
		free(comp_delta);
	if (extent_data)
		free(extent_data);
	if (delta_data)
		free(delta_data);

apply_freeupdate_cleanup:
	if (updated)
		free(updated);

apply_closeall_cleanup:
	close(snapdev2);

apply_closesrc_cleanup:
	close(snapdev1);

apply_cleanup:
	return err;
}

static int apply_delta(int deltafile, char const *devstem)
{
	struct delta_header dh;

	if (fdread(deltafile, &dh, sizeof(dh)) < 0) {
		warn("not a proper delta file (too short)\n");
		return -1; /* FIXME: use named error */
	}

	/* Make sure it's a proper delta file */
	if (strncmp(dh.magic, DELTA_MAGIC_ID, MAGIC_SIZE) != 0) {
		warn("not a proper delta file (wrong magic in header)\n");
		return -1; /* FIXME: use named error */
	}

	if (dh.chunk_size == 0) {
		warn("not a proper delta file (zero chunk size)\n");
		return -1; /* FIXME: use named error */
	}

	int err;
	char *dev1name;

	if (!(dev1name = malloc(strlen(devstem) + 32))) {
		warn("unable to allocate memory for dev1name\n");
		return -ENOMEM;
	}
	sprintf(dev1name, "%s%u", devstem, dh.src_snap);

	if ((err = apply_delta_extents(deltafile, dh.mode, dh.chunk_size, dh.chunk_num, dev1name, devstem, TRUE)) < 0) {
		free(dev1name);
		return err;
	}

	free(dev1name);

	return 0;
}

static int ddsnap_apply_delta(char const *deltaname, char const *devstem)
{
	int deltafile;

	deltafile = open(deltaname, O_RDONLY);
	if (deltafile < 0) {
		warn("could not open delta file \"%s\" for reading", deltaname);
		return 1;
	}

	if (apply_delta(deltafile, devstem) < 0) {
		warn("could not apply delta file \"%s\" to snapstem \"%s\"", deltaname, devstem);
		close(deltafile);
		return 1;
	}

	char test;

	if (read(deltafile, &test, 1) == 1)
		warn("extra data at end of delta file \"%s\"", deltaname);

	close(deltafile);

	return 0;
}

static int list_snapshots(int serv_fd, int verbose, int onlylast)
{
	int err;

	if ((err = outbead(serv_fd, LIST_SNAPSHOTS, struct create_snapshot, 0))) {
		error("%s (%i)", strerror(-err), -err);
		return 1;
	}

	struct head head;

	if (readpipe(serv_fd, &head, sizeof(head)) < 0)
		return eek();

	if (head.code != SNAPSHOT_LIST)
		error("received unexpected code=%x length=%u", head.code, head.length);

	if (head.length < sizeof(int32_t))
		error("reply length mismatch: expected >=%u, actual %u", sizeof(int32_t), head.length);

	int count;

	if (readpipe(serv_fd, &count, sizeof(int)) < 0)
		return eek();

	if (head.length != sizeof(int32_t) + count * sizeof(struct snapinfo))
		error("reply length mismatch: expected %u, actual %u", sizeof(int32_t) + count * sizeof(struct snapinfo), head.length);

	struct snapinfo *buffer = malloc(count * sizeof(struct snapinfo));

	if (readpipe(serv_fd, buffer, count * sizeof(struct snapinfo)) < 0)
		return eek();

	if (verbose)
		printf("Snapshot list:\n");

	int i;

	for (i = ((onlylast && count > 0) ? count - 1 : 0); i < count; i++) {
		if (verbose) {
			printf("Snapshot %d:\n", i);
			printf("\ttag= %u \t", buffer[i].snap);
			printf("priority= %d \t", (int)buffer[i].prio);
			printf("use count= %u \t", (unsigned int)buffer[i].usecnt);

			time_t snap_time = (time_t)buffer[i].ctime;
			char *ctime_str = ctime(&snap_time);
			if (ctime_str[strlen(ctime_str)-1] == '\n')
				ctime_str[strlen(ctime_str)-1] = '\0';

			printf("creation time= %s\n", ctime_str);
		} else {
			printf("%u ", buffer[i].snap);
		}
	}

	if (!verbose)
		printf("\n");

	return 0;
}

static int ddsnap_generate_changelist(int serv_fd, char const *changelist_filename, u32 src_snap, u32 tgt_snap)
{
	int change_fd = open(changelist_filename, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);

	if (change_fd < 0)
		error("unable to open file %s: %s", changelist_filename, strerror(errno));

	struct change_list *cl;

	if ((cl = stream_changelist(serv_fd, src_snap, tgt_snap)) == NULL) {
		fprintf(stderr, "could not generate change list between snapshots %u and %u\n", src_snap, tgt_snap);
		return 1;
	}

	int err = write_changelist(change_fd, cl);

	close(change_fd);

	if (err < 0)
		return 1;

	return 0;
}

static int delete_snapshot(int sock, u32 snaptag)
{
	if (outbead(sock, DELETE_SNAPSHOT, struct create_snapshot, snaptag) < 0) /* FIXME: why create_snapshot? */
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

	if (head.code != REPLY_DELETE_SNAPSHOT) {
		if (head.code == REPLY_ERROR) {
			warn("unable to delete snapshot %u", snaptag);
			return 1;
		}
		error("received unexpected code=%x length=%u", head.code, head.length);
	}

	return 0;
}

static int create_snapshot(int sock, u32 snaptag)
{
	trace_on(printf("sending snapshot create request %u\n", snaptag););

	if (outbead(sock, CREATE_SNAPSHOT, struct create_snapshot, snaptag) < 0)
		return eek();

	struct head head;
	unsigned maxbuf = 500;
	char buf[maxbuf];

	if (readpipe(sock, &head, sizeof(head)) < 0)
		return eek();
	assert(head.length < maxbuf); // !!! don't die
	if (readpipe(sock, buf, head.length) < 0)
		return eek();

	trace_on(printf("snapshot create got reply = %x\n", head.code););

	if (head.code != REPLY_CREATE_SNAPSHOT) {
		if (head.code == REPLY_ERROR) {
			warn("unable to create snapshot %u", snaptag);
			return 1;
		}
		error("received unexpected code=%x length=%u", head.code, head.length);
	}

	return 0;
}

static int set_priority(int sock, u32 snaptag, int8_t pri_val)
{
	if (outbead(sock, SET_PRIORITY, struct snapinfo, snaptag, pri_val) < 0)
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

static int usecount(int sock, u32 snaptag, int32_t usecnt_dev)
{
	if (outbead(sock, USECOUNT, struct usecount_info, snaptag, usecnt_dev) < 0)
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

	if (head.code == USECOUNT_ERROR) {
		struct usecount_error *usecnt_err = (struct usecount_error *) buf;
		usecnt_err->msg[(head.length-(sizeof(struct usecount_error)))-1] = '\0';
		error("received unexpected code: %s", usecnt_err->msg);
	}

	printf("New usecount: %hu\n", ((struct usecount_ok *) buf)->usecount);
 
	return 0;
}

static int ddsnap_delta_server(int lsock, char const *devstem)
{
	char const *origindev = devstem;

	for (;;) {
		int csock;

		if ((csock = accept_socket(lsock)) < 0) {
			warn("unable to accept connection: %s", strerror(-csock));
			continue;
		}

		trace_on(fprintf(stderr, "got client connection\n"););

		pid_t pid;

		if ((pid = fork()) < 0) {
			warn("unable to fork to service connection: %s", strerror(errno));
			goto cleanup_client;
		}

		if (pid != 0) {
			/* parent -- wait for another connection */
			goto cleanup_client;
		}

		trace_on(fprintf(stderr, "processing\n"););

		/* child */

		struct messagebuf message;
		int err;

		if ((err = readpipe(csock, &message.head, sizeof(message.head))) < 0) {
			warn("error reading upstream message header: %s", strerror(-err));
			goto end_connection;
		}
		if (message.head.length > maxbody) {
			warn("message body too long %u", message.head.length);
			goto end_connection;
		}
		if ((err = readpipe(csock, &message.body, message.head.length)) < 0) {
			warn("error reading upstream message body: %s", strerror(-err));
			goto end_connection;
		}

		struct send_delta body;

		switch (message.head.code) {
		case SEND_DELTA:
			if (message.head.length < sizeof(body)) {
				warn("incomplete SEND_DELTA request sent by client");
				outbead(csock, REPLY_ERROR, struct reply_error, REPLY_ERROR_OTHER, 0);
				goto end_connection;
			}

			memcpy(&body, message.body, sizeof(body));

			if (body.snap == (u32)~0UL) {
				warn("invalid snapshot %u in SEND_DELTA", body.snap);
				outbead(csock, REPLY_ERROR, struct reply_error, REPLY_ERROR_OTHER, 0);
				goto end_connection;
			}

			if (body.chunk_size == 0) {
				warn("invalid chunk size %u in SEND_DELTA", body.chunk_size);
				outbead(csock, REPLY_ERROR, struct reply_error, REPLY_ERROR_OTHER, 0);
				goto end_connection;
			}

			char *snapdev;

			if (!(snapdev = malloc(strlen(devstem)+32+1))) {
				warn("unable to allocate device name");
				outbead(csock, REPLY_ERROR, struct reply_error, REPLY_ERROR_OTHER, 0);
				goto end_connection;
			}
			sprintf(snapdev, "%s%u", devstem, body.snap);

			/* FIXME: verify snapshot exists */

			/* FIXME: In the future we should also lookup the client's address in a
			 * device permission table and check for replicatiosn already in progress.
			 */

			outbead(csock, SEND_DELTA_PROCEED, struct {});

			/* retrieve it */

			if (apply_delta_extents(csock, body.delta_mode, body.chunk_size, body.chunk_count, snapdev, origindev, TRUE) < 0) {
				warn("unable to apply upstream delta to device \"%s\"", origindev);
				outbead(csock, REPLY_ERROR, struct reply_error, REPLY_ERROR_OTHER, 0);
				goto end_connection;
			}

			free(snapdev);

			/* success */

			outbead(csock, SEND_DELTA_DONE, struct {});
			trace_on(fprintf(stderr, "applied streamed delta to \"%s\"\n", origindev););
			fprintf(stderr, "closing connection\n");
			close(csock);
			exit(0);

		default:
			warn("unexpected message type sent to snapshot replication server %x", message.head.code);
			outbead(csock, REPLY_ERROR, struct reply_error, REPLY_ERROR_OTHER, 0);
			goto end_connection;
		}

	end_connection:
		warn("closing connection on error\n");
		close(csock);
		exit(1);

	cleanup_client:
		close(csock);
	}

	return 0;
}

static u64 get_origin_sectors(int serv_fd)
{
	int err;

	if ((err = outbead(serv_fd, REQUEST_ORIGIN_SECTORS, struct {}))) {
		error("%s (%i)", strerror(-err), -err);
		return 0;
	}

	struct head head;

	if ((err = readpipe(serv_fd, &head, sizeof(head))) < 0)
		error("%s (%i)", strerror(-err), -err);

	if (head.code != ORIGIN_SECTORS) {
		if (head.code == REPLY_ERROR) {
			warn("unable to obtain origin sectors");
			return 0;
		}
		error("received unexpected code=%x length=%u", head.code, head.length);
	}

	struct origin_sectors body;

	if (head.length != sizeof(body))
		error("reply length mismatch: expected %u, actual %u", sizeof(body), head.length);

	if ((err = readpipe(serv_fd, &body, sizeof(body))) < 0)
		error("%s (%i)", strerror(-err), -err);

	return body.count;
}

static struct status *get_snap_status(struct status_message *message, unsigned int snaptag)
{
	struct status *status;

	status = (struct status *)(message->status_data +
					snaptag * (sizeof(struct status) +
					message->num_columns * sizeof(status->chunk_count[0])));

	return status;
}

static int ddsnap_get_status(int serv_fd, u32 snaptag, int verbose)
{
	int err;

	if ((err = outbead(serv_fd, STATUS_REQUEST, struct status_request, snaptag))) {
		error("%s (%i)", strerror(-err), -err);
		return 1;
	}

	struct head head;

	if (readpipe(serv_fd, &head, sizeof(head)) < 0) {
		error("received incomplete packet header");
		return 1;
	}

	if (head.code != STATUS_MESSAGE)
		error("received unexpected code=%x length=%u", head.code, head.length);

	if (head.length < sizeof(struct status_message))
		error("reply length mismatch: expected >=%u, actual %u", sizeof(struct status_message), head.length);

	struct status_message *reply;

	if (!(reply = malloc(head.length)))
		error("unable to allocate %u bytes for reply buffer\n", head.length);

	/* We won't bother to check that the lengths match because it would
	 * be ugly to read in the structure in pieces.
	 */
	if (readpipe(serv_fd, reply, head.length) < 0)
		return eek();

	if (reply->status_count > reply->num_columns)
		error("mismatched snapshot status count (%u) and the number of columns (%u)\n", reply->status_count, reply->num_columns);

	if (reply->store.chunksize_bits == 0 && reply->store.used == 0 && reply->store.free == 0) {
		printf("Chunk size: %lu\n", 1UL << reply->meta.chunksize_bits);
		printf("Used: %llu\n", reply->meta.used);
		printf("Free: %llu\n", reply->meta.free);
	} else {
		printf("Data chunk size: %lu\n", 1UL << reply->store.chunksize_bits);
		printf("Used data: %llu\n", reply->store.used);
		printf("Free data: %llu\n", reply->store.free);

		printf("Metadata chunk size: %lu\n", 1UL << reply->meta.chunksize_bits);
		printf("Used metadata: %llu\n", reply->meta.used);
		printf("Free metadata: %llu\n", reply->meta.free);
	}

	printf("Origin size: %llu\n", get_origin_sectors(serv_fd) << SECTOR_BITS);
	printf("Write density: %g\n", (double)reply->write_density/(double)0xffffffff);

	time_t time = (time_t)reply->ctime;
	char *ctime_str = ctime(&time);
	if (ctime_str[strlen(ctime_str)-1] == '\n')
		ctime_str[strlen(ctime_str)-1] = '\0';
	printf("Creation time: %s\n", ctime_str);

	unsigned int row, col;
	u64 total_chunks;

	printf("%6s %24s %8s %8s", "Snap", "Creation time", "Chunks", "Unshared");

	if (verbose) {
		for (col = 1; col < reply->num_columns; col++)
			printf(" %7dX", col);
	} else {
		printf(" %8s", "Shared");
	}

	printf("\n");

	struct status *snap_status;

	for (row = 0; row < reply->status_count; row++) {
		snap_status = get_snap_status(reply, row);

		printf("%6u", snap_status->snap);

		time = (time_t)snap_status->ctime;
		ctime_str = ctime(&time);
		if (ctime_str[strlen(ctime_str)-1] == '\n')
			ctime_str[strlen(ctime_str)-1] = '\0';
		printf(" %24s", ctime_str);

		total_chunks = 0;
		for (col = 0; col < reply->num_columns; col++)
			total_chunks += snap_status->chunk_count[col];

		printf(" %8llu", total_chunks);
		printf(" %8llu", snap_status->chunk_count[0]);

		if (verbose)
			for (col = 1; col < reply->num_columns; col++)
				printf(" %8llu", snap_status->chunk_count[col]);
		else
			printf(" %8llu", total_chunks - snap_status->chunk_count[0]);

		printf("\n");
	}

	/* if we are printing all the snapshot stats, also print totals for each
	 * degree of sharing
	 */

	if (snaptag == (u32)~0UL) {
		u64 *column_totals;

		if (!(column_totals = malloc(sizeof(u64) * reply->num_columns)))
			error("unable to allocate array for column totals\n");

		/* sum the columns and divide by their share counts */

		total_chunks = 0;
		for (col = 0; col < reply->num_columns; col++) {
			column_totals[col] = 0;
			for (row = 0; row < reply->status_count; row++) {
				snap_status = get_snap_status(reply, row);

				column_totals[col] += snap_status->chunk_count[col];
			}
			column_totals[col] /= col+1;

			total_chunks += column_totals[col];
		}

		printf("%6s", "totals");
		printf(" %24s", "");
		printf(" %8llu", total_chunks);
		if (reply->num_columns > 0)
			printf(" %8llu", column_totals[0]);
		else
			printf(" %8d", 0);

		if (verbose)
			for (col = 1; col < reply->num_columns; col++)
				printf(" %8llu", column_totals[col]);
		else if (reply->num_columns > 0)
			printf(" %8llu", total_chunks - column_totals[0]);
		else
			printf(" %8d", 0);

		printf("\n");

		free(column_totals);
	}

	free(reply);

	return 0;
}

static void mainUsage(void)
{
	printf("usage: ddsnap [-?|--help|--usage|--version] <subcommand>\n"
	       "\n"
	       "Available subcommands:\n"
	       "        initialize        Initialize snapshot storage device\n"
	       "        agent             Start the snapshot agent\n"
	       "        server            Start the snapshot server\n"
	       "	create            Create a snapshot\n"
	       "	delete            Delete a snapshot\n"
	       "	list              Return list of snapshots currently held\n"
	       "	priority          Set the priority of a snapshot\n"
	       "	usecount          Change the use count of a snapshot\n"
	       "        status            Report snapshot usage statistics\n"
	       "	delta             \n"
               "        usage: ddsnap delta [-?|--help|--usage] <subcommand>\n"
               "\n"
               "        Available delta subcommands:\n"
               "                changelist        Create a changelist given 2 snapshots\n"
	       "	        create            Create a delta file given a changelist and 2 snapshots\n"
	       "	        apply             Apply a delta file to a volume\n"
	       "	        send              Send a delta file to a downstream server\n"
	       "	        listen            Listen for a delta arriving from upstream\n");
}

static void deltaUsage(void)
{
	printf("usage: ddsnap delta [-?|--help|--usage] <subcommand>\n"
	       "\n"
               "Available delta subcommands:\n"
               "        changelist        Create a changelist given 2 snapshots\n"
	       "	create            Create a delta file given a changelist and 2 snapshots\n"
	       "	apply             Apply a delta file to a volume\n"
	       "	send              Send a delta file to a downstream server\n"
	       "        listen            Listen for a delta arriving from upstream\n");
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
	char const *logfile = NULL;
	char const *pidfile = NULL;
	struct poptOption serverOptions[] = {
		{ "foreground", 'f', POPT_ARG_NONE, &nobg, 0, "run in foreground. daemonized by default.", NULL },
		{ "logfile", 'l', POPT_ARG_STRING, &logfile, 0, "use specified log file", NULL },
		{ "pidfile", 'p', POPT_ARG_STRING, &pidfile, 0, "use specified process id file", NULL },
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

	int last = FALSE;
	int list = FALSE;
	int size = FALSE;
	int verb = FALSE;
	struct poptOption stOptions[] = {
		{ "last", '\0', POPT_ARG_NONE, &last, 0, "List the newest snapshot", NULL},
		{ "list", 'l', POPT_ARG_NONE, &list, 0, "List all active snapshots", NULL},
		{ "size", 's', POPT_ARG_NONE, &size, 0, "Print the size of the origin device", NULL},
		{ "verbose", 'v', POPT_ARG_NONE, &verb, 0, "Verbose sharing information", NULL},
		POPT_TABLEEND
	};

	struct poptOption deltaOptions[] = {
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Create changelist\n\t Function: Create a changelist given 2 snapshots\n\t Usage: delta changelist <sockname> <changelist> <snapshot1> <snapshot2>", NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &cdOptions, 0,
		  "Create delta\n\t Function: Create a delta file given a changelist and 2 snapshots\n\t Usage: delta create [OPTION...] <changelist> <deltafile> <snapshot1> <snapshot2>\n", NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Apply delta\n\t Function: Apply a delta file to a volume\n\t Usage: delta apply <deltafile> <devstem>", NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &cdOptions, 0,
		  "Send delta\n\t Function: Send a delta file to a downstream server\n\t Usage: delta send [OPTION...] <sockname> <snapshot1> <snapshot2> <devstem> <remsnapshot> <host>[:<port>]\n", NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &serverOptions, 0,
		  "Listen\n\t Function: Listen for a delta arriving from upstream\n\t Usage: delta listen [OPTION...] <devstem> [<host>[:<port>]]", NULL },
		POPT_TABLEEND
	};

	poptContext mainCon;
	struct poptOption mainOptions[] = {
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &initOptions, 0,
		  "Initialize\n\t Function: Initialize a snapshot storage volume\n\t Usage: initialize [OPTION...] <dev/snapshot> <dev/origin> [dev/meta]", NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &serverOptions, 0,
		  "Agent Server\n\t Function: Start the snapshot agent\n\t Usage: agent [OPTION...] <agent_socket>", NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &serverOptions, 0,
		  "Snapshot Server\n\t Function: Start the snapshot server\n\t Usage: server [OPTION...] <dev/snapshot> <dev/origin> [dev/meta] <agent_socket> <server_socket>", NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Create snapshot\n\t Function: Create a snapshot\n\t Usage: create <sockname> <snapshot>", NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Delete snapshot\n\t Function: Delete a snapshot\n\t Usage: delete <sockname> <snapshot>", NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "List snapshots\n\t Function: Return list of snapshots currently held\n\t Usage: list <sockname>", NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Priority\n\t Function: Set the priority of a snapshot\n\t Usage: priority <sockname> <snap_tag> <new_priority_value>", NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Usecount\n\t Function: Change the use count of a snapshot\n\t Usage: usecount <sockname> <snap_tag> <diff_amount>", NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &stOptions, 0,
		  "Get statistics\n\t Function: Report snapshot usage statistics\n\t Usage: status [OPTION...] <sockname> [<snapshot>]", NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &deltaOptions, 0,
		  "Delta\n\t Usage: delta [OPTION...] <subcommand> ", NULL},
		{ "version", 'V', POPT_ARG_NONE, NULL, 0, "Show version", NULL },
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
		exit(0);
	}

	poptFreeContext(mainCon);

	if (strcmp(command, "--version") == 0 || strcmp(command, "-V") == 0) {
		printf("ddsnap revision \"%s\"\n", SVN_VERSION);
		printf(" built on %s by %s@%s\n", BUILD_DATE, BUILD_USER, BUILD_HOST);
		exit(0);
	}

	if (strcmp(command, "--usage") == 0) {
		mainUsage();
		exit(0);
	}

	if (strcmp(command, "initialize") == 0) {
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

		if (!yes) {
			struct superblock *temp_sb;
			posix_memalign((void **)&temp_sb, 1 << SECTOR_BITS, SB_SIZE);
			
			if (diskread(sb->metadev, temp_sb, SB_SIZE, SB_SECTOR << SECTOR_BITS) < 0)
			warn("Unable to read superblock: %s", strerror(errno));

			if (!(memcmp(temp_sb->image.magic, SB_MAGIC, sizeof(temp_sb->image.magic)))) {
				char input;
				printf("There exists a valid magic number in sb, are you sure you want to overwrite? (y/N) ");
				input = getchar();
				if (input != 'y' && input != 'Y')
					return 1;
			}
			free(temp_sb);
		}

		poptFreeContext(initCon);
		
		trace_off(printf("js_bytes was %u & bs_bits was %u\n", js_bytes, bs_bits););

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

		trace_off(printf("js_bytes is %u & bs_bits is %u\n", js_bytes, bs_bits););

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
	if (strcmp(command, "agent") == 0) {
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

			if (!logfile)
				logfile = "/var/log/ddsnap.agent.log";
			pid = daemonize(logfile, pidfile);
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
	if (strcmp(command, "server") == 0) {
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

		if (diskread(sb->metadev, &sb->image, SB_SIZE, SB_SECTOR << SECTOR_BITS) < 0)
			warn("Unable to read superblock: %s", strerror(errno));

		poptFreeContext(serverCon);
		
		int listenfd, getsigfd, agentfd;
		
		if (snap_server_setup(agent_sockname, server_sockname, &listenfd, &getsigfd, &agentfd) < 0)
			error("Could not setup snapshot server\n");
		if (!nobg) {
			pid_t pid;
			
			if (!logfile)
				logfile = "/var/log/ddsnap.server.log";
			pid = daemonize(logfile, pidfile);
			if (pid == -1)
				error("Could not daemonize\n");
			if (pid != 0) {
				trace_on(printf("pid = %lu\n", (unsigned long)pid););
				return 0;
			}
		}
		if (snap_server(sb, listenfd, getsigfd, agentfd) < 0)
			error("Could not start snapshot server\n");
	
		return 0; /* not reached */
	}
	if (strcmp(command, "create") == 0) {
		if (argc != 4) {
			printf("Usage: %s create <sockname> <snapshot>\n", argv[0]);
			return 1;
		}

		u32 snaptag;

		if (strcmp(argv[3], "-1") == 0)
			snaptag = ~0UL; /* FIXME: we need a better way to represent the origin */
		else 
			if (parse_snaptag(argv[3], &snaptag) < 0) {
				fprintf(stderr, "%s: invalid snapshot %s\n", argv[0], argv[3]);
				return 1;
			}

		int sock = create_socket(argv[2]);

		int ret = create_snapshot(sock, snaptag);
		close(sock);
		return ret;
	}
	if (strcmp(command, "delete") == 0) {
		if (argc != 4) {
			printf("Usage: %s delete <sockname> <snapshot>\n", argv[0]);
			return 1;
		}

		u32 snaptag;

		if (strcmp(argv[3], "-1") == 0)
			snaptag = ~0UL; /* FIXME: we need a better way to represent the origin */
		else 
			if (parse_snaptag(argv[3], &snaptag) < 0) {
				fprintf(stderr, "%s: invalid snapshot %s\n", argv[0], argv[3]);
				return 1;
			}

		int sock = create_socket(argv[2]);

		int ret = delete_snapshot(sock, snaptag);
		close(sock);
		return ret;
	}
	if (strcmp(command, "list") == 0) {
		if (argc != 3) {
			printf("Usage: %s list <sockname>\n", argv[0]);
			return 1;
		}

		int sock = create_socket(argv[2]);

		int ret = list_snapshots(sock, TRUE, FALSE);
		close(sock);
		return ret;
	}
	if (strcmp(command, "priority") == 0) {
		if (argc != 5) {
			printf("usage: %s priority <sockname> <snap_tag> <new_priority_value>\n", argv[0]);
			return 1;
		}

		u32 snaptag;

		if (parse_snaptag(argv[3], &snaptag) < 0) {
			fprintf(stderr, "%s: invalid snapshot %s\n", argv[0], argv[3]);
			return 1;
		}

		int sock = create_socket(argv[2]);

		int ret = set_priority(sock, snaptag, atoi(argv[4]));
		close(sock);
		return ret;
	}
	if (strcmp(command, "usecount") == 0) {
		if (argc != 5) {
			printf("usage: %s usecount <sockname> <snap_tag> <diff_amount>\n", argv[0]);
			return 1;
		}

		u32 snaptag;

		if (parse_snaptag(argv[3], &snaptag) < 0) {
			fprintf(stderr, "%s: invalid snapshot %s\n", argv[0], argv[3]);
			return 1;
		}

		int sock = create_socket(argv[2]);

		int ret = usecount(sock, snaptag, atoi(argv[4]));
		close(sock);
		return ret;
	}
	if (strcmp(command, "status") == 0) {
		poptContext cdCon;

		struct poptOption options[] = {
			{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &stOptions, 0, NULL, NULL },
			POPT_AUTOHELP
			POPT_TABLEEND
		};

		cdCon = poptGetContext(NULL, argc-1, (const char **)&(argv[1]), options, 0);
		poptSetOtherOptionHelp(cdCon, "<sockname> [<snapshot>]");

		char c;

		while ((c = poptGetNextOpt(cdCon)) >= 0);
		if (c < -1) {
			fprintf(stderr, "%s: %s: %s\n", argv[0], poptBadOption(cdCon, POPT_BADOPTION_NOALIAS), poptStrerror(c));
			poptFreeContext(cdCon);
			return 1;
		}

		if (last+list+size+verb > 1) {
			fprintf(stderr, "%s %s: Incompatible status options specified\n", argv[0], argv[1]);
			poptPrintUsage(cdCon, stderr, 0);
			poptFreeContext(cdCon);
			return 1;
		}

		if (last) {
			char const *sockname;

			sockname = poptGetArg(cdCon);

			if (sockname == NULL)
				cdUsage(cdCon, 1, argv[0], "Must specify socket name to status\n");
			if (poptPeekArg(cdCon) != NULL)
				cdUsage(cdCon, 1, argv[0], "Too many arguments to status\n");

			poptFreeContext(cdCon);

			int sock = create_socket(sockname);

			int ret = list_snapshots(sock, FALSE, TRUE);
			close(sock);

			return ret;
		} else if (list) {
			char const *sockname;

			sockname = poptGetArg(cdCon);

			if (sockname == NULL)
				cdUsage(cdCon, 1, argv[0], "Must specify socket name to status\n");
			if (poptPeekArg(cdCon) != NULL)
				cdUsage(cdCon, 1, argv[0], "Too many arguments to status\n");

			poptFreeContext(cdCon);

			int sock = create_socket(sockname);

			int ret = list_snapshots(sock, FALSE, FALSE);
			close(sock);

			return ret;
		} else if (size) {
			char const *sockname;

			sockname = poptGetArg(cdCon);

			if (sockname == NULL)
				cdUsage(cdCon, 1, argv[0], "Must specify socket name to status\n");
			if (poptPeekArg(cdCon) != NULL)
				cdUsage(cdCon, 1, argv[0], "Too many arguments to status\n");

			poptFreeContext(cdCon);

			int sock = create_socket(argv[2]);

			printf("%llu\n", get_origin_sectors(sock));
			close(sock);

			return 0;
		} else {
			char const *sockname, *snaptagstr;

			sockname   = poptGetArg(cdCon);
			snaptagstr = poptGetArg(cdCon);

			if (sockname == NULL)
				cdUsage(cdCon, 1, argv[0], "Must specify socket name to status\n");
			if (poptPeekArg(cdCon) != NULL)
				cdUsage(cdCon, 1, argv[0], "Too many arguments to status\n");

			poptFreeContext(cdCon);

			u32 snaptag;

			if (snaptagstr) {
				if (parse_snaptag(snaptagstr, &snaptag) < 0) {
					fprintf(stderr, "%s: invalid snapshot %s\n", argv[0], snaptagstr);
					return 1;
				}
			} else {
				snaptag = ~0UL; /* meaning "all snapshots" */
			}

			int sock = create_socket(sockname);

			int ret = ddsnap_get_status(sock, snaptag, verb);
			close(sock);

			return ret;
		}
	}
	if (strcmp(command, "delta") == 0) {
		poptContext deltaCon;
		struct poptOption options[] = {
			{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &deltaOptions, 0, NULL, NULL },
			POPT_AUTOHELP
			POPT_TABLEEND
		};
		deltaCon = poptGetContext(NULL, argc-1, (const char **)&(argv[1]), options, 0);

		if (argc < 3) {
			poptPrintHelp(deltaCon, stdout, 0);
			poptFreeContext(deltaCon);
			exit(1);
		}
		
		char const *subcommand = argv[2];
		
		if (strcmp(subcommand, "--help") == 0 || strcmp(subcommand, "-?") == 0) {
			poptPrintHelp(deltaCon, stdout, 0);
			poptFreeContext(deltaCon);
			exit(0);
		}
		
		poptFreeContext(deltaCon);
		
		if (strcmp(subcommand, "--usage") == 0) {
			deltaUsage();
			exit(0);
		}

		if (strcmp(subcommand, "changelist") == 0) {
			if (argc != 7) {
				printf("usage: %s %s changelist <sockname> <changelist> <snapshot1> <snapshot2>\n", argv[0], argv[1]);
				return 1;
			}

			u32 snaptag1, snaptag2;

			if (parse_snaptag(argv[5], &snaptag1) < 0) {
				fprintf(stderr, "%s %s: invalid snapshot %s\n", argv[0], argv[1], argv[5]);
				return 1;
			}

			if (parse_snaptag(argv[6], &snaptag2) < 0) {
				fprintf(stderr, "%s %s: invalid snapshot %s\n", argv[0], argv[1], argv[6]);
				return 1;
			}

			int sock = create_socket(argv[3]);

			int ret = ddsnap_generate_changelist(sock, argv[4], snaptag1, snaptag2);
			close(sock);
			return ret;
		}
		if (strcmp(subcommand, "create") == 0) {
			char cdOpt;
			poptContext cdCon;

			struct poptOption options[] = {
				{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &cdOptions, 0, NULL, NULL },
				POPT_AUTOHELP
				POPT_TABLEEND
			};

			cdCon = poptGetContext(NULL, argc-2, (const char **)&(argv[2]), options, 0);
			poptSetOtherOptionHelp(cdCon, "<changelist> <deltafile> <devstem>");

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
			if (xd+raw+test+opt_comp > 1) {
				fprintf(stderr, "%s %s: Too many chunk options were selected.\nPlease select only one: -x, -r, -t or -o\n", argv[0], argv[1]);
				poptPrintUsage(cdCon, stderr, 0);
				poptFreeContext(cdCon);
				return 1;
			}

			u32 mode = (test ? TEST : (raw ? RAW : (xd? XDELTA : OPT_COMP)));
			if (opt_comp)
				gzip_level = MAX_GZIP_COMP;
		
			trace_on(fprintf(stderr, "xd=%d raw=%d test=%d opt_comp=%d mode=%u gzip_level=%d\n", xd, raw, test, opt_comp, mode, gzip_level););

			char const *changelist, *deltafile, *devstem;

			changelist = poptGetArg(cdCon);
			deltafile  = poptGetArg(cdCon);
			devstem    = poptGetArg(cdCon);

			if (changelist == NULL)
				cdUsage(cdCon, 1, "Specify a changelist", ".e.g., cl01\n");
			if (deltafile == NULL)
				cdUsage(cdCon, 1, "Specify a deltafile", ".e.g., df01\n");
			if (devstem == NULL)
				cdUsage(cdCon, 1, "Specify a devstem", ".e.g., /dev/mapper/snap\n");
			if (poptPeekArg(cdCon) != NULL)
				cdUsage(cdCon, 1, "Too many arguments inputted", "\n");

			int ret = ddsnap_generate_delta(mode, gzip_level, changelist, deltafile, devstem);

			poptFreeContext(cdCon);
			return ret;
		}
		if (strcmp(subcommand, "apply") == 0) {
			if (argc != 5) {
				printf("usage: %s %s apply <deltafile> <devstem>\n", argv[0], argv[1]);
				return 1;
			}
			return ddsnap_apply_delta(argv[3], argv[4]);
		}
		if (strcmp(subcommand, "send") == 0) {
			poptContext cdCon;

			struct poptOption options[] = {
				{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &cdOptions, 0, NULL, NULL },
				POPT_AUTOHELP
				POPT_TABLEEND
			};

			cdCon = poptGetContext(NULL, argc-2, (const char **)&(argv[2]), options, 0);
			poptSetOtherOptionHelp(cdCon, "<sockname> <snapshot1> <snapshot2> <snapstem> <remsnapshot> <host>[:<port>]");

			char c;

			while ((c = poptGetNextOpt(cdCon)) >= 0);
			if (c < -1) {
				fprintf(stderr, "%s: %s: %s\n", argv[0], poptBadOption(cdCon, POPT_BADOPTION_NOALIAS), poptStrerror(c));
				poptFreeContext(cdCon);
				return 1;
			}

			/* Make sure the options are mutually exclusive */
			if (xd+raw+test+opt_comp > 1) {
				fprintf(stderr, "%s %s: Too many chunk options were selected.\nPlease select only one: -x, -r, -t or -o\n", argv[0], argv[1]);
				poptPrintUsage(cdCon, stderr, 0);
				poptFreeContext(cdCon);
				return 1;
			}

			u32 mode = (test ? TEST : (raw ? RAW : (xd ? XDELTA : OPT_COMP)));
			if (opt_comp)
				gzip_level = MAX_GZIP_COMP;

			trace_on(fprintf(stderr, "xd=%d raw=%d test=%d opt_comp=%d mode=%u gzip_level=%d\n", xd, raw, test, opt_comp, mode, gzip_level););

			char const *sockname, *snaptag1str, *snaptag2str, *snapstemstr, *snaptagremstr, *hoststr;

			sockname      = poptGetArg(cdCon);
			snaptag1str   = poptGetArg(cdCon);
			snaptag2str   = poptGetArg(cdCon);
			snapstemstr   = poptGetArg(cdCon);
			snaptagremstr = poptGetArg(cdCon);
			hoststr       = poptGetArg(cdCon);

			if (hoststr == NULL)
				cdUsage(cdCon, 1, argv[0], "Not enough arguments to send-delta\n");
			if (poptPeekArg(cdCon) != NULL)
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

			char *snapdev1 = (char *) malloc (strlen(snapstemstr) + strlen(snaptag1str) + 1);
			char *snapdev2 = (char *) malloc (strlen(snapstemstr) + strlen(snaptag2str) + 1);

			snapdev1 = strcpy(snapdev1, snapstemstr);
			snapdev1 = strcat(snapdev1, snaptag1str);
			snapdev2 = strcpy(snapdev2, snapstemstr);
			snapdev2 = strcat(snapdev2, snaptag2str);

			u32 snaptag1, snaptag2, remsnaptag;

			if (parse_snaptag(snaptag1str, &snaptag1) < 0) {
				fprintf(stderr, "%s %s: invalid snapshot %s\n", argv[0], argv[1], snaptag1str);
				return 1;
			}

			if (parse_snaptag(snaptag2str, &snaptag2) < 0) {
				fprintf(stderr, "%s %s: invalid snapshot %s\n", argv[0], argv[1], snaptag2str);
				return 1;
			}

			if (parse_snaptag(snaptagremstr, &remsnaptag) < 0) {
				fprintf(stderr, "%s %s: invalid snapshot %s\n", argv[0], argv[1], snaptagremstr);
				return 1;
			}

			int sock = create_socket(sockname);

			int ds_fd = open_socket(hostname, port);
			if (ds_fd < 0) {
				fprintf(stderr, "%s %s: unable to connect to downstream server %s port %u\n", argv[0], argv[1], hostname, port);
				return 1;
			}

			struct sigaction ign_sa;

			ign_sa.sa_handler = SIG_IGN;
			sigemptyset(&ign_sa.sa_mask);
			ign_sa.sa_flags = 0;

			if (sigaction(SIGPIPE, &ign_sa, NULL) == -1)
				warn("could not disable SIGPIPE: %s", strerror(errno));

			int ret = ddsnap_send_delta(sock, snaptag1, snaptag2, snapdev1, snapdev2, remsnaptag, mode, gzip_level, ds_fd);
			close(ds_fd);
			close(sock);

			return ret;
		}
		if (strcmp(subcommand, "listen") == 0) {
			char const *devstem;
			char const *hostspec;

			struct poptOption options[] = {
				{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &serverOptions, 0, NULL, NULL },
				POPT_AUTOHELP
				POPT_TABLEEND
			};

			poptContext dsCon = poptGetContext(NULL, argc-2, (const char **)&(argv[2]), options, 0);
			poptSetOtherOptionHelp(dsCon, "<devstem> <host>[:<port>]");

			char dsOpt = poptGetNextOpt(dsCon);

			if (dsOpt < -1) {
				fprintf(stderr, "%s %s: %s: %s\n", command, subcommand, poptBadOption(dsCon, POPT_BADOPTION_NOALIAS), poptStrerror(dsOpt));
				poptFreeContext(dsCon);
				return 1;
			}

			devstem = poptGetArg(dsCon);
			if (devstem == NULL) {
				fprintf(stderr, "%s %s: device stem must be specified\n", command, subcommand);
				poptPrintUsage(dsCon, stderr, 0);
				poptFreeContext(dsCon);
				return 1;
			}

			hostspec = poptGetArg(dsCon);

			if (poptPeekArg(dsCon) != NULL) {
				fprintf(stderr, "%s %s: only one host may be specified\n", command, subcommand);
				poptPrintUsage(dsCon, stderr, 0);
				poptFreeContext(dsCon);
				return 1;
			}

			poptFreeContext(dsCon);

			char *hostname;
			unsigned port;

			if (!hostspec) {
				hostname = strdup("0.0.0.0");
				port = DEFAULT_REPLICATION_PORT;
			} else {
				hostname = strdup(hostspec);

				if (strchr(hostname, ':')) {
					unsigned int len = strlen(hostname);
					port = parse_port(hostname, &len);
					hostname[len] = '\0';
				} else {
					port = DEFAULT_REPLICATION_PORT;
				}
			}

			/* make sure origin device exists (catch typos early) */
			int origin = open(devstem, O_RDONLY);
			if (origin < 0) {
				fprintf(stderr, "%s %s: unable to open origin device \"%s\" for reading: %s\n", command, subcommand, devstem, strerror(errno));
				return 1;
			}

			close(origin);

			int sock = bind_socket(hostname, port);
			if (sock < 0) {
				fprintf(stderr, "%s %s: unable to bind to %s port %u\n", command, subcommand, hostname, port);
				return 1;
			}

			if (!nobg) {
				pid_t pid;

				if (!logfile)
					logfile = "/var/log/ddsnap.delta.log";
				pid = daemonize(logfile, pidfile);
				if (pid == -1)
					error("Error: could not daemonize\n");
				if (pid != 0) {
					trace_on(printf("pid = %lu\n", (unsigned long)pid););
					return 0;
				}
			}

			return ddsnap_delta_server(sock, devstem);
		}

		fprintf(stderr, "%s %s: unrecognized delta subcommand: %s.\n", argv[0], command, subcommand);
		return 1;
	}

	fprintf(stderr, "%s: unrecognized subcommand: %s.\n", argv[0], command);
	
	return 1;
}
