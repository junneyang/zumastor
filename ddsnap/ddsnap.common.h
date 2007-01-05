#ifndef __DDSNAP_COMMON_H
#define __DDSNAP_COMMON_H

#define MAX_ERRMSG_SIZE 128

struct change_list
{
	u64 count;
	u64 length;
	u32 chunksize_bits;
	u32 src_snap;
	u32 tgt_snap;
	u64 *chunks;
};


extern struct change_list *init_change_list(u32 chunksize_bits, u32 src_snap, u32 tgt_snap);
extern int append_change_list(struct change_list *cl, u64 chunkaddr);
extern void free_change_list(struct change_list *cl);

#endif // __DDSNAP_COMMON_H
