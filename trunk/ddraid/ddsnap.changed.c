#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include "ddsnap.h"
#include "../dm-ddsnap.h"
#include "trace.h"
#include "sock.h"

#define THIS_CODE  GENERATE_CHANGE_LIST
#define THIS_REPLY REPLY_GENERATE_CHANGE_LIST

int main(int argc, char *argv[])
{
	int change_fd, sock, err;
	struct head head;
	unsigned maxbuf = 500;
	char buf[maxbuf];

	if (argc < 5)
		error("usage: %s host:port changelist snapshot1 snapshot2", argv[0]);

	int snap1 = atoi(argv[3]);
	int snap2 = atoi(argv[4]);
	char *host = argv[1], * changelist_filename = argv[2];
	int len = strlen(host), port = parse_port(host, &len);

	if (port < 0)
		error("expected host:port, not %s", host);
	host[len] = 0;

/*	if( (change_fd = open(changelist_filename, O_CREAT | O_TRUNC)) < 0) 
		error("Unable to create changelist file: %s",changelist_filename);
*/			
	if (!(sock = open_socket(host, port)))
		error("Can't connect to %s:%i", host, port);

	if (outbead(sock, THIS_CODE, struct generate_changelist, snap1, snap2) < 0)
		goto eek;

	/* hack */
	len = strlen(changelist_filename);
	writepipe(sock, &len, sizeof(len));
	writepipe(sock, changelist_filename, len);
/*	
	if (send_fd(sock, change_fd, "bogus", 5) < 0) 
		goto eek;
*/
	if ((err = readpipe(sock, &head, sizeof(head)))) 
		goto eek;

	assert(head.length < maxbuf); // !!! don't die
	if ((err = readpipe(sock, buf, head.length))) 
		goto eek;

	trace_on(printf("reply = %x\n", head.code);)
	err  = head.code != THIS_REPLY;
/*
	close(change_fd);
*/
	if (head.code == REPLY_ERROR)
		error("%.*s", head.length - 4, buf + 4);

	return 0;

eek:
	error("%s (%i)", strerror(errno), errno);
	return 0;
}
