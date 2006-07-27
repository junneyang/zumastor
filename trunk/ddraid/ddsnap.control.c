#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
//#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include "ddsnap.h"
#include <time.h>
#include "../dm-ddsnap.h"
#include "trace.h"
#include "sock.h"

#ifdef DELETE
#  define THIS_CODE DELETE_SNAPSHOT
#  define THIS_REPLY REPLY_DELETE_SNAPSHOT
#elif CREATE
#  define THIS_CODE CREATE_SNAPSHOT
#  define THIS_REPLY REPLY_CREATE_SNAPSHOT
#elif LIST
#  define THIS_CODE LIST_SNAPSHOTS
#  define THIS_REPLY SNAPSHOT_LIST
#elif PRIORITY
#  define THIS_CODE SET_PRIORITY
#  define THIS_REPLY REPLY_SET_PRIORITY
#elif GENERATE
#  define THIS_CODE GENERATE_CHANGE_LIST
#  define THIS_REPLY REPLY_GENERATE_CHANGE_LIST
#endif

int main(int argc, char *argv[])
{
	int sock, err, snap, snap2;
	struct head head;
	unsigned maxbuf = 500;
	char buf[maxbuf], *host, *changelist_filename;
	struct snapinfo * buffer;

#if LIST
	if (argc < 2)
		error("usage: %s host:port",argv[0]);
#elif GENERATE
	if (argc < 5)
		error("usage: %s host:port changelist snapshot1 snapshot2", argv[0]);
	snap  = atoi(argv[3]);
	snap2 = atoi(argv[4]);
        changelist_filename = argv[2];
#else
	if (argc < 3)
		error("usage: %s host:port snapshot", argv[0]);
	snap = atoi(argv[2]);
#endif

	host = argv[1];
	int len = strlen(host), port = parse_port(host, &len);

	if (port < 0)
		error("expected host:port, not %s", host);
	host[len] = 0;

	if (!(sock = open_socket(host, port)))
		error("Can't connect to %s:%i", host, port);

#if GENERATE
	if (outbead(sock, THIS_CODE, struct generate_changelist, snap, snap2) < 0)
		goto eek;
#else
	if (outbead(sock, THIS_CODE, struct create_snapshot, snap) < 0)
		goto eek;
#endif

#if GENERATE
	/* hack */
	len = strlen(changelist_filename);
	writepipe(sock, &len, sizeof(len));
	writepipe(sock, changelist_filename, len);
#endif

	if ((err = readpipe(sock, &head, sizeof(head)))) 
		goto eek;

        /* Jane's addition */
#if PRIORITY
        if (argc < 3) {
          error("Incorrect Format: %s <snap_tag> <new_priority_value>", argv[0]);
        }
        struct snapinfo new_snap;
        new_snap.snap = atoi(argv[1]);
        new_snap.prio = atoi(argv[2]);
        writepipe(sock, &new_snap, sizeof(snapinfo)); 
#endif

#if LIST
          buffer = (struct snapinfo *) malloc (head.length-sizeof(int));
          int i, count;

          readpipe(sock, &count, sizeof(int));
          readpipe(sock, buffer, head.length-sizeof(int));

          printf("Snapshot list: \n");

          for (i=0; i<count; i++) {
	    time_t snap_time = (time_t)(buffer[i]).ctime;

            printf("Snapshot[%d]: \n", i);
            printf("\tsnap.tag= %Lu \t", (buffer[i]).snap);
            printf("snap.prio= %d \t", (buffer[i]).prio);
            printf("snap.ctime= %s \n", ctime(&snap_time));
          }
#else
        /* end of Jane's addition */
	
        assert(head.length < maxbuf); // !!! don't die
	if ((err = readpipe(sock, buf, head.length)))
		goto eek;
#endif		
	  
	trace_on(printf("reply = %x\n", head.code);)
	err  = head.code != THIS_REPLY;

	if (head.code == REPLY_ERROR)
		error("%.*s", head.length - 4, buf + 4);

	return 0;

eek:
	error("%s (%i)", strerror(errno), errno);
	return 0;
}
