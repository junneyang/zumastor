#include <stdio.h>
#include <stdlib.h>
#include <errno.h> 
#include <unistd.h> // read
#include <sys/socket.h>
#include <sys/poll.h>
#include <sys/un.h>
#include <netinet/in.h>
//#include <libdlm.h>
#include "../dm-ddsnap.h" // message codes
#include "ddsnapd.h" // outbead
#include "trace.h"
#include "sock.h" // send_fd, read/writepipe, connect_socket

#define trace trace_off

struct client { int sock; enum { CLIENT_CON, SERVER_CON } type; };

struct context {
	struct server active, local;
	int serv;
	int waiters; 
	struct client *waiting[100];
	int polldelay;
	unsigned ast_state;
};

static inline int have_address(struct server *server)
{
	return 1;
	/*return !!server->address_len;*/
}

int connect_clients(struct context *context)
{
	warn("connect clients to %x", *(int *)(context->active.address));
	while (context->waiters)
	{
		struct client *client = context->waiting[0];
		int control = client->sock;
		struct server *server = &context->active;
		struct sockaddr_un addr = { .sun_family = server->type };
		int addr_len = sizeof(addr) - sizeof(addr.sun_path) + strlen(server->address);
		int sock;

		if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
			error("Can't get socket");
		trace_on(printf("server address: %s\n", server->address));
		strncpy(addr.sun_path, server->address, sizeof(addr.sun_path));
		if (connect(sock, (struct sockaddr *)&addr, addr_len) == -1) {
			// this really sucks: if the address is wrong, we silently wait for
			// some ridiculous amount of time.  Do something about this please.
			warn("Can't connect to server");
			return -1;
		}
		if (outbead(control, CONNECT_SERVER, struct { }) < 0)
			error("Could not send connect message");
		if (send_fd(control, sock, "fark", 4) < 0)
			error("Could not pass server connection to target");
		context->waiting[0] = context->waiting[--context->waiters];
	}
	return 0;
}

int try_to_instantiate(struct context *context)
{
	warn("Activate local server");
	memcpy(&context->active, &context->local, sizeof(struct server));
	if (outbead(context->serv, START_SERVER, struct { }) < 0) 
		error("Could not send message to server");
	connect_clients(context);
	return 0;
}

int incoming(struct context *context, struct client *client)
{
	int err;
	struct messagebuf message;
	int sock = client->sock;

	if ((err = readpipe(sock, &message.head, sizeof(message.head))))
		goto pipe_error;
	if (message.head.length > maxbody)
		goto message_too_long;
	if ((err = readpipe(sock, &message.body, message.head.length)))
		goto pipe_error;

	switch (message.head.code) {
	case SERVER_READY:
		warn("received server ready");
		assert(message.head.length == sizeof(struct server));
		memcpy(&context->local, message.body, sizeof(struct server));
		context->serv = sock; // !!! refuse more than one
		client->type = SERVER_CON;
		goto instantiate;

	case NEED_SERVER:
		context->waiting[context->waiters++] = client;
		/*
		 * If we have a local server, try to instantiate it as the master.
		 * If there's already a master out there, connect to it.  If there
		 * was a master but it went away then the exclusive lock is up for
		 * grabs.  Always ensure the exclusive is still there before by
		 * trying to get it, before relying on the lvb server address,
		 * because that could be stale.
		 *
		 * If there's no local server, don't do anything: instantiation
		 * will be attempted when/if the local server shows up.
		 */
		trace_on(printf("NEED SERVER is being called\n"));
		if (have_address(&context->active)) {
			trace_on(printf("Calling connect_clients\n"));
			connect_clients(context);
			break;
		}
		break;
	case REPLY_CONNECT_SERVER:
		warn("Everything connected properly, all is well");
		break;
	default: 
		warn("Unknown message %x", message.head.code);
		break;
	}
	return 0;

instantiate:
	return try_to_instantiate(context);

message_too_long:
	warn("message %x too long (%u bytes)\n", message.head.code, message.head.length);
pipe_error:
	return -1;
}

int monitor(char *sockname, struct context *context)
{
	unsigned maxclients = 100, clients = 0, others = 2;
	struct pollfd pollvec[others+maxclients];
	struct client *clientvec[maxclients];
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int addr_len = sizeof(addr) - sizeof(addr.sun_path) + strlen(sockname);
	int listener = socket(AF_UNIX, SOCK_STREAM, 0); //, locksock;

	assert(listener > 0);
	strncpy(addr.sun_path, sockname, sizeof(addr.sun_path));
/*	if (sockname[0] == '@')
		addr.sun_path[0] = 0;
		else */
	unlink(sockname);

	if (bind(listener, (struct sockaddr *)&addr, addr_len) || listen(listener, 5))
		error("Can't bind to control socket (is it in use?)");

#if 0
	/* Launch daemon and exit */
	switch (fork()) {
	case -1:
		error("fork failed");
	case 0:
		break; // !!! should daemonize properly
	default:
		return 0;
	}
#endif

	pollvec[0] = (struct pollfd){ .fd = listener, .events = POLLIN };
	//pollvec[1] = (struct pollfd){ .fd = locksock, .events = POLLIN };
	assert(pollvec[0].fd > 0);

	while (1) {
		switch (poll(pollvec, others+clients, context->polldelay)) {
		case -1:
			if (errno == EINTR)
				continue;
			error("poll failed, %s", strerror(errno));
		case 0:
			/* Timeouts happen here */
			context->polldelay = -1;
			warn("try again");
			connect_clients(context);
			// If we go through this too many times it means somebody
			// out there is sitting on the PW lock but did not write
			// the lvb, this is breakage that should be reported to a
			// human.  So we should do that, but also keep looping
			// forever in case somebody is just being slow or in the
			// process of being fenced/ejected, in which case the PW
			// will eventually come free again.  Yes this sucks.
			continue;
		}

		/* New connection? */
		if (pollvec[0].revents) {
			struct sockaddr_in addr;
			int addr_len = sizeof(addr), sock;

			if (!(sock = accept(listener, (struct sockaddr *)&addr, &addr_len)))
				error("Cannot accept connection");
			trace_on(warn("Received connection %i", clients);)
			assert(clients < maxclients); // !!! make the array bigger

			struct client *client = malloc(sizeof(struct client));
			*client = (struct client){ .sock = sock };
			clientvec[clients] = client;
			pollvec[others+clients] = (struct pollfd){ .fd = sock, .events = POLLIN };
			clients++;
		}
		/* Activity on connection? */
		unsigned i = 0;
		while (i < clients) {
			if (pollvec[others+i].revents) { // !!! check for poll error
				struct client **clientp = clientvec + i, *client = *clientp;

				if (incoming(context, client) == -1) {
					warn("Lost connection %i", i);
					if (client->type == SERVER_CON) {
						warn("local server died...");
						if (!memcmp(&context->active, &context->local, sizeof(struct server))) {
							warn("release lock");
//							struct dlm_lksb *lksb = &context->lksb;
//							memset(&context->active, 0, sizeof(struct server));
//							memset(lksb->sb_lvbptr, 0, sizeof(struct server));
						}
						memset(&context->local, 0, sizeof(struct server));
					}
					close(client->sock);
					free(client);
					--clients;
					clientvec[i] = clientvec[clients];
					pollvec[others + i] = pollvec[others + clients];
//					memmove(clientp, clientp + 1, sizeof(struct client *) * clients);
//					memmove(pollvec + i + others, pollvec + i + others + 1, sizeof(struct pollfd) * clients);
					continue;
				}
			}
			i++;
		}
	}
}

int main(int argc, char *argv[])
{
	if (argc != 2)
		error("usage: %s sockname", argv[0]);

	return monitor(argv[1], &(struct context){ .polldelay = -1 });
}
