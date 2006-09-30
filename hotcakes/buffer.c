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

static struct buffer *buffer_table[BUFFER_BUCKETS];
LIST_HEAD(dirty_buffers);
unsigned dirty_buffer_count;
LIST_HEAD(lru_buffers);
unsigned lru_buffer_count;

#define MAX_BUFFERS_IN_LRU 5000 /* that's 640Mb for 64K chunks */

void set_buffer_dirty(struct buffer *buffer)
{
	buftrace(printf("set_buffer_dirty %llx state=%u\n", buffer->sector, buffer->flags & BUFFER_STATE_MASK););
	if (!buffer_dirty(buffer)) {
		list_add_tail(&buffer->dirty_list, &dirty_buffers);
		dirty_buffer_count++;
	}
	if (buffer->lru_list.next != LIST_POISON1) {
		list_del(&buffer->lru_list);
		lru_buffer_count--;
	}
	buffer->flags = BUFFER_STATE_DIRTY; //| (buffer->flags & ~BUFFER_STATE_MASK);
}

static void check_lru_list(struct buffer *buffer) 
{
	/* check to see if buffer limit has been reached */
	if (lru_buffer_count >= MAX_BUFFERS_IN_LRU) {
		buftrace(printf("need to purge a buffer from the lru list\n"););
		struct list_head *list;
		struct buffer * buffer_candidate = (struct buffer *)NULL;
		list_for_each(list, &lru_buffers) {
			struct buffer *lru_buffer = list_entry(list, struct buffer, lru_list);
			if (lru_buffer->count == 0) {
				buffer_candidate = lru_buffer;
				break;
			}
		}
		if (!buffer_candidate || buffer == buffer_candidate) {
			warn("unable to delete buffer\n");
		}
		else {
			buftrace(printf("buffer has been purged\n"););
			lru_buffer_count--;
			list_del(&buffer_candidate->lru_list);
			evict_buffer(buffer_candidate);
		}
	}
}

void set_buffer_uptodate(struct buffer *buffer)
{
	if (buffer_dirty(buffer)) {
		list_del(&buffer->dirty_list);
		dirty_buffer_count--;
	}
	if(buffer->lru_list.next == LIST_POISON1) { /* not a member of the LRU list */
		list_add_tail(&buffer->lru_list, &lru_buffers);
		lru_buffer_count++;
	}

	check_lru_list(buffer);
	buffer->flags = BUFFER_STATE_CLEAN; // | (buffer->flags & ~BUFFER_STATE_MASK);
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

struct buffer *new_buffer(sector_t sector, unsigned size)
{
	buftrace(printf("Allocate buffer for %llx\n", sector););
	struct buffer *buffer = (struct buffer *)malloc(sizeof(struct buffer));
	posix_memalign((void **)&(buffer->data), size, size); // what if malloc fails?
	buffer->count = 1;
	buffer->flags = 0;
	buffer->size = size;
	buffer->sector = sector;
	buffer->lru_list.next = LIST_POISON1;
	buffer->lru_list.prev = LIST_POISON2;
	return buffer;
}

static void lru_update(struct buffer * buffer) {
	/* now place it on the lru list */
	if(buffer->lru_list.next == LIST_POISON1) 
		return;
	buftrace(printf("moving buffer to be most recently used!\n"););
	list_del(&buffer->lru_list);
	list_add_tail(&buffer->lru_list, &lru_buffers);	
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

void evict_buffer(struct buffer *buffer)
{
	if (buffer_dirty(buffer))  
		write_buffer(buffer);
	if (buffer->lru_list.next != LIST_POISON1) {
		list_del(&buffer->lru_list);
		lru_buffer_count--;
	}
        if(remove_buffer(buffer) != (struct buffer *)NULL) {
		buftrace(printf("Evicted buffer for %llx\n", buffer->sector););
		free(buffer->data); // using posix_memalign though !!! malloc_aligned means pointer is wrong
		free(buffer);
		return;
	}
       	error("buffer not found");
}

void evict_buffers(void) // !!! should use lru list
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
}
