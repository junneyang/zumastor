#ifndef __DDSNAP_DAEMON_H
#define __DDSNAP_DAEMON_H

#include "buffer.h"
#include "ddsnap.common.h"
#include "list.h"
#include "sock.h"
#include "trace.h"

#define SECTORS_PER_BLOCK 7
#define CHUNK_SIZE 4096
#define DEFAULT_JOURNAL_SIZE (100 * CHUNK_SIZE)
#define INPUT_ERROR 0

#define SB_DIRTY 1
#define SB_SECTOR 8
#define SB_SIZE 4096
#define SB_MAGIC "snapshot"

#define METADATA_ALLOC(SB) ((SB)->image.alloc[0])
#define SNAPDATA_ALLOC(SB) ((SB)->image.alloc[((SB)->metadev == (SB)->snapdev) ? 0 : 1])

#define DDSNAPD_CLIENT_ERROR -1
#define DDSNAPD_AGENT_ERROR -2
#define DDSNAPD_CAUGHT_SIGNAL -3

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
		u64 deleting;
		struct snapshot
		{
			u32 ctime; // upper 32 bits are in super create_time
			u32 tag;   // external name (id) of snapshot
			u8 bit;    // internal snapshot number, not derived from tag
			s8 prio;   // 0=normal, 127=highest, -128=lowestdrop
			u16 usecnt; // use count on snapshot device (just use a bit?)
			char reserved[7];  // change me to 4 !!!
		} snaplist[MAX_SNAPSHOTS]; // entries are contiguous, in creation order
		u32 snapshots;
		u32 etree_levels;
		s32 journal_base, journal_next, journal_size;
		u32 sequence;
		u64 meta_chunks_used, snap_chunks_used;
		struct allocspace_img
		{
			sector_t bitmap_base;
			sector_t chunks;
			sector_t freechunks;
			chunk_t  last_alloc;
			u64      bitmap_blocks;
			u32      allocsize_bits;
		} alloc[2]; 
	} image;

	/* Derived, not saved to disk */
	u64 snapmask;
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
	struct allocspace {
		struct allocspace_img *asi;
		u32 allocsize;  
		u32 sectors_per_alloc_bits, sectors_per_alloc;
		u32 alloc_per_node;  /* only for metadata */
	} metadata, snapdata;
};

int snap_server_setup(const char *agent_sockname, const char *server_sockname, int *listenfd, int *getsigfd, int *agentfd);
int snap_server(struct superblock *sb, int listenfd, int getsigfd, int agentfd);

int init_snapstore(struct superblock *sb, u32 js_bytes, u32 bs_bits, u32 cs_bits);

u32 strtobytes(char const *string);
u32 strtobits(char const *string);

#endif // __DDSNAP_DAEMON_H
