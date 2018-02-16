/* 
 * This file is responsible for handling communication between Hosts and
 * Endpoints (RPI) 
 * It uses TCP for communication and uses heartbeats to detect health of 
 * connections
 * For hosts, event handler runs in a seperate thread created by this module
 *
 * Currently, as libevent is not set up to be multithreaded, one host/ep can
 * only use one comm_handle
 */

/*
 * TODO: Use newer libevent library
 * TODO: Seperate initialization and looping for ep
 * TODO: When can't send data (because of any error), stop the connection
 * TODO: Essentially RPC, But data representation needs to be independent of
 * machine types (RPI and Intel - differences?)
 * FIXME: If no connections on host established with eps, need to fail
 * TODO: Make API more informative ->
 * E.g. Allow host to know how many EPs connected presently,
 * Indicate how many EPs it was able to send message to etc
 * Is this difficult? As multithreaded, so locks or message passing?
 * Go memcached type - Use message passing for waking up and locks for data
 * communication??
 * TODO: Numbering of packets??
 * FIXME: What if host comes up before EP?
 * TODO: Re-establish connection after connection broken
 * 	- Can't do anything if EP/Host is simply stuck
 * TODO: Break down API into host and ep (seperate)
 * TODO: Support corrupted packets detection? MD5?
 * TODO: Use buffered ev and new libevent library
 */

/* 
 * XXX: Endpoint doesn't seem to need to detect health of system. Does it?
 */

#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <err.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/types.h>

#include "list.h"

#include "comm.h"

/* Libeevent */
#include <event2/thread.h>
#include <event2/event.h>

/* In host, need a seperate thread for event loop */
#include <pthread.h>

/* List of hosts and endpoints */
nodes_t hosts[NUM_HOSTS] = HOST_NODES_LIST;
nodes_t eps[NUM_EPS] = EP_NODES_LIST;

/* Detects if the current node is host/ep */
static bool is_node_host(void)
{
	/* The hostname indicates whether the node is host or ep */
	char name[MAX_NODE_NAME + 1];
	char prefix[] = HOST_NAME_PREFIX;
	int i, len;

	/* Not supporting too long names */
	assert(gethostname(name, MAX_NODE_NAME + 1) == 0);

	len = strlen(prefix);

	assert(len <= MAX_NODE_NAME);

	/* Check for prefix */
	for (i = 0; i < len; i++) {
		if (name[i] != prefix[i])
			return false;
	}

	return true;
}


static int set_nonblock(int fd)
{
	int flags;
	int ret;
	flags = fcntl(fd, F_GETFL);
	if (flags < 0)
		return flags;

	flags |= O_NONBLOCK;
	ret = fcntl(fd, F_SETFL, flags);
	if (ret < 0) {
		warn("ERROR: Couldn't set fd to nonblocking");
		return ret;
	}

        return 0;
}

/* Get ip address in string format */
static int get_ip_addr(struct sockaddr_storage *addr, char *dest_ip, int dest_len)
{
	struct sockaddr_in *s;

	/* Check which family is it */
	if (addr->ss_family != AF_INET) {
		/* We are only using ipv4 currently */
		warn("Warning: Ip is ipv6, we support only ipv4");
		return -EINVAL;
	}

	s = (struct sockaddr_in *)addr;
	if (inet_ntop(AF_INET, &s->sin_addr,
			dest_ip, dest_len) == NULL) {
		warn("Warning: Couldn't get ipaddr");
		return -EINVAL; 
	}

	return 0;
}

/* Used by host to send msg to all the eps */
int host_send_msg(comm_handle_t *handle, char *buf, size_t len)
{
	/* Write to write end of pipe */
	ssize_t ret;

	if (len > MAX_DATA_LEN) {
		warn("WARNING: Data too long to send: %zu", len);
		return -EINVAL;
	}

	ret = write(handle->host_write_fd, buf, len);
	if (ret < 0) {
		warn("WARNING: Couldn't write data to pipe");
		return ret;
	}
	
	if ((size_t)ret != len) {
		warn("WARNING: Couldn't write the whole data");
		return -EINVAL;
	}

	return 0;
}

/* Runs the event loop in seperate thread */
static void *host_event_loop(void *arg)
{
	comm_handle_t *handle = (comm_handle_t *)arg;

	event_base_dispatch(handle->ev_base);

	/* Should never return */
	assert(0);

	return NULL;
}

/* Prepares incoming data from host to be sent to eps */
static void host_incoming_data(int fd, short ev, void *arg)
{
	comm_handle_t *handle = (comm_handle_t *)arg;
	pending_data_t *data;
	int i, j, ret;

	(void)ev;

	/* Read data */
	data = malloc(sizeof(pending_data_t));
	if (data == NULL) {
		warn("WARNING: Out of memory");
		return;
	}

	ret = read(fd, data->buf, sizeof(data->buf));
	if (ret < 0) {
		warn("WARNING: Couldn't read from pipe");
	}

	data->len = ret;
	data->ref = 0;

	for (i = 0; i < NUM_EPS; i++) {
		for (j = 0; j < NUM_SWITCHES; j++) {

			int is_empty;

			host_data_t *host_data = &handle->host_data[i][j];
			
			if (!host_data->is_connected)
				continue;

			is_empty = list_size(&host_data->comm_data_list) == 0;

			ret = list_append(&host_data->comm_data_list, (void *)data);
			if (ret == false) {
				warn("Warning: Couldn't send data to ep: %d", i);
				continue;
			}

			if (is_empty) {
				event_add(host_data->ev_write, NULL);
			}

			data->ref++;
		}
	}
	
}

/* Send pending data to ep */
static void host_send_to_ep(int fd, short ev, void *arg)
{
	host_data_t *host_data = (host_data_t *)arg;
	pending_data_t *data;
	comm_data_t comm_data;
	int ret;

	(void)ev;

	data = list_pop_head(&host_data->comm_data_list);
	assert(data != NULL);

	memcpy(comm_data.buf, data->buf, data->len);
	comm_data.msg_len = data->len;
	comm_data.msg_type = MSG_DATA;

	/* Ignore Sigpipe */
	ret = send(fd, (char *)&comm_data, sizeof(comm_data_t), MSG_NOSIGNAL);
	
	data->ref--;

	if (data->ref == 0)
		free(data);

	if (ret < 0) {
		warn("Warning: Couldn't send data to ep");
		host_data->is_connected = false;
		goto err;
	}

	if (ret != sizeof(comm_data_t)) {
		warn("Warning: Sent corrupted data packet to ep");
		goto err;
	}

	if (list_size(&host_data->comm_data_list) == 0) {
		event_del(host_data->ev_write);
	}

	return;
err:
	host_data->is_connected = false;
	event_del(host_data->ev_write);
	event_free(host_data->ev_write);
}


/* Function used by list package to free up data when deleting list */
void ep_list_free(void *arg)
{
	pending_data_t *data = (pending_data_t *)arg;

	data->ref--;
	if (data->ref == 0)
		free(data);
}

/* Initialize the host. Return negative code on error */
static int host_init(comm_handle_t *handle)
{
	/* 
	 * TODO: EP might come up later than host, so might be good idea
	 * to wait for some time and then retry upto some limit
	 */

	/* Create a pipe to comm with the new thread being spawned */
	int i, j, ret, pipefd[2];

	ret = pipe(pipefd);
	if (ret < 0) {
		warn("ERROR: Couldn't open pipes: %d", ret);
		return ret;
	}

	handle->host_write_fd = pipefd[1]; /* Write end of pipe */

	ret = set_nonblock(pipefd[0]); /* Read end */
	if (ret < 0)
		goto nb_err;

	handle->ev_outstanding_data = event_new(handle->ev_base, pipefd[0],
						EV_READ | EV_PERSIST,
						host_incoming_data, handle);

	event_add(handle->ev_outstanding_data, NULL);

	/* Initialization */
	for (i = 0; i < NUM_EPS; i++) {
		for (j = 0; j < NUM_SWITCHES; j++) {
			handle->host_data[i][j].ep_num = i;
			handle->host_data[i][j].ep_fd = -1;
			handle->host_data[i][j].is_connected = false;
			list_new(&handle->host_data[i][j].comm_data_list,
					ep_list_free);
		}
	}

	/* Try to connect with the eps */
	for (i = 0; i < NUM_EPS; i++) {
		for (j = 0; j < NUM_SWITCHES; j++) {

			struct hostent *server;
			struct sockaddr_in serveraddr;
			int *sockfd = &handle->host_data[i][j].ep_fd;
			char *ep_name = eps[i].ip[j];
				
	    		*sockfd = socket(AF_INET, SOCK_STREAM, 0);
    			if (*sockfd < 0) {
        			warn("WARNING: Couldn't open socket with ep %s",
					ep_name);
				continue;
			}

			server = gethostbyname(ep_name);
			if (server == NULL) {
				warn("WARNING: Issue in EPs ipaddr: %s", ep_name);
				close(*sockfd);
				*sockfd = -1;
				continue;
			}

			/* build the server's Internet address */
			bzero((char *) &serveraddr, sizeof(serveraddr));
			serveraddr.sin_family = AF_INET;
			bcopy((char *)server->h_addr, 
				(char *)&serveraddr.sin_addr.s_addr,
				server->h_length);
			serveraddr.sin_port = htons((unsigned short)EP_LISTEN_PORT);

			/* connect: create a connection with the server */
    			ret = connect(*sockfd, (struct sockaddr *)&serveraddr,
					sizeof(serveraddr));
			if (ret < 0) { 
      				warn("WARNING: Couldn't connect to ep: %s",
						ep_name);
				*sockfd = -1;
				close(*sockfd);
				continue;
			}

			handle->host_data[i][j].ev_write =
				event_new(handle->ev_base, *sockfd,
						EV_WRITE|EV_PERSIST, host_send_to_ep, 
						&handle->host_data[i][j]);

			handle->host_data[i][j].is_connected = true;

			/* Not adding event here, wait for any data to come first */


		}
	}

	/* start a new thread that will handle the event loop */
	ret = pthread_create(&handle->host_event_thread, NULL,
			host_event_loop, (void *)handle);
	if (ret < 0) {
		warn("ERROR: Couldn't start a new event handler thread: %d", ret);
		goto sock_err;
	}

	return 0;

sock_err:
	for (i = 0; i < NUM_EPS; i++) {
		for (j = 0; j < NUM_SWITCHES; j++) {
			if (handle->host_data[i][j].ep_fd == -1)
				continue;
			close(handle->host_data[i][j].ep_fd);
		}
	}
nb_err:
	close(pipefd[0]);
	close(pipefd[1]);
	return ret;
}

/*
 * This function will be called by libevent when there is a pending heartbeat to
 * be sent by the end point
 */
void ep_write(int fd, short ev, void *arg)
{
	comm_data_t comm_data;
	size_t len, wlen;
	ep_data_t *ep_data = (ep_data_t *)arg;

	(void)ev;

	comm_data.msg_type = MSG_HEARTBEAT_RESP;
	comm_data.msg_len = 0;

	len = (uintptr_t)&comm_data.buf[0] - (uintptr_t)&comm_data;
	wlen = write(fd, (char *)&comm_data, len);

        if (wlen < len) {
		warn("Warning: Couldn't send heartbeat: %d", ep_data->host_num);
	}	
}

/*
 * This function will be called by libevent when there is a pending data to
 * be read by end point on existing connection
 */
void ep_read(int fd, short ev, void *arg)
{
	ep_data_t *ep_data = (ep_data_t *)arg;
	comm_handle_t *handle = ep_data->ep_handle;
	comm_data_t comm_data;
	size_t min_len;
	ssize_t len;

	(void)ev;

	/* We need atleast certain len */
	min_len = (uintptr_t)&comm_data.buf[0] - (uintptr_t)&comm_data;

        len = read(fd, (char *)&comm_data, sizeof(comm_data));
	if (len == 0) {
		/* Host disconnected */
		fprintf(stderr, "Warning: Host connection terminated: %d\n",
			ep_data->host_num);
                close(fd);
		event_del(ep_data->ev_read);
		event_free(ep_data->ev_read);
		free(ep_data);
		return;

	} else if (len < 0) {
		/* Some other error occurred */
		warn("Warning: Socket failure, disconnecting host: %d",
			ep_data->host_num);
		close(fd);
		event_del(ep_data->ev_read);
		event_free(ep_data->ev_read);
		free(ep_data);
		return;
	} else if ((size_t)len < min_len) {
		warn("Warning: Invalid packet data");
		return;
	}

	if (comm_data.msg_type == MSG_HEARTBEAT_REQ) {

		assert(comm_data.msg_len == 0);

		/* Send the heartbeat when ready */
		ep_data->ev_write = event_new(handle->ev_base, fd, EV_WRITE,
						ep_write, ep_data);

		event_add(ep_data->ev_write, NULL);

		return;

	} else if (comm_data.msg_type == MSG_DATA) {
		/* Call the callback indicating reception of data */
		ep_data->ep_handle->ep_callback(ep_data->host_num,
						 comm_data.buf,
						 comm_data.msg_len);

		return;
	} else {
		warn("Warning: Invalid message type: %d", comm_data.msg_type);
		assert(0);
		return;
	}

}

/*
 * This function will be called by libevent when there is a connection
 * ready to be accepted by end point
 */
static void ep_accept(int fd, short ev, void *arg)
{

	int hfd;
	struct sockaddr_storage host_addr;
	socklen_t addr_len = sizeof(host_addr);
	ep_data_t *ep_data;
	int ret;
	char ipstr[INET_ADDRSTRLEN];
	int i, j;

	(void)ev;

	/* Accept the new connection. */
	hfd = accept(fd, (struct sockaddr *)&host_addr, &addr_len);
	if (fd < 0) {
		warn("Warning: Accept failed: %d", hfd);
		return;
	}

	ep_data = malloc(sizeof(ep_data_t));
	if (ep_data == NULL) {
		warn("Warning: Out of memory");
		goto err;
	}

	ret = get_ip_addr(&host_addr, ipstr, sizeof(ipstr));
	if (ret < 0)
		goto err;

	/* Check which host is it by comparing ips */
	ep_data->ep_handle = (comm_handle_t *)arg;
	ep_data->host_num = -1;
	for (i = 0; i < NUM_HOSTS; i++) {
		for (j = 0; j < NUM_SWITCHES; j++) {
			if (strcmp(hosts[i].ip[j], ipstr) == 0) {
				ep_data->host_num = i;
				break;
			}
		}
		if (ep_data->host_num != -1)
			break;
	}

	if (ep_data->host_num == -1) {
		warn("Warning: Unknow host contacted endpoint: %s", ipstr);
		goto err;
	}

	/* Set the client socket to non-blocking mode. */
	ret = set_nonblock(hfd);
	if (ret < 0) {
		warn("Warning: Failed to set host socket non-blocking: %d", ret);
		goto err;
	}


	/* 
	 * Setup the read event, libevent will call ep_read() whenever
	 * the clients socket becomes read ready.  We also make the
	 * read event persistent so we don't have to re-add after each
	 * read. 
	 */
	ep_data->ev_read = event_new(ep_data->ep_handle->ev_base,
					hfd, EV_READ|EV_PERSIST, ep_read, 
	    				ep_data);

	event_add(ep_data->ev_read, NULL);

	return;

err:
	/* Close the socket. Let the host deal with RST packet. */
	close(hfd);
}

/* Initialize the host. Return negative code on error */
static int ep_init(comm_handle_t *handle)
{
	int fd; /* listening socket */
	int optval; /* flag value for setsockopt */
	struct sockaddr_in epaddr; /* ep's addr */
	int ret;
	
	/*
	 * Setup a port, start listening on it and call the callback whenever
	 * data arrives or respond to the heartbeats
	 */
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		warn("ERROR: Couldn't open socket for endpoint");
		return fd;
	}

	/* 
	 * setsockopt: Handy debugging trick that lets 
	 * us rerun the server immediately after we kill it; 
	 * otherwise we have to wait about 20 secs. 
	 * Eliminates "ERROR on binding: Address already in use" error. 
	 */
	optval = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
		(const void *)&optval , sizeof(int));

	/*
	 * build the server's Internet address
	 */
	bzero((char *)&epaddr, sizeof(epaddr));
	epaddr.sin_family = AF_INET;

	/* 
	 * let the system figure out our IP address
	 * (We will be listening on multiple IPs)
	 */
	epaddr.sin_addr.s_addr = htonl(INADDR_ANY);

	epaddr.sin_port = htons((unsigned short)EP_LISTEN_PORT);

	ret = bind(fd, (struct sockaddr *)&epaddr, 
	   		sizeof(epaddr));
	if (ret < 0) {
    		warn("ERROR: Endpoint couldn't bind to port: %d",
				EP_LISTEN_PORT);
		goto err;
	}

	/* 
	 * listen: make this socket ready to accept connection requests 
	 */
  	ret = listen(fd, EP_LISTEN_QUEUE_SIZE);
	if (ret < 0) {
	    warn("ERROR: Endpoint couldn't listen on port: %d",
			    EP_LISTEN_PORT);
	    goto err;
	}

	ret = set_nonblock(fd);
	if (ret < 0)
		goto err;

	/*
	 * We now have a listening socket, we create a read event to
	 * be notified when a host connects
	 */
	handle->ev_accept = event_new(handle->ev_base, fd,
					EV_READ | EV_PERSIST,
					ep_accept, handle);
	
	event_add(handle->ev_accept, NULL);

	/* Start the libevent event loop. */
	event_base_dispatch(handle->ev_base);

err:
	close(fd);
	return ret;
}

/* Initializes the module. Return negative code on error */
int comm_init(comm_handle_t *handle, comm_ep_callback_t ep_callback)
{
	/* Initialize the event lib */
	int ret = evthread_use_pthreads();
	if (ret < 0 || (handle->ev_base = event_base_new()) == NULL) {
		fprintf(stderr, "Error: Libevent initialization failed\n");
		return -EINVAL;
	}

	handle->is_host = is_node_host();
	handle->ep_callback = ep_callback;

	if (handle->is_host) {

		if (ep_callback != NULL) {
			warn("Error: Callback mentioned for a host node");
			return -EINVAL;
		}
		
		return host_init(handle);
	} else {

		if (ep_callback == NULL) {
			
			warn("Error: No callback for endpoint node");
			return -EINVAL;
		}

		return ep_init(handle);
	}
}
