
struct change_list
{
	u64 count;
	u64 length;
	u32 chunksize_bits;
	u64 *chunks;
};


extern struct change_list *init_change_list(u32 chunksize_bits);
extern int append_change_list(struct change_list *cl, u64 chunkaddr);
extern void free_change_list(struct change_list *cl);

