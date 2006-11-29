#ifndef __DDSNAP_DAEMON_H
#define __DDSNAP_DAEMON_H

#include "buffer.h"
#include "ddsnap.common.h"
#include "list.h"
#include "sock.h"
#include "trace.h"

#define MAX_ERRMSG_SIZE 128

#define SECTORS_PER_BLOCK 7
#define CHUNK_SIZE 4096
#define DEFAULT_JOURNAL_SIZE (100 * CHUNK_SIZE)
#define INPUT_ERROR 0

#define SB_DIRTY 1
#define SB_SECTOR 8
#define SB_SIZE 4096
#define SB_MAGIC "snapshot"

struct superblock
{
	/* Persistent, saved to disk */
	struct disksuper
	{
		char magic[8];
		u64 create_time;
		sector_t etree_root;
		sector_t orgoffset, orgsectors;
		u64 flags;
		u32 blocksize_bits, chunksize_bits;
		u64 deleting;
		struct snapshot
		{
			u32 ctime; // upper 32 bits are in super create_time
			u32 tag;   // external name (id) of snapshot
			u8 bit;    // internal snapshot number, not derived from tag
			s8 prio;   // 0=normal, 127=highest, -128=lowestdrop
			u8 usecnt; // use count on snapshot device (just use a bit?)
			char reserved[7];
		} snaplist[MAX_SNAPSHOTS]; // entries are contiguous, in creation order
		u32 snapshots;
		u32 etree_levels;
		s32 journal_base, journal_next, journal_size;
		u32 sequence;
		struct allocation_info {
			sector_t bitmap_base;
			sector_t chunks;
			sector_t freechunks;
			chunk_t  last_alloc;
			u64      bitmap_blocks;
		} alloc[2]; /* shouldn't be hardcoded? */
	} image;

	/* Derived, not saved to disk */
	u64 snapmask;
	u32 blocksize, chunksize, blocks_per_node;
	u32 sectors_per_block_bits, sectors_per_block;
	u32 sectors_per_chunk_bits, sectors_per_chunk;
	unsigned flags;
	unsigned snapdev, metadev, orgdev;
	unsigned snaplock_hash_bits;
	struct snaplock **snaplocks;
	unsigned copybuf_size;
	char *copybuf;
	chunk_t source_chunk;
	chunk_t dest_exception;
	unsigned copy_chunks;
	unsigned max_commit_blocks;
	struct allocation_info  *metadata, *snapdata, *current_alloc;
};

int snap_server_setup(const char *agent_sockname, const char *server_sockname, int *listenfd, int *getsigfd, int *agentfd);
int snap_server(struct superblock *sb, int listenfd, int getsigfd, int agentfd);

int init_snapstore(struct superblock *sb, u32 js_bytes, u32 bs_bits);

u32 strtobytes(char const *string);
u32 strtobits(char const *string);

#endif // __DDSNAP_DAEMON_H
