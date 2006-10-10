#define _XOPEN_SOURCE 600 /* pwrite >=500(?), posix_memalign needs >= 600*/
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <errno.h> 
#include "list.h"
#include "diskio.h"
#include "buffer.h"
#include "trace.h"

#define buftrace trace_off

/*
 * Kernel-like buffer api
 */

/*
 * Even though we are in user space, for reasons of durability and speed
 * we need to access the block directly, handle our own block caching and
 * keep track block by block of which parts of the on-disk data structures
 * as they are accessed and modified.  There's no need to reinvent the
 * wheel here.  I have basically cloned the traditional Unix kernel buffer
 * paradigm, with one small twists of my own, that is, instead of state
 * bits we use scalar values.  This captures the notion of buffer state
 * transitions more precisely than the traditional approach.
 *
 * One big benefit of using a buffer paradigm that looks and acts very
 * much like the kernel incarnation is, porting this into the kernel is
 * going to be a whole lot easier.  Most higher level code will not need
 * to be modified at all.  Another benefit is, it will be much easier to
 * add async IO.
 */
struct list_head lru_buffers;
struct list_head free_buffers;

static struct buffer *buffer_table[BUFFER_BUCKETS];
LIST_HEAD(dirty_buffers);
unsigned dirty_buffer_count;
LIST_HEAD(lru_buffers);
unsigned lru_buffer_count;
LIST_HEAD(free_buffers);
unsigned free_buffer_count;

#define MAX_BUFFERS_IN_LRU 10000 /* 2*chunksize*MAX_BUFFERS_IN_LRU is the maximum memory used */
#define MAX_FREE_BUFFERS   (MAX_BUFFERS_IN_LRU/10) /* free 10 percent of the buffers */

void set_buffer_dirty(struct buffer *buffer)
{
	buftrace(printf("set_buffer_dirty %llx state=%u\n", buffer->sector, buffer->flags & BUFFER_STATE_MASK););
	if (!buffer_dirty(buffer)) {
		list_add_tail(&buffer->dirty_list, &dirty_buffers);
		dirty_buffer_count++;
	}
	buffer->flags = BUFFER_STATE_DIRTY; 
}

void set_buffer_uptodate(struct buffer *buffer)
{
	if (buffer_dirty(buffer)) {
		list_del(&buffer->dirty_list);
		dirty_buffer_count--;
	}
	buffer->flags = BUFFER_STATE_CLEAN; 
}

void brelse(struct buffer *buffer)
{
	buftrace(printf("Release buffer %llx\n", buffer->sector););
	if (!--buffer->count)
		trace_off(printf("Free buffer %llx\n", buffer->sector));
}

void brelse_dirty(struct buffer *buffer)
{
	buftrace(printf("Release dirty buffer %llx\n", buffer->sector););
	set_buffer_dirty(buffer);
	brelse(buffer);
}

int write_buffer_to(struct buffer *buffer, sector_t sector)
{
	return diskio(buffer->fd, buffer->data, buffer->size, sector << SECTOR_BITS, 1);
}

int write_buffer(struct buffer *buffer)
{
	buftrace(warn("write buffer %Lx/%u", buffer->sector, buffer->size););
	int err = write_buffer_to(buffer, buffer->sector);

	if (!err)
		set_buffer_uptodate(buffer);
	return err;
}

int read_buffer(struct buffer *buffer)
{
	buftrace(warn("read buffer %llx", buffer->sector););
	int err = diskio(buffer->fd, buffer->data, buffer->size, buffer->sector << SECTOR_BITS, 0);

	if (!err)
		set_buffer_uptodate(buffer);
	return err;
}

unsigned buffer_hash(sector_t sector)
{
	return (((sector >> 32) ^ (sector_t)sector) * 978317583) % BUFFER_BUCKETS;
}

static void lru_add(struct buffer *buffer) 
{	
	lru_buffer_count++;
	list_add_tail(&buffer->list, &lru_buffers);
}

static void lru_remove(struct buffer *buffer) 
{
	lru_buffer_count--;
	list_del(&buffer->list);
}

static void lru_update(struct buffer * buffer) {
	/* now place it on the lru list */
	buftrace(printf("moving buffer to be most recently used!\n"););
	list_del(&buffer->list);
	list_add_tail(&buffer->list, &lru_buffers);	
}

static struct buffer *remove_buffer(struct buffer *buffer) 
{
	struct buffer **pbuffer = &buffer_table[buffer_hash(buffer->sector)], **prev = pbuffer;
       	
	assert(*pbuffer != NULL);

       	if (*pbuffer == buffer) { /* head of list */
		buftrace(printf("buffer is head of list\n"););
		*pbuffer = buffer->hashlist;
		goto buffer_removed;
	}
	buftrace(printf("not the head of the list\n"););
	for (pbuffer = &(*pbuffer)->hashlist; *pbuffer; pbuffer = &(*pbuffer)->hashlist) {
		if (*pbuffer == buffer) {
			(*prev)->hashlist = buffer->hashlist;
			goto buffer_removed;
		}	
		prev = pbuffer;
	}
	return (struct buffer *)NULL;
 buffer_removed:
	buftrace(printf("Removed buffer for %llx from buffer table\n", buffer->sector););
	buffer->hashlist = NULL;
	return buffer;
}

static void add_free_buffer(struct buffer *buffer) { 
	free_buffer_count++;
	list_add_tail(&buffer->list, &free_buffers);
}
static struct buffer *get_free_buffer(void) 
{
	struct buffer *buffer = NULL;
	if (free_buffer_count > 0) {
		free_buffer_count--;
		buffer = list_entry(free_buffers.next, struct buffer, list);
		list_del(&buffer->list);
	}
	return buffer;
}

static struct buffer *check_lru_list(void) 
{	
	struct buffer *cand_buffer = (struct buffer *)NULL;
	if ( ((cand_buffer = get_free_buffer()) != NULL) || lru_buffer_count < MAX_BUFFERS_IN_LRU)
		return cand_buffer;

	buftrace(printf("need to purge a buffer from the lru list\n"););
	struct list_head *list, *safe;	
	int count = 0;
	
	list_for_each_safe(list, safe, &lru_buffers) {
		struct buffer *buffer = list_entry(list, struct buffer, list);
		if (buffer->count == 0 && !buffer_dirty(buffer) && buffer_uptodate(buffer)) {
			lru_remove(buffer);
			remove_buffer(buffer); /* remove from hashlist */
			add_free_buffer(buffer);
			if(++count == MAX_FREE_BUFFERS)
				break;			
		}
	}

       	return get_free_buffer();
}
	
struct buffer *new_buffer(sector_t sector, unsigned size)
{
	buftrace(printf("Allocate buffer for %llx\n", sector););
	struct buffer *buffer;

	if (!(buffer = check_lru_list())) {
		buftrace(warn("allocating a new buffer"););
		if (lru_buffer_count == MAX_BUFFERS_IN_LRU) {
			warn("Number of dirty buffers: %d", dirty_buffer_count);
			error("Out of Memory"); /* need to handle this properly */
		}
		buffer = (struct buffer *)malloc(sizeof(struct buffer));
		posix_memalign((void **)&(buffer->data), size, size); // what if malloc fails?
	}
	else if (buffer->size != size) { /* reusing buffer, make sure data is the right size */
		buftrace(warn("reusing buffer with a different size"););
		free(buffer->data);
		posix_memalign((void **)&(buffer->data), size, size); 
	}
 	
	buffer->count = 1;
	buffer->flags = 0;
	buffer->size = size;
	buffer->sector = sector;
	/* insert into LRU list */
	lru_add(buffer);

	return buffer;
}

struct buffer *getblk(unsigned fd, sector_t sector, unsigned size)
{
	struct buffer **bucket = &buffer_table[buffer_hash(sector)], *buffer;

	for (buffer = *bucket; buffer; buffer = buffer->hashlist)
		if (buffer->sector == sector) {
			buftrace(printf("Found buffer for %llx\n", sector););
 			buffer->count++;
			lru_update(buffer); 
			return buffer;
		}
	buffer = new_buffer(sector, size);
	buffer->fd = fd;
	buffer->hashlist = *bucket;
	*bucket = buffer;
	return buffer;
}

struct buffer *bread(unsigned fd, sector_t sector, unsigned size)
{
	struct buffer *buffer = getblk(fd, sector, size);

	if (buffer_uptodate(buffer) || buffer_dirty(buffer))
		return buffer;
	read_buffer(buffer);
	if (!buffer_uptodate(buffer)) {
		brelse(buffer);
		error("bad read");
		return NULL;
	}
	return buffer;
}

void evict_buffer(struct buffer *buffer)
{
	if (buffer_dirty(buffer))  
		write_buffer(buffer);
	lru_remove(buffer);
        if (remove_buffer(buffer)) 
		warn("buffer not found in hashlist");
	buftrace(printf("Evicted buffer for %llx\n", buffer->sector););
	free(buffer->data); // using posix_memalign though !!! malloc_aligned means pointer is wrong
	free(buffer);
}

void evict_buffers(void) 
{
	unsigned i;
	for (i = 0; i < BUFFER_BUCKETS; i++)
	{
		struct buffer *buffer;
		for (buffer = buffer_table[i]; buffer;) {
			struct buffer *next = buffer->hashlist;
			if (!buffer->count)
				evict_buffer(buffer);
			buffer = next;
		}
		buffer_table[i] = NULL; /* all buffers have been freed in this bucket */
	}
}

void flush_buffers(void) // !!! should use lru list
{
	while (!list_empty(&dirty_buffers)) {
		struct list_head *entry = dirty_buffers.next;
		struct buffer *buffer = list_entry(entry, struct buffer, dirty_list);
		if (buffer_dirty(buffer))
			write_buffer(buffer);
	}
}

void show_buffer(struct buffer *buffer)
{
	printf("%s%llx/%i ", 
		buffer_dirty(buffer)? "+": buffer_uptodate(buffer)? "": "?", 
		buffer->sector, buffer->count);
}

void show_buffers_(int all)
{
	unsigned i;

	for (i = 0; i < BUFFER_BUCKETS; i++)
	{
		struct buffer *buffer = buffer_table[i];

		if (!buffer)
			continue;

		printf("[%i] ", i);
		for (; buffer; buffer = buffer->hashlist)
			if (all || buffer->count)
				show_buffer(buffer);
		printf("\n");
	}
}

void show_active_buffers(void)
{
	printf("Active buffers:\n");
	show_buffers_(0);
}

void show_buffers(void)
{
	printf("Buffers:\n");
	show_buffers_(1);
}

void show_dirty_buffers(void)
{
	struct list_head *list;

	printf("Dirty buffers: ");
	list_for_each(list, &dirty_buffers) {
		struct buffer *buffer = list_entry(list, struct buffer, dirty_list);
		printf("%llx ", buffer->sector);
	}
	printf("\n");
}

#if 0
void dump_buffer(struct buffer *buffer, unsigned offset, unsigned length)
{
	hexdump(buffer->data + offset, length);
}
#endif

void init_buffers(void)
{
	memset(buffer_table, 0, sizeof(buffer_table));
	INIT_LIST_HEAD(&dirty_buffers);
	dirty_buffer_count = 0;
	INIT_LIST_HEAD(&lru_buffers);
	lru_buffer_count = 0;
	INIT_LIST_HEAD(&free_buffers);
	free_buffer_count = 0;
}
