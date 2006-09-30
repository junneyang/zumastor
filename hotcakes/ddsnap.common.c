#include <stdlib.h>
#include <unistd.h> /* for ddnsap.h */
#include "ddsnap.h"
#include "ddsnap.common.h"


#define CHUNK_ARRAY_INIT 1024


struct change_list *init_change_list(u32 chunksize_bits)
{
	struct change_list *cl;

	if ((cl = malloc(sizeof(struct change_list))) == NULL)
		return NULL;

	cl->count = 0;
	cl->length = CHUNK_ARRAY_INIT;
	cl->chunksize_bits = chunksize_bits;
	cl->chunks = malloc(cl->length * sizeof(u64));

	if (cl->chunks == NULL) {
		free(cl);
		return NULL;
	}

	return cl;
}

int append_change_list(struct change_list *cl, u64 chunkaddr)
{
	if ((cl->count + 1) >= cl->length) {
		u64 *newchunks;

		/* we could use a different strategy here like incrementing but this
		 * should work and will avoid having too many
		 * reallocations
		 */
		if ((newchunks = realloc(cl->chunks, cl->length * 2 * sizeof(u64))) == NULL)
		    return -1;

		cl->length *= 2;
		cl->chunks = newchunks;
	}

	cl->chunks[cl->count] = chunkaddr;
	cl->count++;

	return 0;
}

void free_change_list(struct change_list *cl)
{
	if (cl->chunks)
		free(cl->chunks);
	free(cl);
}

