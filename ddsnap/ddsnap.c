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
#define BEST_COMP (1 << 3)

#define DEF_GZIP_COMP 0
#define MAX_GZIP_COMP 9

#define MAX_MEM_BITS 20
#define MAX_MEM_SIZE (1 << MAX_MEM_BITS)
#define DEF_CHUNK_SIZE_BITS SECTOR_BITS + SECTORS_PER_BLOCK;

struct cl_header
{
	char magic[MAGIC_SIZE];
	u32 chunksize_bits;
	u32 src_snap;
	u32 tgt_snap;
};

struct vol_header 
{
	char magic[MAGIC_SIZE];
	u64 vol_size_bytes;
	u32 chunksize_bits;
};

struct delta_header
{
	char magic[MAGIC_SIZE];
	u64 chunk_num;
	u32 chunk_size;
	u32 vol_device;
	u32 src_snap;
	u32 tgt_snap;
};

struct delta_extent_header
{
	u32 magic_num;
	u32 mode;
	u32 gzip_on;
	u64 extent_addr;
	u64 num_of_chunks;
	u64 extents_delta_length;
	u64 ext1_chksum;
	u64 ext2_chksum;
};

static u64 checksum(const unsigned char *data, u32 data_length)
{
	u64 result = 0;
	u32 i;

	for (i = 0; i < data_length; i++)
		result = result + data[i];

	return result;
}

static char *get_message(int sock, unsigned int length) 
{
	char *buffer;
	if (!(buffer = malloc(length)))
		return NULL;

	int err;
	if ((err = readpipe(sock, buffer, length)) < 0) {
		warn("unable to get reason why command failed: %s", strerror(err));
		free(buffer);
		return NULL;
	}
	buffer[length-1] = '\0';
	
	return buffer;
}

static void error_message_handler(int sock, const char *prefix, unsigned int length) 
{
	char *buffer = get_message(sock, length);
	warn("%s : %s", prefix, (buffer? buffer : "no error message"));
	if (buffer)
		free(buffer);
}

static void unknown_message_handler(int sock, struct head *head) 
{
	if (head->code != PROTOCOL_ERROR) {
		warn("received unexpected code=%x length=%u", head->code, head->length);
		return;
	}

	/* process protocol error message */
	int err;
	struct protocol_error pe;
	
	if (head->length < sizeof(struct protocol_error)) {
		warn("message too short");
		return;
	}
	
	if ((err = readpipe(sock, &pe, sizeof(struct protocol_error))) < 0) {
		warn("unable to receive protocol error message: %s", strerror(-err));
		return;
	}
	
	if (head->length - sizeof(struct protocol_error) <= 0)
		return;
	error_message_handler(sock, "protocol error message", 
			head->length - sizeof(struct protocol_error));
		
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
	if (snapstr[0] == '\0' || snapstr[0] == '-')
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
		warn("Not a proper changelist file (too short for header)");
		return NULL;
	}

	if (strncmp(clh.magic, CHANGELIST_MAGIC_ID, MAGIC_SIZE) != 0) {
		warn("Not a proper changelist file (wrong magic in header: %s)", clh.magic);
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
			error("Incomplete chunk address.");
			break;
		}

		if (chunkaddr == -1)
			break;

		if (append_change_list(cl, chunkaddr) < 0)
			error("unable to append chunk address.");
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

	if ((err = fdwrite(change_fd, &marker, sizeof(marker))) < 0) {
		warn("unable to write changelist marker: %s", strerror(-err));
		return err;
	}

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

static int create_raw_delta(struct delta_extent_header *deh_ptr, const unsigned char *input_buffer, unsigned char *output_buffer, u64 input_size, u64 *output_size) 
{
	memcpy(output_buffer, input_buffer, input_size);
	*output_size = input_size;
	deh_ptr->mode = RAW;
	deh_ptr->extents_delta_length = input_size;
	return 0;
}

static int create_xdelta_delta(struct delta_extent_header *deh_ptr, unsigned char *input_buffer1, unsigned char *input_buffer2, unsigned char *output_buffer, u64 input_size, u64 *output_size) 
{
	trace_off(printf("create xdelta delta\n"););
	int err;
	int ret = create_delta_chunk(input_buffer1, input_buffer2, output_buffer, input_size, (int *)output_size);
	deh_ptr->mode = XDELTA;			
	deh_ptr->extents_delta_length = *output_size;
	
	/* If xdelta result is larger than or equal to extent_size, we want to just copy over the raw extent */
	if (ret == BUFFER_SIZE_ERROR || *output_size == input_size) {
		trace_off(printf("extents_delta size is larger than or equal to extent_size\n"););
		create_raw_delta(deh_ptr, input_buffer2, output_buffer, input_size, output_size);
	} else if (ret < 0) {
		goto gen_create_error;
	} else if (ret >= 0) {
		/* sanity test for xdelta creation */
		unsigned char *delta_test = malloc(input_size);
		if (!delta_test)
			goto gen_alloc_error;

		ret = apply_delta_chunk(input_buffer1, delta_test, output_buffer, input_size, *output_size);
		
		if (ret != input_size) {
			free(delta_test);
			goto gen_applytest_error;
		}
		
		if (memcmp(delta_test, input_buffer2, input_size) != 0) {
			trace_off(printf("generated delta does not match extent on disk.\n"););			
			create_raw_delta(deh_ptr, input_buffer2, output_buffer, input_size, output_size);			
		}
		trace_off(printf("able to generate delta\n"););
		free(delta_test);
	}
	return 0;

gen_create_error:
	err = -ERANGE;
	printf("\n");
	warn("unable to create delta: %s", strerror(-err));
	return err;

gen_alloc_error:
	err = -ENOMEM;
	printf("\n");
	warn("memory allocation failed: %s", strerror(-err));
	return err;

gen_applytest_error:
	err = -ERANGE;
	printf("\n");
	warn("test application of delta failed: %s", strerror(-err));
	return err;
}

static int gzip_on_delta(struct delta_extent_header *deh_ptr, unsigned char *input_buffer, unsigned char *output_buffer, u64 input_size, u64 *output_size, int level) 
{
	int err;
	*output_size = input_size + 12 + (input_size >> 9);
	int comp_ret = compress2(output_buffer, (unsigned long *) output_size, input_buffer, input_size, level);
	
	if (comp_ret == Z_MEM_ERROR)
		goto gen_compmem_error;
	if (comp_ret == Z_BUF_ERROR)
		goto gen_compbuf_error;
	if (comp_ret == Z_STREAM_ERROR)
		goto gen_compstream_error;
			
	if (*output_size < input_size) {
		deh_ptr->gzip_on = TRUE;
		deh_ptr->extents_delta_length = *output_size;
	} else {
		deh_ptr->gzip_on = FALSE;
		deh_ptr->extents_delta_length = input_size;
		memcpy(output_buffer, input_buffer, input_size);
		*output_size = input_size;
	}
	return 0;

gen_compmem_error:
	err = -ENOMEM;
	printf("\n");
	warn("not enough buffer memory for compression of delta: %s", strerror(-err));
	return err;

gen_compbuf_error:
	err = -ERANGE;
	printf("\n");
	warn("not enough room in the output buffer for compression of delta: %s", strerror(-err));
	return err;

gen_compstream_error:
	err = -ERANGE;
	printf("\n");
	warn("invalid compression parameter level=%d delta_size=%llu in delta: %s", level, input_size, strerror(-err));
	return err;
}

static struct status_message * generate_status(int serv_fd, u32 snaptag)
{
	int err;
	struct status_message *reply;

	if ((err = outbead(serv_fd, STATUS, struct status_request, snaptag))) {
		warn("unable to send status request: %s", strerror(-err));
		return NULL;
	}

	struct head head;

	if ((err = readpipe(serv_fd, &head, sizeof(head))) < 0) {
		warn("received incomplete packet header: %s", strerror(-err));
		return NULL;
	}

	if (head.code != STATUS_OK) {
		if (head.code != STATUS_ERROR) {
			unknown_message_handler(serv_fd, &head);
			return NULL;
		}
		error_message_handler(serv_fd, "server reason why status failed", head.length);
		return NULL;
	}

	if (head.length < sizeof(struct status_message)) {
		warn("status length mismatch: expected >=%u, actual %u", sizeof(struct status_message), head.length);
		return NULL;
	}

	if (!(reply = malloc(head.length))) {
		warn("unable to allocate %u bytes for reply buffer", head.length);
		return NULL;
	}

	/* We won't bother to check that the lengths match because it would
	 * be ugly to read in the structure in pieces.
	 */
	if ((err = readpipe(serv_fd, reply, head.length)) < 0) {
		warn("received incomplete status message: %s", strerror(-err));
		free(reply);
		return NULL;
	}

	if (reply->status_count > reply->num_columns) {
		warn("mismatched snapshot status count (%u) and the number of columns (%u)", reply->status_count, reply->num_columns);
		free(reply);
		return NULL;
	}

	return reply;
}

static int generate_vol_extents(int serv_fd, int volfile, char const *devname, int progress)
{
	int vol_dev;
	int err = 0, ret;
	
	vol_dev = open(devname, O_RDONLY);
	if (vol_dev < 0) {
		err = -errno;
		goto vol_open_error;
	}

	u64 vol_size_bytes;
	if ((err = fd_size(vol_dev, &vol_size_bytes)) < 0)
		goto vol_size_error;

        /* chunk size set up */
	struct status_message *reply;
	reply = generate_status(serv_fd, ~0UL);
	if (!reply)
		goto vol_chunksize_error;
	u32 chunksize_bits = reply->meta.chunksize_bits;
	u64 chunk_size = 1 << chunksize_bits;

	/* Delta header set-up */
	struct delta_header dh;
	strncpy(dh.magic, DELTA_MAGIC_ID, sizeof(dh.magic));
	dh.chunk_num = vol_size_bytes >> chunksize_bits;
	dh.chunk_size = chunk_size;
	dh.vol_device = TRUE;

	if ((err = fdwrite(volfile, &dh, sizeof(dh))) < 0)
		goto vol_cleanup;

	trace_on(fprintf(stderr, "writing delta file with chunk_num="U64FMT" chunk_size=%u\n", dh.chunk_num, dh.chunk_size););

	/* Variable set up */
	u64 extent_size = MAX_MEM_SIZE;
	u64 num_of_extents = vol_size_bytes >> MAX_MEM_BITS;

	if ((vol_size_bytes % extent_size) != 0)
		num_of_extents++;

	unsigned char *dev_extent = malloc(extent_size);
	unsigned char *gzip_delta = malloc(extent_size + 12 + (extent_size >> 9));

	if (!dev_extent || !gzip_delta)
       		goto vol_alloc_error;

	trace_on(printf("opened vol device vol=%d to create vol_file.\n", vol_dev););
	trace_off(printf("devname: %s\n", devname););
	trace_on(printf("starting vol generation\n"););

	struct delta_extent_header deh = { .magic_num = MAGIC_NUM };
	u64 extent_addr = 0, gzip_size;
	u64 extent_num;

	for (extent_num = 1; extent_num <= num_of_extents; extent_num++) {
	       		
		/* read in extent from volume */
		if ((ret = pread(vol_dev, dev_extent, extent_size, extent_addr)) < 0) {
			err = ret;
			goto vol_readvol_error;
		}

		/* delta extent header set-up*/
		deh.mode = RAW;
		deh.gzip_on = TRUE;
		deh.extent_addr = extent_addr;

		/* fix: assume extent is always a multiple of chunk */
		deh.num_of_chunks = ret >> chunksize_bits;
		deh.extents_delta_length = ret;
		deh.ext2_chksum = checksum((const unsigned char *) dev_extent, ret);
			
		if ((err = gzip_on_delta(&deh, dev_extent, gzip_delta, ret, &gzip_size, MAX_GZIP_COMP)) < 0)
			goto vol_error_source;

		/* write the delta extent header and extents_delta to the delta file*/
		trace_off(printf("writing delta for extent starting at chunk "U64FMT", address "U64FMT"\n", extent_num, extent_addr););
		if ((err = fdwrite(volfile, &deh, sizeof(deh))) < 0)
			goto vol_writehead_error;
		if ((err = fdwrite(volfile, gzip_delta, deh.extents_delta_length)) < 0)
			goto vol_writedata_error;

		extent_addr += ret;

		if (progress) {
#ifdef DEBUG_GEN
			printf("Generating extent "U64FMT"/"U64FMT" ("U64FMT"%%)\n", extent_num, num_of_extents, (extent_num * 100) / num_of_extents);
#else
			printf("\rGenerating extent "U64FMT"/"U64FMT" ("U64FMT"%%)", extent_num, num_of_extents, (extent_num * 100) / num_of_extents);
			fflush(stdout);
#endif
		}
	}

	/* clean up: release memory and close file */
	free(gzip_delta);
	free(dev_extent);

	if (progress)
		printf("\n");

	trace_on(printf("All extents written to vol delta\n"););

	return 0;

	/* error messages*/
vol_open_error:
	warn("could not open snapshot device \"%s\" for reading: %s", devname, strerror(-err));
	goto vol_cleanup;

vol_size_error:
	warn("could not acquire volume size: %s", strerror(-err));
	goto vol_cleanup;

vol_chunksize_error:
	warn("could not acquire chunksize");
	goto vol_cleanup;

vol_alloc_error:
	err = -ENOMEM;
	warn("memory allocation failed while generating a "U64FMT" extent starting at offset "U64FMT": %s", extent_num, extent_addr, strerror(-err));
	goto vol_cleanup;

vol_readvol_error:
	if (progress)
		printf("\n");
	warn("read from snapshot device \"%s\" failed ", devname);
	goto vol_error_source;

vol_writehead_error:
	if (progress)
		printf("\n");
	warn("unable to write delta header ");
	goto vol_error_source;

vol_writedata_error:
	if (progress)
		printf("\n");
	warn("unable to write delta data ");
	goto vol_error_source;

vol_error_source:
	warn("for "U64FMT" extent starting at offset "U64FMT": %s", extent_num, extent_addr, strerror(-err));
	goto vol_free_cleanup;

	/* error cleanup */
vol_free_cleanup:
	if (dev_extent)
		free(dev_extent);
	if (gzip_delta)
		free(gzip_delta);

vol_cleanup:
	free(reply);
	return err;
}

static int ddsnap_generate_vol(int serv_fd, char const *vol_filename, char const *vol_dev)
{
	int vol_file = open(vol_filename, O_CREAT|O_WRONLY|O_TRUNC, S_IRWXU);
	if (vol_file < 0) {
		warn("could not create volume file \"%s\": %s", vol_filename, strerror(errno));
		return 1;
	}

	if (generate_vol_extents(serv_fd, vol_file, vol_dev, TRUE) < 0) {
		warn("could not write delta file \"%s\"", vol_filename);
		close(vol_file);
		return 1;
	}

	close(vol_file);
	return 0;
}

static int generate_delta_extents(u32 mode, int level, struct change_list *cl, int deltafile, char const *dev1name, char const *dev2name, int progress)
{
	int snapdev1, snapdev2;
	int err = 0;
	
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

	/* Variable set up */
	u32 chunk_size = 1 << cl->chunksize_bits;
	unsigned char *dev1_extent   = malloc(MAX_MEM_SIZE);
	unsigned char *dev2_extent   = malloc(MAX_MEM_SIZE);
	unsigned char *extents_delta = malloc(MAX_MEM_SIZE);
	unsigned char *gzip_delta    = malloc(MAX_MEM_SIZE + 12 + (MAX_MEM_SIZE >> 9));

	u64 dev2_gzip_size;
	unsigned char *dev2_gzip_extent = malloc(MAX_MEM_SIZE + 12 + (MAX_MEM_SIZE >> 9));
	struct delta_extent_header deh2 = { .magic_num = MAGIC_NUM, .mode = RAW };

	if (!dev1_extent || !dev2_extent || !extents_delta || !gzip_delta || !dev2_gzip_extent)
       		goto gen_alloc_error;

	trace_on(printf("opened snapshot devices snap1=%d snap2=%d to create delta.\n", snapdev1, snapdev2););
	trace_off(printf("dev1name: %s\n", dev1name););
	trace_off(printf("dev2name: %s\n", dev2name););
	trace_on(printf("mode: %u\n", mode););
	trace_off(printf("level: %d\n", level););
	trace_off(printf("chunksize bits: %u\t", cl->chunksize_bits););
	trace_off(printf("chunk_count: "U64FMT"\n", cl->count););
	trace_on(printf("chunksize: %u\n", chunk_size););
	trace_on(printf("starting delta generation\n"););

	struct delta_extent_header deh = { .magic_num = MAGIC_NUM };
	u64 extent_addr, chunk_num, num_of_chunks;
	u64 extent_size, delta_size, gzip_size;

	for (chunk_num = 0; chunk_num < cl->count;) {
		
		extent_addr = cl->chunks[chunk_num] << cl->chunksize_bits;
		
		if (chunk_num == (cl->count - 1) )
			num_of_chunks = 1;
		else
			num_of_chunks = chunks_in_extent(cl, chunk_num, chunk_size);

		extent_size = chunk_size * num_of_chunks;
		
		/* read in extents from dev1 & dev2 */
		if ((err = diskread(snapdev1, dev1_extent, extent_size, extent_addr)) < 0)
			goto gen_readsnap1_error;
		if ((err = diskread(snapdev2, dev2_extent, extent_size, extent_addr)) < 0)
			goto gen_readsnap2_error;

		/* delta extent header set-up*/
		deh.gzip_on = FALSE;
		deh.extent_addr = extent_addr;
		deh.num_of_chunks = num_of_chunks;
		deh.ext1_chksum = checksum((const unsigned char *) dev1_extent, extent_size);
		deh.ext2_chksum = checksum((const unsigned char *) dev2_extent, extent_size);
		
		/* 3 different modes, raw (raw dev2 extent), xdelta (xdelta dev2 extent), best (either gzipped raw dev2 extent or gzipped xdelta dev2 extent) */
		if (mode == RAW) {
			if ((err = create_raw_delta (&deh, dev2_extent, extents_delta, extent_size, &delta_size)) < 0) 
				goto error_source;
			if ((err = gzip_on_delta(&deh, extents_delta, gzip_delta, delta_size, &gzip_size, level)) < 0)
				goto error_source;
		} else {
			if ((err = create_xdelta_delta (&deh, dev1_extent, dev2_extent, extents_delta, extent_size, &delta_size)) < 0)
				goto error_source;
			if ((err = gzip_on_delta(&deh, extents_delta, gzip_delta, delta_size, &gzip_size, level)) < 0)
				goto error_source;
			if (mode != XDELTA) {
				/* delta extent header set-up for dev2_extent */
				deh2.gzip_on = FALSE;
				deh2.extents_delta_length = extent_size;
				if ((err = gzip_on_delta(&deh2, dev2_extent, dev2_gzip_extent, extent_size, &dev2_gzip_size, level)) < 0)
					goto error_source;

				if (dev2_gzip_size <= gzip_size) {
					
					deh.mode = deh2.mode;
					deh.gzip_on = deh2.gzip_on;
					deh.extents_delta_length = deh2.extents_delta_length;

					memcpy(gzip_delta, dev2_gzip_extent, dev2_gzip_size);
				}
			}
		}

		/* write the delta extent header and extents_delta to the delta file*/
		trace_off(printf("writing delta for extent starting at chunk "U64FMT", address "U64FMT"\n", chunk_num, extent_addr););
		if ((err = fdwrite(deltafile, &deh, sizeof(deh))) < 0)
			goto gen_writehead_error;
		if ((err = fdwrite(deltafile, gzip_delta, deh.extents_delta_length)) < 0)
			goto gen_writedata_error;

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

	/* clean up: release memory and close file */
	free(dev2_gzip_extent);
	free(gzip_delta);
	free(extents_delta);
	free(dev2_extent);
	free(dev1_extent);

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

	/* error messages*/
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
	warn("read from snapshot device \"%s\" failed ", dev1name);
	goto error_source;

gen_readsnap2_error:
	if (progress)
		printf("\n");
	warn("read from snapshot device \"%s\" failed ", dev2name);
	goto error_source;

gen_writehead_error:
	if (progress)
		printf("\n");
	warn("unable to write delta header ");
	goto error_source;

gen_writedata_error:
	if (progress)
		printf("\n");
	warn("unable to write delta data ");
	goto error_source;

error_source:
	warn("for "U64FMT" chunk extent starting at offset "U64FMT": %s", num_of_chunks, extent_addr, strerror(-err));
	goto gen_free_cleanup;

	/* error cleanup */
gen_free_cleanup:
	if (dev1_extent)
		free(dev1_extent);
	if (dev2_extent)
		free(dev2_extent);
	if (extents_delta)
		free(extents_delta);
	if (gzip_delta)
		free(gzip_delta);
	if (dev2_gzip_extent)
		free(dev2_gzip_extent);

gen_closeall_cleanup:
	close(snapdev2);

gen_close1_cleanup:
	close(snapdev1);

gen_cleanup:
	return err;
}

static char *malloc_snapshot_name(const char *devstem, u32 id) 
{
	char *snapshot_name = NULL;
	int length = strlen(devstem) + 32; // !!! why 32?

	if (!(snapshot_name = malloc(length))) {
		warn("unable to allocate memory snapshot name");
		return NULL;
	}
	
	snprintf(snapshot_name, length, "%s(%u)", devstem, id);

	return snapshot_name;
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

	char *dev1name;
	char *dev2name;

	int err;

	if (!(dev1name = malloc_snapshot_name(devstem, dh.src_snap))) {
		warn("unable to allocate memory for dev2name");
		err = -ENOMEM;
		goto delta_cleanup;
	}

	if (!(dev2name = malloc_snapshot_name(devstem, dh.tgt_snap))) {
		warn("unable to allocate memory for dev2name");
		err = -ENOMEM;
		goto delta_free1_cleanup;
	}

	trace_on(fprintf(stderr, "writing delta file with chunk_num="U64FMT" chunk_size=%u mode=%u\n", dh.chunk_num, dh.chunk_size, mode););
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

	if ((err = outbead(serv_fd, STREAM_CHANGELIST, struct stream_changelist, src_snap, tgt_snap))) {
		warn("could not request change list: %s", strerror(-err));
		return NULL;
	}

	struct head head;

	if ((err = readpipe(serv_fd, &head, sizeof(head))) < 0) {
		warn("could not read change list message head: %s", strerror(-err));
		return NULL;
	}

	trace_on(printf("reply = %x\n", head.code););

	if (head.code != STREAM_CHANGELIST_OK) {
		warn("unable to obtain changelist between snapshot %u and %u", src_snap, tgt_snap);
		if (head.code != STREAM_CHANGELIST_ERROR) {
			unknown_message_handler(serv_fd, &head);
			return NULL;
		}
		error_message_handler(serv_fd, "downstream server reason why send delta failed",
				head.length);
		return NULL;
	}

	struct changelist_stream cl_head;

	if (head.length != sizeof(cl_head)) {
		warn("change list length mismatch: expected %u, actual %u", sizeof(cl_head), head.length);
		return NULL;
	}

	if ((err = readpipe(serv_fd, &cl_head, sizeof(cl_head))) < 0) {
		warn("could not read change list head: %s", strerror(-err));
		return NULL;
	}

	struct change_list *cl;

	if ((cl = malloc(sizeof(struct change_list))) == NULL) {
		/* FIXME: need to read the data anyway to clear the socket */
		warn("unable to allocate change list");
		return NULL;
	}

	cl->count = cl_head.chunk_count;
	cl->chunksize_bits = cl_head.chunksize_bits;
	cl->src_snap = src_snap;
	cl->tgt_snap = tgt_snap;

	if (cl->chunksize_bits == 0) {
		/* FIXME: need to read the data anyway to clear the socket */
		warn("invalid chunk size %u in REPLY_STREAM_CHANGE_LIST", cl->chunksize_bits);
		free(cl);
		return NULL;
	}

	if (cl->count == 0) {
		cl->chunks = NULL;
		return cl;
	}

	if ((cl->chunks = malloc(cl->count * sizeof(cl->chunks[0]))) == NULL) {
		/* FIXME: need to read the data anyway to clear the socket */
		warn("unable to allocate change list chunks");
		free(cl);
		return NULL;
	}

	trace_on(printf("reading "U64FMT" chunk addresses (%u bits) from ddsnapd\n", cl->count, cl->chunksize_bits););
	if ((err = readpipe(serv_fd, cl->chunks, cl->count * sizeof(cl->chunks[0]))) < 0) {
		warn("unable to read change list chunks: %s", strerror(-err));
		free(cl->chunks);
		free(cl);
		return NULL;
	}

	return cl;
}

static int ddsnap_send_delta(int serv_fd, u32 src_snap, u32 tgt_snap, char const *devstem, u32 remsnap, u32 mode, int level, int ds_fd)
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
		warn("unable to send delta: %s", strerror(-err));
		return 1;
	}

	struct head head;

	trace_on(fprintf(stderr, "waiting for response\n"););

	if ((err = readpipe(ds_fd, &head, sizeof(head))) < 0) {
		warn("unable to read response from downstream: %s", strerror(-err));
		return 1;
	}

	trace_on(printf("reply = %x\n", head.code););

	if (head.code != SEND_DELTA_PROCEED) {
		if (head.code != SEND_DELTA_ERROR) {
			unknown_message_handler(ds_fd, &head);
			return 1;
		}
		error_message_handler(ds_fd, "downstream server reason why send delta failed",
				head.length);
		return 1;
	}

	trace_on(fprintf(stderr, "sending delta\n"););

	/* stream delta */

	char *dev1name;
	if (!(dev1name = malloc_snapshot_name(devstem, src_snap))) {
		warn("unable to allocate memory for dev1name");
		return 1;
	}

	char *dev2name;
	if (!(dev2name = malloc_snapshot_name(devstem, tgt_snap))) {
		warn("unable to allocate memory for dev2name");
		free(dev1name); 
		return 1;
	}

	if (generate_delta_extents(mode, level, cl, ds_fd, dev1name, dev2name, TRUE) < 0) {
		warn("could not send delta downstream for snapshot devices %s and %s", dev1name, dev2name);
		free(dev2name);
		free(dev1name);
		return 1;
	}

	free(dev2name);
	free(dev1name);

	trace_on(fprintf(stderr, "waiting for response\n"););

	if ((err = readpipe(ds_fd, &head, sizeof(head))) < 0) {
		warn("unable to read response from downstream: %s", strerror(-err));
		return 1;
	}

	trace_on(printf("reply = %x\n", head.code););

	if (head.code != SEND_DELTA_DONE) {
		
		if (head.code != SEND_DELTA_ERROR) {
			unknown_message_handler(ds_fd, &head);
			return 1;
		}
		error_message_handler(ds_fd, "downstream server reason why send delta failed",
				head.length);
		return 1;
	}

	/* success */

	trace_on(fprintf(stderr, "downstream server successfully applied delta to snapshot %u\n", remsnap););

	return 0;
}

static int apply_delta_extents(int deltafile, u32 chunk_size, u64 chunk_count, char const *dev1name, char const *dev2name, int progress, int vol_device)
{
	int snapdev1, snapdev2;
	int err;

	/* if an extent is being applied */
	if (!vol_device) {
		snapdev1 = open(dev1name, O_RDONLY);
		if (snapdev1 < 0) {
			err = -errno;
			goto apply_open1_error;
		}
		printf("src device: %s\n", dev1name);
	}

	snapdev2 = open(dev2name, O_WRONLY);
	if (snapdev2 < 0) {
		err = -errno;
		goto apply_open2_error;
	}

	printf("dst device: %s\n", dev2name);
	printf("chunk_count: "U64FMT"\n", chunk_count);

	struct delta_extent_header deh;
	u64 uncomp_size, extent_size;
	u64 extent_addr, chunk_num;

	unsigned char *updated     = malloc(MAX_MEM_SIZE);
	unsigned char *extent_data = malloc(MAX_MEM_SIZE);
	unsigned char *delta_data  = malloc(MAX_MEM_SIZE);
	unsigned char *comp_delta  = malloc(MAX_MEM_SIZE);
	char *up_extent1  = malloc(MAX_MEM_SIZE);
	char *up_extent2  = malloc(MAX_MEM_SIZE);

	if (!updated || !extent_data || !delta_data || !comp_delta || !up_extent1 || !up_extent2)
		goto apply_alloc_error;	

	for (chunk_num = 0; chunk_num < chunk_count;) {
		trace_off(printf("reading chunk "U64FMT" header\n", chunk_num););
		if ((err = fdread(deltafile, &deh, sizeof(deh))) < 0)
			goto apply_headerread_error;
		if (deh.magic_num != MAGIC_NUM)
			goto apply_magic_error;

		extent_size = deh.num_of_chunks * chunk_size;
		uncomp_size = extent_size;
		extent_addr = deh.extent_addr;
		
		if (!vol_device && ((err = diskread(snapdev1, extent_data, extent_size, extent_addr)) < 0))
			goto apply_devread_error;
		/* check to see if the checksum of snap0 is the same on upstream and downstream */
		if (!vol_device && deh.ext1_chksum != checksum((const unsigned char *)extent_data, extent_size)) 
			goto apply_checksum_error_snap0;

		trace_off(printf("extent data length is %llu (extent buffer is %llu)\n", deh.extents_delta_length, extent_size););

		/* gzip compression was used on extent */
		if (deh.gzip_on == TRUE) {
			if ((err = fdread(deltafile, comp_delta, deh.extents_delta_length)) < 0)
				goto apply_deltaread_error;
			trace_off(printf("data was compressed\n"););
			/* zlib decompression */
			int comp_ret = uncompress(delta_data, (unsigned long *) &uncomp_size, comp_delta, deh.extents_delta_length);
			if (comp_ret == Z_MEM_ERROR)
				goto apply_compmem_error;
			if (comp_ret == Z_BUF_ERROR)
				goto apply_compbuf_error;
			if (comp_ret == Z_DATA_ERROR)
				goto apply_compdata_error;		
		} else {
			if ((err = fdread(deltafile, delta_data, deh.extents_delta_length)) < 0)
				goto apply_deltaread_error;
			uncomp_size = deh.extents_delta_length;		
		}
		
		//if the mode is RAW or XDELTA RAW then copy the delta file directly to updated (no uncompression)
		if (deh.mode == RAW)
			memcpy(updated, delta_data, extent_size);
		if (!vol_device && deh.mode == XDELTA) {
			trace_off(printf("read %llx chunk delta extent data starting at chunk "U64FMT"/offset "U64FMT" from \"%s\"\n", deh.num_of_chunks, chunk_num, extent_addr, dev1name););
			int apply_ret = apply_delta_chunk(extent_data, updated, delta_data, extent_size, uncomp_size);
			trace_off(printf("apply_ret %d\n", apply_ret););
			if (apply_ret < 0)
				goto apply_chunk_error;
		}
		
		if (deh.ext2_chksum != checksum((const unsigned char *)updated, extent_size)) 
			goto apply_checksum_error;
		if ((err = diskwrite(snapdev2, updated, extent_size, extent_addr)) < 0)
			goto apply_write_error;
		chunk_num = chunk_num + deh.num_of_chunks;
		if (progress) {
			printf("\rApplied chunk "U64FMT"/"U64FMT" ("U64FMT"%%)", chunk_num, chunk_count, (chunk_num * 100) / chunk_count);
			fflush(stdout);
		}
	}
	free(up_extent1);
	free(up_extent2);
	free(comp_delta);
	free(extent_data);
	free(delta_data);
	free(updated);

	if (progress)
		printf("\n");

	close(snapdev2);
	if (!vol_device)
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

apply_checksum_error_snap0:
	err = -ERANGE;
	if (progress)
		printf("\n");
	warn("checksum failed for "U64FMT" chunk extent with start address of "U64FMT" snapshot0 is not the same on the upstream and the downstream", deh.num_of_chunks, extent_addr);
	goto apply_freeall_cleanup;


apply_checksum_error:
	err = -ERANGE;
	if (progress)
		printf("\n");
	warn("checksum failed for "U64FMT" chunk extent with start address of "U64FMT, deh.num_of_chunks, extent_addr);
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
		warn("not a proper delta file (too short)");
		return -1; /* FIXME: use named error */
	}

	/* Make sure it's a proper delta file */
	if (strncmp(dh.magic, DELTA_MAGIC_ID, MAGIC_SIZE) != 0) {
		warn("not a proper delta file (wrong magic in header)");
		return -1; /* FIXME: use named error */
	}

	if (dh.chunk_size == 0) {
		warn("not a proper delta file (zero chunk size)");
		return -1; /* FIXME: use named error */
	}

	char *dev1name;
	int err;

	/* check to see if the replication is full volume replication or replication via snapshot */
	if (dh.vol_device)
		dev1name = NULL;
	else {
		if (!(dev1name = malloc_snapshot_name(devstem, dh.src_snap))) {
			warn("unable to allocate memory for dev1name");
			return -ENOMEM;
		}
	}

	if ((err = apply_delta_extents(deltafile, dh.chunk_size, dh.chunk_num, dev1name, devstem, TRUE, dh.vol_device)) < 0) {
		if (!dh.vol_device)
			free(dev1name);
		return err;
	}

	if (!dh.vol_device)
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
		warn("could not apply delta file \"%s\" to origin device \"%s\"", deltaname, devstem);
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
		warn("unable to request snaphot list: %s", strerror(-err));
		return 1;
	}

	struct head head;

	if ((err = readpipe(serv_fd, &head, sizeof(head))) < 0) {
		warn("unable to read snaphot list head: %s", strerror(-err));
		return 1;
	}

	if (head.code != SNAPSHOT_LIST) {
		unknown_message_handler(serv_fd, &head);
		return 1;
	}

	if (head.length < sizeof(int32_t)) {
		warn("snapshot list reply length mismatch: expected >=%u, actual %u", sizeof(int32_t), head.length);
		return 1;
	}

	int count;

	if ((err = readpipe(serv_fd, &count, sizeof(int))) < 0) {
		warn("unable to read snaphot list count: %s", strerror(-err));
		return 1;
	}

	if (head.length != sizeof(int32_t) + count * sizeof(struct snapinfo)) {
		warn("snapshot list reply length mismatch: expected %u, actual %u", sizeof(int32_t) + count * sizeof(struct snapinfo), head.length);
		return 1;
	}

	struct snapinfo *buffer = malloc(count * sizeof(struct snapinfo));

	if (!buffer) {
		warn("unable to allocate snapshot list buffer");
		return 1;
	}

	if ((err = readpipe(serv_fd, buffer, count * sizeof(struct snapinfo))) < 0) {
		warn("unable to read snaphot list: %s", strerror(-err));
		free(buffer);
		return 1;
	}

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

	free(buffer);

	return 0;
}

static int ddsnap_generate_changelist(int serv_fd, char const *changelist_filename, u32 src_snap, u32 tgt_snap)
{
	int change_fd = open(changelist_filename, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);

	if (change_fd < 0) {
		warn("unable to open changelist file %s for writing: %s", changelist_filename, strerror(errno));
		return 1;
	}

	struct change_list *cl;

	if ((cl = stream_changelist(serv_fd, src_snap, tgt_snap)) == NULL) {
		warn("could not generate change list between snapshots %u and %u", src_snap, tgt_snap);
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
	int err;

	if ((err = outbead(sock, DELETE_SNAPSHOT, struct create_snapshot, snaptag)) < 0) { /* FIXME: why create_snapshot? */
		warn("unable to send delete snapshot message: %s", strerror(-err));
		return 1;
	}

	struct head head;
	if ((err = readpipe(sock, &head, sizeof(head))) < 0) {
		warn("unable to read delete snapshot message head: %s", strerror(-err));
		return 1;
	}

	trace_on(printf("snapshot delete reply = %x\n", head.code););

	if (head.code != DELETE_SNAPSHOT_OK) {
		if (head.code == DELETE_SNAPSHOT_ERROR)
			warn("snapshot server is unable to delete snapshot %u", snaptag);
		else
			unknown_message_handler(sock, &head);
		return 1;
	}
	return 0;
}

static int create_snapshot(int sock, u32 snaptag)
{
	int err;

	trace_on(printf("sending snapshot create request %u\n", snaptag););

	if ((err = outbead(sock, CREATE_SNAPSHOT, struct create_snapshot, snaptag)) < 0) {
		warn("unable to send create snapshot message: %s", strerror(-err));
		return 1;
	}

	struct head head;
	if ((err = readpipe(sock, &head, sizeof(head))) < 0) {
		warn("unable to read create snapshot message head: %s", strerror(-err));
		return 1;
	}

	trace_on(printf("snapshot create reply = %x\n", head.code););

	if (head.code != CREATE_SNAPSHOT_OK) {
		if (head.code == CREATE_SNAPSHOT_ERROR)
			warn("snapshot server is unable to create snapshot %u", snaptag);
		else 
			unknown_message_handler(sock, &head);
		return 1;
	}
	return 0;
}

static int set_priority(int sock, u32 snaptag, int8_t prio_val)
{
	int err;

	if ((err = outbead(sock, PRIORITY, struct priority_info, snaptag, prio_val)) < 0) {
		warn("unable to send set priority message: %s", strerror(-err));
		return 1;
	}

	struct head head;
	if ((err = readpipe(sock, &head, sizeof(head))) < 0) {
		warn("unable to read priority message head: %s", strerror(-err));
		return 1;
	}
	
	trace_on(printf("reply = %x\n", head.code););
	if (head.code != PRIORITY_OK) {
		if (head.code != PRIORITY_ERROR) {
			unknown_message_handler(sock, &head);
			return 1;
		}
		struct priority_error *prio_err = 
			(struct priority_error *)get_message(sock, head.length);
		warn("snapshot server is unable to set priority for snapshot %u", snaptag);
		if (!prio_err || head.length == sizeof(struct priority_error))
			return 1;
		warn("server reason for priority failure: %s", prio_err->msg);
		free(prio_err);
		return 1;
	}

	return 0;
}

static int usecount(int sock, u32 snaptag, int32_t usecnt_dev)
{
	int err;

	if ((err = outbead(sock, USECOUNT, struct usecount_info, snaptag, usecnt_dev)) < 0) {
		warn("unable to send usecount message: %s", strerror(-err));
		return 1;
	}

	struct head head;
	unsigned maxbuf = 500;
	char buf[maxbuf];

	/* read message header */
	if ((err = readpipe(sock, &head, sizeof(head))) < 0) {
		warn("unable to read usecount message head: %s", strerror(-err));
		return 1;
	}

	trace_on(printf("reply = %x\n", head.code););

	/* check for error before reading message body */
	if (head.code != USECOUNT_OK) {
		if (head.code != USECOUNT_ERROR) {
			unknown_message_handler(sock, &head);
			return 1;
		}	
		struct usecount_error *usecnt_err = 
			(struct usecount_error *)get_message(sock, head.length);
		warn("snapshot server is unable to set usecount for snapshot %u", snaptag);
		if (!usecnt_err || head.length == sizeof(struct usecount_error))
			return 1;
		warn("server reason for usecount failure: %s", usecnt_err->msg);
		free(usecnt_err);
		return 1;
	}

	assert(head.length < maxbuf); // !!! don't die

	/* read message body */
	if ((err = readpipe(sock, buf, head.length)) < 0) {
		warn("unable to read usecount message body: %s", strerror(-err));
		return 1;
	}
	printf("New usecount: %u\n", (unsigned int)((struct usecount_ok *)buf)->usecount);
 
	return 0; // should we return usecount here too?
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
		char err_msg[MAX_ERRMSG_SIZE];
		err_msg[0] = '\0';

		if ((err = readpipe(csock, &message.head, sizeof(message.head))) < 0) {
			snprintf(err_msg, MAX_ERRMSG_SIZE, "error reading upstream message header: %s", strerror(-err));
			err_msg[MAX_ERRMSG_SIZE-1] = '\0';
			goto end_connection;
		}
		if (message.head.length > maxbody) {
			snprintf(err_msg, MAX_ERRMSG_SIZE, "message body too long %u", message.head.length);
			err_msg[MAX_ERRMSG_SIZE-1] = '\0';
			goto end_connection;
		}
		if ((err = readpipe(csock, &message.body, message.head.length)) < 0) {
			snprintf(err_msg, MAX_ERRMSG_SIZE, "error reading upstream message body: %s", strerror(-err)); 
			err_msg[MAX_ERRMSG_SIZE-1] = '\0';
			goto end_connection;
		}

		struct send_delta body;

		switch (message.head.code) {
		case SEND_DELTA:
			if (message.head.length < sizeof(body)) {
				snprintf(err_msg, MAX_ERRMSG_SIZE, "incomplete SEND_DELTA request sent by client");
				err_msg[MAX_ERRMSG_SIZE-1] = '\0';
				goto end_connection;
			}

			memcpy(&body, message.body, sizeof(body));

			if (body.snap == (u32)~0UL) {
				snprintf(err_msg, MAX_ERRMSG_SIZE, "invalid snapshot %u in SEND_DELTA", body.snap);
				err_msg[MAX_ERRMSG_SIZE-1] = '\0';
				goto end_connection;
			}

			if (body.chunk_size == 0) {
				snprintf(err_msg, MAX_ERRMSG_SIZE, "invalid chunk size %u in SEND_DELTA", body.chunk_size);
				err_msg[MAX_ERRMSG_SIZE-1] = '\0';
				goto end_connection;
			}

			char *snapdev;

			if (!(snapdev = malloc_snapshot_name(devstem, body.snap))) {
				snprintf(err_msg, MAX_ERRMSG_SIZE, "unable to allocate device name");
				err_msg[MAX_ERRMSG_SIZE-1] = '\0';
				goto end_connection;
			}

			/* FIXME: verify snapshot exists */

			/* FIXME: In the future we should also lookup the client's address in a
			 * device permission table and check for replicatiosn already in progress.
			 */

			if (outbead(csock, SEND_DELTA_PROCEED, struct {}) < 0) {
				snprintf(err_msg, MAX_ERRMSG_SIZE, "unable to send delta proceed message to server");
				err_msg[MAX_ERRMSG_SIZE-1] = '\0';
				goto end_connection;
			}

			/* retrieve it */

			if (apply_delta_extents(csock, body.chunk_size, 
						body.chunk_count, snapdev, origindev, TRUE, FALSE) < 0) {
				snprintf(err_msg, MAX_ERRMSG_SIZE, "unable to apply upstream delta to device \"%s\"", origindev);
				err_msg[MAX_ERRMSG_SIZE-1] = '\0';
				goto end_connection;
			}

			free(snapdev);

			/* success */

			if (outbead(csock, SEND_DELTA_DONE, struct {}) < 0) {
				snprintf(err_msg, MAX_ERRMSG_SIZE, "unable to send delta complete message to server");
				err_msg[MAX_ERRMSG_SIZE-1] = '\0';
				goto end_connection;
			}
			trace_on(fprintf(stderr, "applied streamed delta to \"%s\", closing connection\n", origindev););
			close(csock);
			exit(0);

		default:
			snprintf(err_msg, MAX_ERRMSG_SIZE, 
					"unexpected message type sent to snapshot replication server %x", message.head.code); 
			err_msg[MAX_ERRMSG_SIZE-1] = '\0';
			goto end_connection;
		}

	end_connection:
		warn("closing connection on error: %s", err_msg);
		if (outhead(csock, SEND_DELTA_ERROR, strlen(err_msg)+1) < 0 ||
				writepipe(csock, err_msg, strlen(err_msg)+1) < 0)
			warn("unable to send delta error message to upstream server");
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
		warn("unable to send request for origin sector count: %s", strerror(-err));
		return 0;
	}

	struct head head;

	if ((err = readpipe(serv_fd, &head, sizeof(head))) < 0) {
		warn("unable to send request for origin sector count: %s", strerror(-err));
		return 0;
	}

	if (head.code != ORIGIN_SECTORS) {
		unknown_message_handler(serv_fd, &head);
		return 1;
	}

	struct origin_sectors body;

	if (head.length != sizeof(body)) {
		warn("origin sector message length mismatch: expected %u, actual %u", sizeof(body), head.length);
		return 1;
	}

	if ((err = readpipe(serv_fd, &body, sizeof(body))) < 0) {
		warn("unable to read origin sector message body: %s", strerror(-err));
		return 1;
	}

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
	struct status_message *reply;
	reply = generate_status(serv_fd, snaptag);

	if (!reply)
		return 1;

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

		if (!(column_totals = malloc(sizeof(u64) * reply->num_columns))) {
			warn("unable to allocate array for column totals");
			free(reply);
			return 1;
		}

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
	       "	vol               \n"
               "        usage: ddsnap vol [-?|--help|--usage] <subcommand>\n"
               "\n"
               "        Available vol subcommands:\n"
 	       "	create            Create a volume delta file\n"
	       "	apply             Apply a volume file to a downstream volume\n"
	       "	send              Send a volume file to a downstream server\n"
	       "        listen            Listen for a volume file arriving from upstream\n"
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

static void volUsage(void)
{
	printf("usage: ddsnap vol [-?|--help|--usage] <subcommand>\n"
	       "\n"
               "Available vol subcommands:\n"
 	       "	create            Create a volume delta file\n"
	       "	apply             Apply a volume file to a downstream volume\n"
	       "	send              Send a volume file to a downstream server\n"
	       "        listen            Listen for a volume file arriving from upstream\n");
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

int same_BLK_device(char const *dev1,char const *dev2) {
	struct stat dev1buf, dev2buf;
	
	if (stat(dev1, &dev1buf) < 0) { 
		error("stat error on %s", dev1);
		return -1;
	}

	if (stat(dev2, &dev2buf) < 0) {
		error("stat error on %s", dev2);				
		return -1;
	}
	
	if (!S_ISBLK(dev1buf.st_mode)) {
		fprintf(stderr, "the device %s must be a block device\n", dev1);
		return -1;
	}

	if (!S_ISBLK(dev2buf.st_mode)) {
		fprintf(stderr, "the device %s must be a block device\n", dev2);
		return -1;			
	}
	
	int dev1_rmajor = major(dev1buf.st_rdev);
	int dev1_rminor = minor(dev1buf.st_rdev);
	int dev2_rmajor = major(dev2buf.st_rdev);
	int dev2_rminor = minor(dev2buf.st_rdev);
	
	trace_off(printf("dev1_rmajor is %d, dev1_rminor is %d, dev2_rmajor is %d, dev2_rminor is %d.\n", dev1_rmajor, dev1_rminor, dev2_rmajor, dev2_rminor););

	if (dev1_rmajor == dev2_rmajor && dev1_rminor == dev2_rminor) {
		fprintf(stderr, "the devices cannot be the same: %s, %s\n", dev1, dev2);
		return -2;
	}

	return 0;
}

int main(int argc, char *argv[])
{
	char const *command;

	struct poptOption noOptions[] = {
		POPT_TABLEEND
	};

	char *js_str = NULL, *bs_str = NULL, *cs_str = NULL;
	int yes = FALSE;
	struct poptOption initOptions[] = {
		{ "yes", 'y', POPT_ARG_NONE, &yes, 0, "Answer yes to all prompts", NULL},
		{ "journal_size", 'j', POPT_ARG_STRING, &js_str, 0, "User specified journal size, i.e. 400k (default: 100 * chunk_size)", "desired journal size" },
		{ "block_size", 'b', POPT_ARG_STRING, &bs_str, 0, "User specified block size, has to be a power of two, i.e. 8k (default: 4k)", "desired block size" },
		{ "chunk_size", 'c', POPT_ARG_STRING, &cs_str, 0, "User specified chunk size, has to be a power of two, i.e. 8k (default: 4k)", "desired chunk size" },
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

	int xd = FALSE, raw = FALSE, test = FALSE, gzip_level = DEF_GZIP_COMP, best_comp = FALSE;
	struct poptOption cdOptions[] = {
		{ "xdelta", 'x', POPT_ARG_NONE, &xd, 0, "Delta file format: xdelta chunk", NULL },
		{ "raw", 'r', POPT_ARG_NONE, &raw, 0, "Delta file format: raw chunk from later snapshot", NULL },
		{ "best", 'b', POPT_ARG_NONE, &best_comp, 0, "Delta file format: best compression (slowest)", NULL},
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

	struct poptOption volOptions[] = {
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Create volume file\n\t Function: Create a volume file\n\t Usage: vol create <sockname> <volfile> <vol_device>\n", NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Apply volume file\n\t Function: Apply a volume file to a downstream volume\n\t Usage: vol apply <volfile> <vol_device>", NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Send vol\n\t Function: Send a volume file to a downstream server\n\t Usage: vol send <sockname> <vol_device> <host>[:<port>]\n", NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &serverOptions, 0,
		  "Listen\n\t Function: Listen for a volume file arriving from upstream\n\t Usage: vol listen [OPTION...] <vol_device> [<host>[:<port>]]", NULL },
		POPT_TABLEEND
	};

	struct poptOption deltaOptions[] = {
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Create changelist\n\t Function: Create a changelist given 2 snapshots\n\t Usage: delta changelist <sockname> <changelist> <snapshot1> <snapshot2>", NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &cdOptions, 0,
		  "Create delta\n\t Function: Create a delta file given a changelist and 2 snapshots\n\t Usage: delta create [OPTION...] <changelist> <deltafile> <devstem>\n", NULL },
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
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &volOptions, 0,
		  "Vol\n\t Usage: vol [OPTION...] <subcommand> ", NULL},
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
		u32 bs_bits = DEF_CHUNK_SIZE_BITS;
		u32 cs_bits = DEF_CHUNK_SIZE_BITS;
		u32 js_bytes = DEFAULT_JOURNAL_SIZE;

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

		trace_off(printf("snapdev is %s, origdev is %s, metadev is %s.\n", snapdev, origdev, metadev););

		if (same_BLK_device(snapdev, origdev) < 0) {
			poptPrintUsage(initCon, stderr, 0);
			poptFreeContext(initCon);
			return 1;			
		}

		if (bs_str && cs_str && (strcmp(bs_str, cs_str) != 0)) {

			if (!metadev) {
				fprintf(stderr, "%s: metadata device must be specified\n", command);
				poptPrintUsage(initCon, stderr, 0);
				poptFreeContext(initCon);
				return 1;
			}

			if (same_BLK_device(snapdev, metadev) < 0) {
				poptPrintUsage(initCon, stderr, 0);
				poptFreeContext(initCon);
				return 1;			
			}			

			if (same_BLK_device(origdev, metadev) < 0) {
				poptPrintUsage(initCon, stderr, 0);
				poptFreeContext(initCon);
				return 1;			
			}
		}
		
		if (poptPeekArg(initCon) != NULL) {
			fprintf(stderr, "%s: too many arguments\n", command);
			poptPrintUsage(initCon, stderr, 0);
			poptFreeContext(initCon);
			return 1;
		}

		int error = 0;
		struct superblock *sb;
		if ((error = posix_memalign((void **)&sb, 1 << SECTOR_BITS, SB_SIZE))) {
			warn("Error: %s unable to allocate memory for superblock", strerror(error));
			return 1;
		}
		memset(sb, 0, SB_SIZE);
		
		
		if ((sb->snapdev = open(snapdev, O_RDWR | O_DIRECT)) == -1)
			error("Could not open snapshot store %s: %s", snapdev, strerror(errno));
		
		if ((sb->orgdev = open(origdev, O_RDONLY | O_DIRECT)) == -1)
			error("Could not open origin volume %s: %s", origdev, strerror(errno));
		
		sb->metadev = sb->snapdev;

		if (metadev && (sb->metadev = open(metadev, O_RDWR | O_DIRECT)) == -1) 
			error("Could not open meta volume %s: %s", metadev, strerror(errno));
		
		if (!yes) {
			struct superblock *temp_sb;
			if ((error = posix_memalign((void **)&temp_sb, 1 << SECTOR_BITS, SB_SIZE))) {
				warn("Error: %s unable to allocate temporary superblock", strerror(error));
				return 1;
			}
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
		
		trace_on(printf("js_bytes was %u, bs_bits was %u and cs_bits was %u\n", js_bytes, bs_bits, cs_bits););

		if (bs_str != NULL) {
			bs_bits = strtobits(bs_str);
			if (bs_bits == INPUT_ERROR) {
				poptPrintUsage(initCon, stderr, 0);
				fprintf(stderr, "Invalid block size input. Try 64k\n");
				exit(1);
			}
			if (!cs_str)
				cs_bits = bs_bits;
		}

		if (cs_str != NULL) {
			cs_bits = strtobits(cs_str);
			if (cs_bits == INPUT_ERROR) {
				poptPrintUsage(initCon, stderr, 0);
				fprintf(stderr, "Invalid chunk size input. Try 64k\n");
				exit(1);
			}
			if (!bs_str)
				bs_bits = cs_bits;
		}

		if (js_str != NULL) {
			js_bytes = strtobytes(js_str);
			if (js_bytes == INPUT_ERROR) {
				poptPrintUsage(initCon, stderr, 0);
				fprintf(stderr, "Invalid journal size input. Try 400k\n");
				exit(1);
			}
		}

		trace_off(printf("js_bytes is %u, bs_bits is %u, and cs_bits is %u\n", js_bytes, bs_bits, cs_bits););

# ifdef MKDDSNAP_TEST
		if (init_snapstore(sb, js_bytes, bs_bits, cs_bits) < 0)
			return 1;
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
		
		unsigned bufsize = 1 << bs_bits;
		init_buffers(bufsize, (1 << 25)); /* preallocate 32Mb of buffers */
		
		if (init_snapstore(sb, js_bytes, bs_bits, cs_bits) < 0) 
			warn("Snapshot storage initiailization failed");
		free(sb);
		return 0;
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
	
		int error = 0;	
		struct superblock *sb;
		if ((error = posix_memalign((void **)&sb, 1 << SECTOR_BITS, SB_SIZE))) {
			warn("Error: %s unable to allocate memory for superblock", strerror(error));
			return 1;
		}
		memset(sb, 0, SB_SIZE);

		if ((sb->snapdev = open(snapdev, O_RDWR | O_DIRECT)) == -1)
			error("Could not open snapshot store %s: %s", snapdev, strerror(errno));
		
		if ((sb->orgdev = open(origdev, O_RDONLY | O_DIRECT)) == -1)
			error("Could not open origin volume %s: %s", origdev, strerror(errno));
		
		sb->metadev = sb->snapdev;
		if (metadev && (sb->metadev = open(metadev, O_RDWR | O_DIRECT)) == -1) 
			error("Could not open meta volume %s: %s", metadev, strerror(errno));

		if (diskread(sb->metadev, &sb->image, SB_SIZE, SB_SECTOR << SECTOR_BITS) < 0)
			warn("Unable to read superblock: %s", strerror(errno));

		poptFreeContext(serverCon);
		
		int listenfd, getsigfd, agentfd, ret;
		
		unsigned bufsize = 1 << METADATA_ALLOC(sb).allocsize_bits;	
		init_buffers(bufsize, (1 << 27)); /* preallocate 128Mb of buffers */
		
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

		/* should only return on an error */
		if ((ret = snap_server(sb, listenfd, getsigfd, agentfd)) < 0)
			warn("server exited with error %i", ret);
	
		return 0;
	}
	if (strcmp(command, "create") == 0) {
		if (argc != 4) {
			printf("Usage: %s create <sockname> <snapshot>\n", argv[0]);
			return 1;
		}

		u32 snaptag;

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

			int sock = create_socket(sockname);

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
	if (strcmp(command, "vol") == 0) {
		poptContext volCon;
		struct poptOption options[] = {
			{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &volOptions, 0, NULL, NULL },
			POPT_AUTOHELP
			POPT_TABLEEND
		};
		volCon = poptGetContext(NULL, argc-1, (const char **)&(argv[1]), options, 0);

		if (argc < 3) {
			poptPrintHelp(volCon, stdout, 0);
			poptFreeContext(volCon);
			exit(1);
		}
		
		char const *subcommand = argv[2];
		
		if (strcmp(subcommand, "--help") == 0 || strcmp(subcommand, "-?") == 0) {
			poptPrintHelp(volCon, stdout, 0);
			poptFreeContext(volCon);
			exit(0);
		}
		
		poptFreeContext(volCon);
		
		if (strcmp(subcommand, "--usage") == 0) {
			volUsage();
			exit(0);
		}

		if (strcmp(subcommand, "create") == 0) {
			if (argc != 6) {
				printf("usage: %s %s create <sockname> <vol_file> <vol_dev>\n", argv[0], argv[1]);
				return 1;
			}

			int sock = create_socket(argv[3]);

			int ret = ddsnap_generate_vol(sock, argv[4], argv[5]);
			close(sock);
			return ret;
		}
		if (strcmp(subcommand, "apply") == 0) {
			if (argc != 5) {
				printf("usage: %s %s apply <vol_file> <vol_dev>\n", argv[0], argv[1]);
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
			poptSetOtherOptionHelp(cdCon, "<sockname> <vol_dev> <host>[:<port>]");

			char c;

			while ((c = poptGetNextOpt(cdCon)) >= 0);
			if (c < -1) {
				fprintf(stderr, "%s: %s: %s\n", argv[0], poptBadOption(cdCon, POPT_BADOPTION_NOALIAS), poptStrerror(c));
				poptFreeContext(cdCon);
				return 1;
			}

			char const *sockname, *devstem, *hoststr;

			sockname      = poptGetArg(cdCon);
			devstem       = poptGetArg(cdCon);
			hoststr       = poptGetArg(cdCon);

			if (hoststr == NULL)
				cdUsage(cdCon, 1, argv[0], "Not enough arguments to vol send\n");
			if (poptPeekArg(cdCon) != NULL)
				cdUsage(cdCon, 1, argv[0], "Too many arguments to vol send\n");

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

			u32 snaptag1, snaptag2, remsnaptag;
			snaptag1 = snaptag2 = remsnaptag = 0;

			int ret = ddsnap_send_delta(sock, snaptag1, snaptag2, devstem, remsnaptag, RAW, 9, ds_fd);
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
			if (xd+raw+test+best_comp > 1) {
				fprintf(stderr, "%s %s: Too many chunk options were selected.\nPlease select only one: -x, -r, -t or -b\n", argv[0], argv[1]);
				poptPrintUsage(cdCon, stderr, 0);
				poptFreeContext(cdCon);
				return 1;
			}

			u32 mode = (test ? TEST : (raw ? RAW : (xd? XDELTA : BEST_COMP)));
			if (best_comp)
				gzip_level = MAX_GZIP_COMP;
		
			trace_on(fprintf(stderr, "xd=%d raw=%d test=%d best_comp=%d mode=%u gzip_level=%d\n", xd, raw, test, best_comp, mode, gzip_level););

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
			poptSetOtherOptionHelp(cdCon, "<sockname> <snapshot1> <snapshot2> <devstem> <remsnapshot> <host>[:<port>]");

			char c;

			while ((c = poptGetNextOpt(cdCon)) >= 0);
			if (c < -1) {
				fprintf(stderr, "%s: %s: %s\n", argv[0], poptBadOption(cdCon, POPT_BADOPTION_NOALIAS), poptStrerror(c));
				poptFreeContext(cdCon);
				return 1;
			}

			/* Make sure the options are mutually exclusive */
			if (xd+raw+test+best_comp > 1) {
				fprintf(stderr, "%s %s: Too many chunk options were selected.\nPlease select only one: -x, -r, -t or -o\n", argv[0], argv[1]);
				poptPrintUsage(cdCon, stderr, 0);
				poptFreeContext(cdCon);
				return 1;
			}

			u32 mode = (test ? TEST : (raw ? RAW : (xd ? XDELTA : BEST_COMP)));
			if (best_comp)
				gzip_level = MAX_GZIP_COMP;

			trace_on(fprintf(stderr, "xd=%d raw=%d test=%d best_comp=%d mode=%u gzip_level=%d\n", xd, raw, test, best_comp, mode, gzip_level););

			char const *sockname, *snaptag1str, *snaptag2str, *devstem, *snaptagremstr, *hoststr;

			sockname      = poptGetArg(cdCon);
			snaptag1str   = poptGetArg(cdCon);
			snaptag2str   = poptGetArg(cdCon);
			devstem       = poptGetArg(cdCon);
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

			int ret = ddsnap_send_delta(sock, snaptag1, snaptag2, devstem, remsnaptag, mode, gzip_level, ds_fd);
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
