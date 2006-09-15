#define _XOPEN_SOURCE 500 /* pread */

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
	int compress;
};

static int eek(void) {
	error("%s (%i)", strerror(errno), errno);
	return 1;
}

static u64 checksum(const unsigned char *data, u32 data_length) {
	u64 result = 0;
	int i;

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

int generate_delta(int mode, int level, int clfile, int deltafile, char const *dev1name, char const *dev2name) {
	int snapdev1, snapdev2, err, chunk_num = 0, ret = 0;
	unsigned char *chunk_data1, *chunk_data2, *delta_data, *comp_delta;
	u32 chunk_size, chunk_size_bits, delta_size;
	u64 chunkaddr;
	unsigned long comp_size;
	struct cl_header cl = { };
	struct delta_header dh = { };
	struct delta_chunk_header dch = { .magic_num = MAGIC_NUM };
	
	strncpy(dh.magic, DELTA_MAGIC_ID, MAGIC_SIZE);
	dh.mode = mode;
	printf("mode: %d\n", mode);
	printf("level: %d\n", level);
	
	snapdev1 = open(dev1name, O_RDONLY);
	if (snapdev1 < 0) {
		err = -errno;
		printf("Could not open snapdev file \"%s\" for reading: %s\n", dev1name, strerror(errno));
		return err;
	}

	snapdev2 = open(dev2name, O_RDONLY);
	if (snapdev2 < 0) {
		err = -errno;
		printf("Could not open snapdev file \"%s\" for reading: %s\n", dev2name, strerror(errno));
		return err;
	}
	
	/* Make sure it's a proper changelist */
	if (read(clfile, &cl, sizeof(struct cl_header)) != sizeof(struct cl_header)) {
		printf("Not a proper changelist file (too short).\n");
		close(snapdev1);
		close(snapdev2);
		return -1; /* FIXME: use named error */
	}
	printf("header is %s\n", cl.magic);
	if (strcmp(cl.magic, CHANGELIST_MAGIC_ID) != 0) {
		printf("Not a proper changelist file (wrong magic in header).\n");
		close(snapdev1);
		close(snapdev2);
		return -1; /* FIXME: use named error */
	}
	
	/* Variable set up */
	read(clfile, &chunk_size_bits, sizeof(u32));
	chunk_size = 1 << chunk_size_bits;
	comp_size = chunk_size*2;
	chunk_data1 = (unsigned char *)malloc (chunk_size);
	chunk_data2 = (unsigned char *)malloc (chunk_size);
	delta_data  = (unsigned char *)malloc (chunk_size);
	comp_delta  = (unsigned char *)malloc (comp_size);

	printf("chunksize bit: %u\t", chunk_size_bits);
	printf("chunksize: %u\n", chunk_size);
	printf("comp_size: %lu\n", comp_size);

	/* Delta header set-up */
	write(deltafile, &dh, sizeof(struct delta_header));
	
	/* Chunk address followed by CHUNK_SIZE bytes of chunk data */
	while (read(clfile, &chunkaddr, sizeof(u64)) == sizeof(u64) && chunkaddr != -1) {
		chunk_num = chunk_num + 1;
		printf(".");
		chunkaddr = chunkaddr << chunk_size_bits;
		
		/* read in and generate the necessary chunk information */
		if (diskio(snapdev1, chunk_data1, chunk_size, chunkaddr, 0) < 0) {
			printf("chunk_data1 for chunkaddr %Lu not read properly from snapdev1.\n", chunkaddr);
			return -1;
		}
		if (diskio(snapdev2, chunk_data2, chunk_size, chunkaddr, 0) < 0) {
			printf("chunk_data2 for chunkaddr %Lu not read properly from snapdev2.\n", chunkaddr);
			return -1;
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
				printf("Delta for chunk address %Lu was not generated properly. \n", chunkaddr);
				close(snapdev1);
				close(snapdev2);
				return -1;
			}
			if (mode == TEST && ret != BUFFER_SIZE_ERROR && ret > 0) {
				/* sanity test for delta creation */
				unsigned char *delta_test = (unsigned char *)malloc (chunk_size);
				ret = apply_delta_chunk(chunk_data1, delta_test, delta_data, chunk_size, delta_size);
				
				if (ret != chunk_size) {
					free(delta_test);
					printf("Unable to create delta. \n");
					close(snapdev1);
					close(snapdev2);
					return -1;
				}
				
				if (checksum((const unsigned char *)delta_test, chunk_size) 
				    != checksum((const unsigned char *)chunk_data2, chunk_size))
					printf("checksum of delta_test does not match check_sum of chunk_data2");
				
				if (memcmp(delta_test, chunk_data2, chunk_size) != 0) {
					free(delta_test);
					printf("Generated delta does not match chunk on disk. \n");
					close(snapdev1);
					close(snapdev2);
					return -1;
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
			close(snapdev1);
			close(snapdev2);
			return -1;
		}
		if (comp_ret == Z_BUF_ERROR) {
			printf("Not enough room in the output buffer for compression.\n");
			close(snapdev1);
			close(snapdev2);
			return -1;
		}
		if (comp_ret == Z_STREAM_ERROR) {
			printf("Parameter is invalid: level=%d delta_size=%d\n", level, delta_size);
			close(snapdev1);
			close(snapdev2);
			return -1;
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
		if (write(deltafile, &dch, sizeof(struct delta_chunk_header)) != sizeof(struct delta_chunk_header)) {
			printf("delta_chunk_header was not written properly to deltafile. \n");
			return -1;
		}
		
		if (dch.compress == TRUE) {
			if (write(deltafile, comp_delta, comp_size) != comp_size) {
				printf("comp_delta was not written properly to deltafile. \n");
				return -1;
			}
		} else {
			if (write(deltafile, delta_data, delta_size) != delta_size) {
				printf("delta_data was not written properly to deltafile. \n");
				return -1;
			}
		}
		
                if (mode == TEST) {
			write(deltafile, chunk_data1, chunk_size);
			write(deltafile, chunk_data2, chunk_size);
                }
	}
	printf("\n");
	
	/* Make sure everything in changelist was properly transmitted */
	if (chunkaddr != -1) {
		printf("Changelist was not fully transmitted. \n");
		close(snapdev1);
		close(snapdev2);
		return -1; /* FIXME: use named error */
	}
	
	/* Updating deltafile header */
	dh.chunk_num = chunk_num;
	dh.chunk_size = chunk_size;
	if (diskio(deltafile, &dh, sizeof(struct delta_header), 0, 1) < 0) {
		printf("delta_header was not written properly to deltafile. \n");
		return -1; /* FIXME: use named error */
	}
	
	close(snapdev1);
	close(snapdev2);
	
	return 0;
}

int ddsnap_generate_delta(int mode, int level, char const *changelistname, char const *deltaname, char const *dev1name, char const *dev2name) {
	int clfile, deltafile;
	
	clfile = open(changelistname, O_RDONLY);
	if (clfile<0) {
		printf("Could not open changelist file \"%s\" for reading.\n", changelistname);
		return 1;
	}
	
	deltafile = open(deltaname, O_CREAT|O_WRONLY|O_TRUNC, S_IRWXU);
	if (deltafile<0) {
		printf("Could not create delta file \"%s\"\n", deltaname);
		return 1;
	}
	
	if (generate_delta(mode, level, clfile, deltafile, dev1name, dev2name) < 0) {
		close(deltafile);
		close(clfile);
		return 1;
	}
	
	close(deltafile);
	close(clfile);
	
	return 0;
}

int apply_delta(int deltafile, char const *devname) {
	int snapdev;
	u64 chunkaddr;
	unsigned long uncomp_size;
	unsigned char *chunk_data, *delta_data, *updated, *comp_delta;
	int err, check_chunk_num = 0, chunk_num = 0, chunk_size = 0, ret = 0;
	struct delta_header dh = { };
	struct delta_chunk_header dch = { };
	
        int c1_chk_sum = 0;
        char *up_chunk1, *up_chunk2;
	
	snapdev = open(devname, O_RDWR); /* FIXME: why not O_WRONLY? */
	if (snapdev < 0) {
		err = -errno;
		printf("Could not open snapdev file \"%s\" for writing.\n", devname);
		return err;
	}
	
	/* Make sure it's a proper delta file */
	if (read(deltafile, &dh, sizeof(struct delta_header)) != sizeof(struct delta_header)) {
		printf("Not a proper delta file (too short).\n");
		close(snapdev);
		return -1; /* FIXME: use named error */
	}
	if (strcmp(dh.magic, DELTA_MAGIC_ID) != 0) {
		printf("Not a proper delta file (wrong magic in header).\n");
		close(snapdev);
		return -1; /* FIXME: use named error */
	}
	
	check_chunk_num = dh.chunk_num;
	chunk_size = dh.chunk_size;
	chunk_data = (unsigned char *)malloc (chunk_size);
	delta_data = (unsigned char *)malloc (chunk_size*2);
	updated    = (unsigned char *)malloc (chunk_size);
	comp_delta = (unsigned char *)malloc (chunk_size);

        up_chunk1  = (char *)malloc (chunk_size);
        up_chunk2  = (char *)malloc (chunk_size);
	
        printf("Mode is %d\n", dh.mode);
	
	while (read(deltafile, &dch, sizeof(struct delta_chunk_header)) == sizeof(struct delta_chunk_header)) {
		if (dch.magic_num != MAGIC_NUM) {
			printf("Not a proper delta file (magic_num doesn't match). \n");
			close(snapdev);
			return -1;
		}

		if (dch.compress == TRUE) {
			if (read(deltafile, comp_delta, dch.data_length) != dch.data_length) {
				printf("Could not properly read comp_delta from deltafile. \n");
				close(snapdev);
				return -1;
			}
		} else {
			if (read(deltafile, delta_data, dch.data_length) != dch.data_length) {
				printf("Could not properly read delta_data from deltafile. \n");
				close(snapdev);
				return -1;
			}
		}

		chunkaddr = dch.chunk_addr;
		if (diskio(snapdev, chunk_data, chunk_size, chunkaddr, 0) < 0) {
			printf("Snapdev reading of downstream chunk1 has failed. \n");
			close(snapdev);
			return -1;
		}
		printf("Updating chunkaddr: %Lu\n", chunkaddr);

		if (dch.compress == TRUE) {

			printf("data was compressed \n");

			/* zlib decompression */
			uncomp_size = chunk_size*2;
			int comp_ret = uncompress(delta_data, &uncomp_size, comp_delta, dch.data_length);
			if (comp_ret == Z_MEM_ERROR) {
				printf("Not enough buffer memory for decompression. \n");
				close(snapdev);
				return -1;
			}
			if (comp_ret == Z_BUF_ERROR) {
				printf("Not enough room in the output buffer for decompression. \n");
				close(snapdev);
				return -1;
			}
			if (comp_ret == Z_DATA_ERROR) {
				printf("The input data was corrupted for decompression. \n");
				close(snapdev);
				return -1;
			}
		} else 
			uncomp_size = dch.data_length;

                if (dh.mode == RAW) {
			memcpy(updated, delta_data, chunk_size);
		} else {
			if (uncomp_size == chunk_size)
				memcpy(updated, delta_data, chunk_size);
			else
				ret = apply_delta_chunk(chunk_data, updated, delta_data, chunk_size, uncomp_size);
			
			printf("uncomp_size %lu & dch.data_length %u \n", uncomp_size, dch.data_length);
			printf("ret %d \n", ret);
			
			if (ret < 0) {
				printf("Delta for chunk address %Lu was not applied properly.\n", chunkaddr);
				close(snapdev);
				return -1;
			}
			if (dh.mode == TEST) {
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
				printf("Check_sum failed for chunk address %Lu \n", chunkaddr);
				if (dh.mode == TEST) {
					/* sanity check: does the checksum of upstream chunk1 = checksum of downstream chunk1? */
					if (c1_chk_sum != checksum((const unsigned char *)up_chunk1, chunk_size)) {
						printf("check_sum of chunk1 doesn't match for address %Lu \n", chunkaddr);
						if (dch.data_length == chunk_size)
							memcpy(updated, delta_data, chunk_size);
						else
							ret = apply_delta_chunk(up_chunk1, updated, delta_data, chunk_size, dch.data_length);
						
						if (ret < 0)
							printf("Delta for chunk address %Lu with upstream chunk1 was not applied properly.\n", chunkaddr);
						
						if (dch.check_sum != checksum((const unsigned char *)updated, chunk_size)) {
							printf("Check_sum of apply delta onto upstream chunk1 failed for chunk address %Lu \n", chunkaddr);
							memcpy(updated, up_chunk2, chunk_size);
						}
					} else {
						printf("apply delta doesn't work; check_sum of chunk1 matches for address %Lu \n", chunkaddr);
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
			printf("updated was not written properly into snapdev. \n");
			return -1;
		}
		chunk_num++;
	}
	
	if (chunk_num != check_chunk_num) {
		printf("Number of chunks don't match up.\n");
		close(snapdev);
		return -2; /* FIXME: use named error */
	}
	
	close(snapdev);
	return 0;
}

int ddsnap_apply_delta(char const *deltaname, char const *devname) {
	int deltafile;
	
	deltafile = open(deltaname, O_RDONLY);
	if (deltafile<0) {
		printf("Could not open delta file \"%s\" for reading.\n", deltaname);
		return 1;
	}
	
	if (apply_delta(deltafile, devname) < 0) {
		printf("Could not apply delta file \"%s\" to snapdev \"%s\"\n", deltaname, devname);
		close(deltafile);
		return 1;
	}
	
	close(deltafile);
	
	return 0;
}

int list_snapshots(int sock) {
	if (outbead(sock, LIST_SNAPSHOTS, struct create_snapshot, 0) < 0)
		return eek();
	
	struct head head;
	unsigned maxbuf = 500;
	char buf[maxbuf];
	int err;
	
	if ((err = readpipe(sock, &head, sizeof(head))))
		return eek();
	
	struct snapinfo * buffer = (struct snapinfo *) malloc(head.length-sizeof(int));
	int i, count;
	
	readpipe(sock, &count, sizeof(int));
	readpipe(sock, buffer, head.length-sizeof(int));
	
	printf("Snapshot list: \n");
	
	for (i=0; i<count; i++) {
		time_t snap_time = (time_t)(buffer[i]).ctime;

		printf("Snapshot[%d]: \n", i);
		printf("\ttag= %Lu \t", (buffer[i]).snap);
		printf("priority= %d \t", (buffer[i]).prio);
		printf("use count= %d \t", (buffer[i]).usecnt);
		printf("creation time= %s \n", ctime(&snap_time));
	}
	
	trace_on(printf("reply = %x\n", head.code););
	err = head.code != SNAPSHOT_LIST;
	
	if (head.code == REPLY_ERROR)
		error("%.*s", head.length - 4, buf + 4);
	
	/* server doesn't return a reply? should return something */

	return 0;
}

int generate_changelist(int sock, char const *changelist_filename, int snap1, int snap2) {
	unsigned maxbuf = 500;
	char buf[maxbuf];
	
	int change_fd = open(changelist_filename, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);
	
	if (change_fd < 0)
		error("unable to open file %s: %s", changelist_filename, strerror(errno));
	
	struct cl_header cl = { };
	strncpy(cl.magic, CHANGELIST_MAGIC_ID, MAGIC_SIZE);

	if (write(change_fd, &cl, sizeof(cl)) < 0)
		error("unable to write magic information to changelist file");

	if (outbead(sock, GENERATE_CHANGE_LIST, struct generate_changelist, snap1, snap2) < 0)
		return eek();
	
	/* send fd to server */
	if (send_fd(sock, change_fd) < 0)
		error("unable to send fd to server");
	
        struct head head;
	int err;
        if ((err = readpipe(sock, &head, sizeof(head))))
                return eek();
        assert(head.length < maxbuf); // !!! don't die
        if ((err = readpipe(sock, buf, head.length)))
                return eek();

        trace_on(printf("reply = %x\n", head.code););
        err = head.code != REPLY_GENERATE_CHANGE_LIST; /* FIXME: we don't use err after this */

        if (head.code == REPLY_ERROR)
                error("received error code: %.*s", head.length - 4, buf + 4);

	close(change_fd);
        return 0;
}

int delete_snapshot(int sock, int snap) {
	if (outbead(sock, DELETE_SNAPSHOT, struct create_snapshot, snap) < 0)
		return eek();

	struct head head;
	unsigned maxbuf = 500;
	char buf[maxbuf];
	int err;

	if ((err = readpipe(sock, &head, sizeof(head))))
		return eek();
	assert(head.length < maxbuf); // !!! don't die
	trace_on(printf("reply head.length = %x\n", head.length););
	if ((err = readpipe(sock, buf, head.length)))
		return eek();
	trace_on(printf("reply = %x\n", head.code););
	err = head.code != REPLY_DELETE_SNAPSHOT;

	if (head.code == REPLY_ERROR)
		error("%.*s", head.length - 4, buf + 4);

	return 0;
}

int create_snapshot(int sock, int snap) {
	if (outbead(sock, CREATE_SNAPSHOT, struct create_snapshot, snap) < 0)
		return eek();

	struct head head;
	unsigned maxbuf = 500;
	char buf[maxbuf];
	int err;

	if ((err = readpipe(sock, &head, sizeof(head))))
		return eek();
	assert(head.length < maxbuf); // !!! don't die
	if ((err = readpipe(sock, buf, head.length)))
		return eek();
	trace_on(printf("reply = %x\n", head.code););
	err = head.code != REPLY_CREATE_SNAPSHOT;

	if (head.code == REPLY_ERROR)
		error("%.*s", head.length - 4, buf + 4);

	return 0;
}

int set_priority(int sock, uint32_t tag_val, int8_t pri_val) {
	if (outbead(sock, SET_PRIORITY, struct snapinfo, tag_val, pri_val) < 0)
		return eek();

	struct head head;
	unsigned maxbuf = 500;
	char buf[maxbuf];
	int err = 0;

	if ((err = readpipe(sock, &head, sizeof(head))))
		return eek();
	assert(head.length < maxbuf); // !!! don't die
	if ((err = readpipe(sock, buf, head.length)))
		return eek();
	trace_on(printf("reply = %x\n", head.code););
	err = (head.code != REPLY_SET_PRIORITY);

	if (head.code == REPLY_ERROR || err)
		error("%.*s", head.length - 4, buf + 4);

	return 0;
}

int set_usecount(int sock, uint32_t tag_val, const char * op) {
	int usecnt = (strcmp(op, "inc") == 0) ? 1 : ((strcmp(op, "dec") == 0) ? -1 : 0);

	if (outbead(sock, SET_USECOUNT, struct snapinfo, tag_val, 0, usecnt) < 0)
		return eek();

	struct head head;
	unsigned maxbuf = 500;
	char buf[maxbuf];
	int err = 0;

	if ((err = readpipe(sock, &head, sizeof(head))))
		return eek();
	assert(head.length < maxbuf); // !!! don't die
	if ((err = readpipe(sock, buf, head.length)))
		return eek();
	trace_on(printf("reply = %x\n", head.code););
	err = (head.code != REPLY_SET_USECOUNT);

	if (head.code == REPLY_ERROR || err)
		error("%.*s", head.length - 4, buf + 4);

	return 0;
}

static int daemon(int lsock, char const *devname)
{
	int csock;

	for (;;) {
		if ((csock = accept_socket(lsock)) < 0)
			goto cleanup_connection; /* FIXME: log errors */

		/* FIXME: fork */
		if (apply_delta(csock, devname) < 0)
			goto cleanup_connection; /* FIXME: log errors */

	cleanup_connection:
		close(csock);
	}

	return 0;
}

static void mainUsage(void)
{
	printf("usage: ddsnap [-?|--help] <subcommand>\n"
		"\n"
		"Available subcommands:\n"
		"	create-snap\n"
		"	delete-snap\n"
		"	list\n"
		"	set-priority\n"
		"	set-usecount\n"
		"	create-cl\n"
		"	create-delta\n"
		"	apply-delta\n"
		"	send-delta\n"
		"	daemon\n");
}

static void cdUsage(poptContext optCon, int exitcode, char const *error, char const *addl) {
	poptPrintUsage(optCon, stderr, 0);
	if (error) fprintf(stderr, "%s: %s", error, addl);
	exit(exitcode);
}

int main(int argc, char *argv[]) {
	char const *command;
	int sock, help = 0, usage = 0;
	int xd = FALSE, raw = FALSE, test = FALSE, gzip_level = 0, ret = 0, mode = 0;
	
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
		  "Create snapshot usage: create-snap <sockname> <snapshot>" , NULL },		
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Delete snapshot usage: delete-snap <sockname> <snapshot>" , NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "List snapshots usage: list <sockname>" , NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Set priority usage: set-priority <sockname> <snap_tag> <new_priority_value>" , NULL },	
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Set usecount usage: set-usecount <sockname> <snap_tag> <inc|dec>" , NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Create changelist usage: create-cl <sockname> <changelist> <snapshot1> <snapshot2>" , NULL },	
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &cdOptions, 0,
		  "Create delta usage: create-delta <changelist> <deltafile> <snapshot1> <snapshot2>" , NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Apply delta usage: apply-delta <deltafile> <dev>" , NULL },
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Send delta usage: send-delta <changelist> host[:port] <snapshot1> <snapshot2>" , NULL },	
		{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &noOptions, 0,
		  "Daemon usage: daemon [host[:port]] <dev>" , NULL },	
		POPT_AUTOHELP
		POPT_TABLEEND
	};
	mainCon = poptGetContext(NULL, argc, (const char **)argv, mainOptions, 0);

	if (help || argc < 2) {
		poptPrintHelp(mainCon, stdout, 0);
		exit(1);
	}
	
	// no usage displayed overall
	if (usage) {
		poptPrintUsage(mainCon, stdout, 0);
		exit(1);
	}

	command = argv[1];

	if (strcmp(command, "--help")==0 || strcmp(command, "-?")==0) {
		poptPrintHelp(mainCon, stdout, 0);
		exit(1);
	}
	if (strcmp(command, "--usage")==0) {
		mainUsage();
		exit(1);
	}

	//poptResetContext(mainCon);
	mainCon = poptFreeContext(mainCon);	

	if (strcmp(command, "create-snap")==0) {
		if (argc != 4) {
			printf("Usage: %s create-snap <sockname> <snapshot>\n", argv[0]);
			return 1;
		}
		sock = create_socket(argv[2]);
		return create_snapshot(sock, atoi(argv[3]));
	}
	if (strcmp(command, "delete-snap")==0) {
		if (argc != 4) {
			printf("Usage: %s delete-snap <sockname> <snapshot>\n", argv[0]);
			return 1;
		}
		sock = create_socket(argv[2]);
		return delete_snapshot(sock, atoi(argv[3]));
	}
	if (strcmp(command, "list")==0) {
		if (argc != 3) {
			printf("Usage: %s list <sockname>\n", argv[0]);
			return 1;
		}
		sock = create_socket(argv[2]);
		return list_snapshots(sock);
	}
	if (strcmp(command, "set-priority")==0) {
		if (argc != 5) {
			printf("usage: %s set-priority <sockname> <snap_tag> <new_priority_value>\n", argv[0]);
			return 1;
		}
		sock = create_socket(argv[2]);
		return set_priority(sock, atoi(argv[3]), atoi(argv[4]));
	}
	if (strcmp(command, "set-usecount")==0) {
		if (argc != 5) {
			printf("usage: %s set-usecount <sockname> <snap_tag> <inc|dec>\n", argv[0]);
			return 1;
		}
		sock = create_socket(argv[2]);
		return set_usecount(sock, atoi(argv[3]), argv[4]);
	}
	if (strcmp(command, "create-cl")==0) {
		if (argc != 6) {
			printf("usage: %s create-cl <sockname> <changelist> <snapshot1> <snapshot2>\n", argv[0]);
			return 1;
		}
		sock = create_socket(argv[2]);
		return generate_changelist(sock, argv[3], atoi(argv[4]), atoi(argv[5]));
	}
	if (strcmp(command, "create-delta")==0) {
		char cdOpt, *changelist, *deltafile, *snapdev1, *snapdev2;
	
		poptContext cdCon;

		struct poptOption options[] = {
			{ NULL, '\0', POPT_ARG_INCLUDE_TABLE, &cdOptions, 0,
			  NULL, NULL },
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

		mode = (xd ? XDELTA : (raw ? RAW : TEST));

		changelist = (char *) poptGetArg(cdCon);
		deltafile  = (char *) poptGetArg(cdCon);
		snapdev1   = (char *) poptGetArg(cdCon);
		snapdev2   = (char *) poptGetArg(cdCon);

		if ((changelist == NULL) || !(poptPeekArg(cdCon) == NULL))
			cdUsage(cdCon, 1, "Specify a changelist", ".e.g., cl01 \n");
		if ((deltafile == NULL) || !(poptPeekArg(cdCon) == NULL))
			cdUsage(cdCon, 1, "Specify a deltafile", ".e.g., df01 \n");
		if ((snapdev1 == NULL) || !(poptPeekArg(cdCon) == NULL))
			cdUsage(cdCon, 1, "Specify a snapdev1", ".e.g., /dev/mapper/snap0 \n");
		if ((snapdev2 == NULL) || !(poptPeekArg(cdCon) == NULL))
			cdUsage(cdCon, 1, "Specify a snapdev2", ".e.g., /dev/mapper/snap1 \n");

		ret = ddsnap_generate_delta(mode, gzip_level, changelist, deltafile, snapdev1, snapdev2);

		poptFreeContext(cdCon);
		return ret;
	}
	if (strcmp(command, "apply-delta")==0) {
		if (argc != 4) {
			printf("usage: %s apply-delta <deltafile> <dev>\n", argv[0]);
			return 1;
		}
		return ddsnap_apply_delta(argv[2], argv[3]);
	}
	if (strcmp(command, "send-delta")==0) {
		char const *hostname;
		unsigned port;
		int retval;

		if (argc != 6) {
			printf("usage: %s send-delta <changelist> host[:port] <snapshot1> <snapshot2>\n", argv[0]);
			return 1;
		}

		hostname = argv[3];
		if (strchr(hostname, ':')) {
			unsigned int len = strlen(hostname);

			port = parse_port(hostname, &len);
			argv[3][len] = '\0';
		} else {
			port = DEFAULT_REPLICATION_PORT;
		}

		sock = open_socket(hostname, port);
		if (sock < 0) {
			printf("Error: unable to connect to %s port %u\n", hostname, port);
			return 1;
		}
		/* FIX ME */
		/*
		retval = ddsnap_generate_delta(argv[2], sock, argv[4], argv[5]);
		*/
		close(sock);

		return retval;
	}
	if (strcmp(command, "daemon")==0) {
		char const *hostname;
		unsigned port;
		char const *devname;

		if (argc < 3 || argc > 4) {
			printf("usage: %s daemon [host[:port]] <dev>\n", argv[0]);
			return 1;
		}

		if (argc < 4) {
			char buffer[]="0.0.0.0";

			port = DEFAULT_REPLICATION_PORT;
			hostname = buffer;
			devname = argv[2];
		} else {
			hostname = argv[2];
			if (strchr(hostname, ':')) {
				unsigned int len = strlen(hostname);

				port = parse_port(hostname, &len);
				argv[2][len] = '\0';
			} else {
				port = DEFAULT_REPLICATION_PORT;
			}
			devname = argv[3];
		}

		sock = bind_socket(hostname, port);
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

		return daemon(sock, devname);

	}
	printf("Unrecognized command %s.\n", command);
	return 1;
}

