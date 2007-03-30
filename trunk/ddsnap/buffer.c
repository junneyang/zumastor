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
unsigned buffer_count;
LIST_HEAD(free_buffers);

static unsigned max_buffers = 10000;
static unsigned max_free_buffers = 1000; /* free 10 percent of the buffers */ 

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
	return diskwrite(buffer->fd, buffer->data, buffer->size, sector << SECTOR_BITS);
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
	int err = diskread(buffer->fd, buffer->data, buffer->size, buffer->sector << SECTOR_BITS);

	if (!err)
		set_buffer_uptodate(buffer);
	return err;
}

unsigned buffer_hash(sector_t sector)
{
	return (((sector >> 32) ^ (sector_t)sector) * 978317583) % BUFFER_BUCKETS;
}

static void add_buffer_lru(struct buffer *buffer) 
{	
	buffer_count++;
	list_add_tail(&buffer->list, &lru_buffers);
}

static void remove_buffer_lru(struct buffer *buffer) 
{
	buffer_count--;
	list_del(&buffer->list);
}

static struct buffer *remove_buffer_hash(struct buffer *buffer) 
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

static void add_buffer_free(struct buffer *buffer) 
{
	list_add_tail(&buffer->list, &free_buffers);
}

static struct buffer *remove_buffer_free(void) 
{
	struct buffer *buffer = NULL;
	if (!list_empty(&free_buffers)) {
		buffer = list_entry(free_buffers.next, struct buffer, list);
		list_del(&buffer->list);
	}
	return buffer;
}

struct buffer *new_buffer(sector_t sector, unsigned size)
{
	buftrace(printf("Allocate buffer for %llx\n", sector););
	struct buffer *buffer = NULL;

	/* check if we hit the MAX_BUFFER limit and if there are any free buffers avail */
	if ( ((buffer = remove_buffer_free()) != NULL) || buffer_count < max_buffers)
		goto alloc_buffer;

        buftrace(printf("need to purge a buffer from the lru list\n"););
        struct list_head *list, *safe;
        int count = 0;

        list_for_each_safe(list, safe, &lru_buffers) {
                struct buffer *buffer_evict = list_entry(list, struct buffer, list);	
                if (buffer_evict->count == 0 && !buffer_dirty(buffer_evict)) {
                        remove_buffer_lru(buffer_evict);
			remove_buffer_hash(buffer_evict); /* remove from hashlist */
			add_buffer_free(buffer_evict);
                        if(++count == max_free_buffers)
                                break;
                }
        }
	buffer = remove_buffer_free();
		
alloc_buffer:	
	if (!buffer) {
		buftrace(warn("allocating a new buffer"););
		if (buffer_count == max_buffers) {
			warn("Number of dirty buffers: %d", dirty_buffer_count);
			return NULL;
		}
		buffer = (struct buffer *)malloc(sizeof(struct buffer));
		if (!buffer)
			return NULL;
		int error = 0;
		if ((error = posix_memalign((void **)&(buffer->data), (1 << SECTOR_BITS), size))) {
			warn("Error: %s unable to allocate space for buffer data", strerror(error));
			free(buffer);
			return NULL;
		}
	}
 	
	buffer->count = 1;
	buffer->flags = 0; 
	buffer->size = size;
	buffer->sector = sector;
	/* insert into LRU list */
	add_buffer_lru(buffer);

	return buffer;
}

struct buffer *getblk(unsigned fd, sector_t sector, unsigned size)
{
	struct buffer **bucket = &buffer_table[buffer_hash(sector)], *buffer;

	for (buffer = *bucket; buffer; buffer = buffer->hashlist)
		if (buffer->sector == sector) {
			buftrace(printf("Found buffer for %llx\n", sector););
 			buffer->count++;
			list_del(&buffer->list);
			list_add_tail(&buffer->list, &lru_buffers);	
			return buffer;
		}
	if (!(buffer = new_buffer(sector, size)))
		return NULL;
	buffer->fd = fd;
	buffer->hashlist = *bucket;
	*bucket = buffer;
	return buffer;
}

struct buffer *bread(unsigned fd, sector_t sector, unsigned size)
{
	int err = 0;
	struct buffer *buffer; 
       
	if (!(buffer = getblk(fd, sector, size))) 
		return NULL;
	if (buffer_uptodate(buffer) || buffer_dirty(buffer))
		return buffer;
	if ((err = read_buffer(buffer))) {
		warn("error: %s unable to read sector %llx", strerror(-err), sector);
		brelse(buffer);
		return NULL;
	}
	if (!buffer_uptodate(buffer)) { // redundant with the read_buffer check?
		brelse(buffer);
		warn("bad read");
		return NULL;
	}
	return buffer;
}

void evict_buffer(struct buffer *buffer)
{
	remove_buffer_lru(buffer);
        if (!remove_buffer_hash(buffer)) 
		warn("buffer not found in hashlist");
	buftrace(printf("Evicted buffer for %llx\n", buffer->sector););
	add_buffer_free(buffer);
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

int allocate_buffers(unsigned bufsize) {
	struct buffer *buffers = (struct buffer *)malloc(max_buffers*sizeof(struct buffer));
	unsigned char *data_pool = NULL;
	int i, error = -ENOMEM; /* if malloc fails */

	buftrace(warn("Pre-allocating buffers..."););
	if (!buffers)
		goto buffers_allocation_failure;
	buftrace(warn("Pre-allocating data for buffers..."););
	if ((error = posix_memalign((void **)&data_pool, (1 << SECTOR_BITS), max_buffers*bufsize)))
		goto data_allocation_failure;
		
	/* let's clear out the buffer array and data */
	memset(buffers, 0, max_buffers*sizeof(struct buffer));
	memset(data_pool, 0, max_buffers*bufsize);

	for(i = 0; i < max_buffers; i++) {
		buffers[i].data = (data_pool + i*bufsize);
		add_buffer_free(&buffers[i]);
	}

	return 0; /* sucess on pre-allocation of buffers */
	
data_allocation_failure:
	/* go back to on demand allocation */
	warn("Error: %s unable to allocate space for buffer data", strerror(error));
	free(buffers);
buffers_allocation_failure:
	warn("Unable to pre-allocate buffers. Using on demand allocation for buffers");
	return error;	
}

/* mem_pool_size defines "roughly" the amount of memory allocated for
 * buffers. I use the term "roughly" since it doesn't take into 
 * consideration the size of the buffer struct and the overhead for 
 * posix_memalign(). From empirical tests, the additional memory
 * is negligible.
 */

void init_buffers(unsigned bufsize, unsigned mem_pool_size)
{
	assert(bufsize);
	memset(buffer_table, 0, sizeof(buffer_table));
	INIT_LIST_HEAD(&dirty_buffers);
	dirty_buffer_count = 0;
	INIT_LIST_HEAD(&lru_buffers);
	buffer_count = 0;
	INIT_LIST_HEAD(&free_buffers);

	/* calculate number of max buffers to a fixed size, independent of chunk size */
	max_buffers = mem_pool_size / bufsize;	
	max_free_buffers = max_buffers / 10;		
	
#ifdef _PRE_ALLOCATE_BUFFERS
	allocate_buffers(bufsize);
#endif
}
