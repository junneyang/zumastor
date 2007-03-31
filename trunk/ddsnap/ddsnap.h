#include <unistd.h>
#include <errno.h>

#define u8 unsigned char
#define s8 char
#define u16 unsigned short
#define s16 short
#define s32 int
#define u32 unsigned
#define u64 unsigned long long

#define U64FMT "%llu"

#define le_u32 u32
#define le_u16 u16
#define le_u64 u64
#define EFULL ENOMEM
#define PACKED __attribute__ ((packed))

#ifndef __DDSNAP_H
#define __DDSNAP_H
static inline int readpipe(int fd, void *buffer, size_t count)
{
	int n;

	while (count) {
		if ((n = read(fd, buffer, count)) < 1)
			return n == 0 ? -EPIPE : -errno;
		buffer += n;
		count -= n;
	}

	return 0;
}

static inline int writepipe(int fd, void const *buffer, size_t count)
{
	int n;

	while (count) {
		if ((n = write(fd, buffer, count)) < 0)
			return -errno;
		buffer += n;
		count -= n;
	}

	return 0;
}

#define outbead(SOCK, CODE, STRUCT, VALUES...) ({ \
	struct { struct head head; STRUCT body; } PACKED message = \
		{ { CODE, sizeof(STRUCT) }, { VALUES } }; \
	writepipe(SOCK, &message, sizeof(message)); })

#define outhead(SOCK, CODE, SIZE) writepipe(SOCK, &(struct head){ CODE, SIZE }, sizeof(struct head) )

typedef unsigned long long chunk_t;

struct server { struct server_head { u8 type; u8 length; } header; char *address; } PACKED;

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 256
#endif

#define MAX_SNAPSHOTS 64
#define SNAPSHOT_SQUASHED MAX_SNAPSHOTS

struct change_list
{
	u64 count;
	u64 length;
	u32 chunksize_bits;
	u32 src_snap;
	u32 tgt_snap;
	u64 *chunks;
};

#endif // __DDSNAP_H
