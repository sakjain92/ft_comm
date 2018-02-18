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
 * TODO: Seperate initialization and looping for ep
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
 * TODO: Re-establish connection after connection broken
 * 	- Can't do anything if EP/Host is simply stuck
 * TODO: Break down API into host and ep (seperate)
 * TODO: Support corrupted packets detection? MD5?
 * TODO: Error handling of libevent
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
#include <stdarg.h>

#include "list.h"

#include "comm.h"

/* Libeevent */
#include <event2/thread.h>
#include <event2/event.h>
#include <event2/bufferevent.h>

/* In host, need a seperate thread for event loop */
#include <pthread.h>
#include <semaphore.h>

/* List of hosts and endpoints */
nodes_t hosts[NUM_HOSTS] = HOST_NODES_LIST;
nodes_t eps[NUM_EPS] = EP_NODES_LIST;

/* Forward declaration */
static void host_connect_cb(int sockfd, short which, void *arg);
static void host_connect_terminate_now(host_data_t *host_data);
static void host_connect_terminate_defer(host_data_t *host_data);

/* TODO: Not evertime errno is required */

/* Prints a generic error */
static void _genericLog(int logType, bool printErrno, char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);

	switch (logType) {
	case LOG_FATAL:
		fprintf(stderr, "ERROR: ");
		break;
	case LOG_WARN:
		fprintf(stderr, "WARNING: ");
		break;
	default:
		assert(0 && "Invalid logging option");
	}

	vfprintf(stderr, fmt, ap);

	if (printErrno && errno != 0)
		fprintf(stderr, ": Errno(%s)", strerror(errno));

	fprintf(stderr, "\n");
	va_end(ap);

}

#define genericLog(...)	_genericLog(__VA_ARGS__)
#define hostLog(host_data, ...) 				\
{								\
	fprintf(stderr, "EP(%d:%d): ",				\
			host_data->ep_num,			\
			host_data->ep_sw);			\
	genericLog(__VA_ARGS__);				\
}
#define epLog(ep_data, ...)	 				\
{								\
	fprintf(stderr, "HOST(%d:%d): ",			\
			ep_data->host_num,			\
			ep_data->host_sw);			\
	genericLog(__VA_ARGS__);				\
}

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
	if (flags < 0) {
		
		return flags;
	}

	flags |= O_NONBLOCK;
	ret = fcntl(fd, F_SETFL, flags);
	if (ret < 0) {
		genericLog(LOG_FATAL, true, "Couldn't set fd to nonblocking");
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
		genericLog(LOG_WARN, false, "Ip is ipv6, we support only ipv4");
		return -EINVAL;
	}

	s = (struct sockaddr_in *)addr;
	if (inet_ntop(AF_INET, &s->sin_addr,
			dest_ip, dest_len) == NULL) {
		genericLog(LOG_WARN, false, "Couldn't get ipaddr");
		return -EINVAL; 
	}

	return 0;
}

/* Host error */
static void host_err(host_data_t *host_data, int errType)
{
	comm_handle_t *handle = host_data->handle;
	assert(errType == HOST_CONNECT_FAIL ||
			errType == HOST_CONNECT_TERMINATE);
	
	if (handle->err_callback) {
		handle->err_callback(host_data->ep_num,
					host_data->ep_sw,
					errType);
	}	
}

/* EP error */
static void ep_err(ep_data_t *ep_data, int errType)
{
	comm_handle_t *handle = ep_data->ep_handle;
	assert(errType == EP_CONNECT_TERMINATE ||
			errType == EP_HEARTBEAT_FAIL ||
			errType == EP_INVALID_MSG);
	
	if (handle->err_callback)
		handle->err_callback(ep_data->host_num,
					ep_data->host_sw,
					errType);
}

/* Used by host to send msg to all the eps */
int host_send_msg(comm_handle_t *handle, char *buf, size_t len)
{
	/* Write to write end of pipe */
	comm_data_t *data;

	if (len == 0) {
		genericLog(LOG_WARN, false,
				"Doesn't support sending empty packets");
		return -EINVAL;
	}

	if (len > MAX_DATA_LEN) {
		genericLog(LOG_WARN, false, "Data too long to send: %zu", len);
		return -EINVAL;
	}

	/* Read data */
	data = malloc(sizeof(comm_data_t));
	if (data == NULL) {
		genericLog(LOG_WARN, false, "Out of memory");
		return -ENOMEM;
	}

	memcpy(data->buf, buf, len);
	data->msg_len = len;
	data->msg_type = MSG_DATA;

	pthread_mutex_lock(&handle->lock);

	if (list_append(&handle->data_list, data) != true) {
		free(data);
		genericLog(LOG_WARN, false, "Couldn't add to list");
		pthread_mutex_unlock(&handle->lock);
		return -ENOMEM;
	}

	pthread_mutex_unlock(&handle->lock);

	/* Write dummy char - Just a trigger */
	bufferevent_write(handle->host_write, HOST_TRIGGER_VAL , 1);

	return 0;
}

/* Runs the event loop in seperate thread */
static void *host_event_loop(void *arg)
{
	comm_handle_t *handle = (comm_handle_t *)arg;

	event_base_dispatch(handle->ev_base);

	/* 
	 * Termination - i.e. all connections terminated
	 * (either voluntary or due to errors)
	 * TODO: Retry connections? Then need to make sure this loop doesn't
	 * exits when all connections temporarily disconnect
	 */

	return NULL;
}

/* Called when connection is to be closed after reading/writing out all pending data */
static void host_end_connection(struct bufferevent *bev, void *arg)
{
	host_data_t *host_data = (host_data_t *)arg;
	
	bufferevent_free(bev);

	/* Voluntary termination - So no error */

	(void)host_data;
	/* host_err(host_data, HOST_CONNECT_TERMINATE); */
}

static void host_end_connection_event(struct bufferevent *bev, short event, void *arg)
{
	(void)event;
	host_end_connection(bev, arg);
}

/* Prepares incoming data from host to be sent to eps */
static void host_incoming_data(struct bufferevent *bev, void *arg)
{
	comm_handle_t *handle = (comm_handle_t *)arg;
	comm_data_t *data;
	int i, j, ret, len;
	char ch;

	while (1) {

		ret = bufferevent_read(bev, &ch, 1);
		if (ret == 0)
			return;

		if (ch == HOST_TRIGGER_VAL[0]) {

			pthread_mutex_lock(&handle->lock);
			data = list_pop_head(&handle->data_list);
			assert(data != NULL);
			pthread_mutex_unlock(&handle->lock);

			data->session = handle->session;
			data->msg_num = handle->num_msg_sent;

			for (i = 0; i < NUM_EPS; i++) {
				for (j = 0; j < NUM_SWITCHES; j++) {
		
					host_data_t *host_data =
						&handle->host_data[i][j];
					
					if (!host_data->is_connected)
						continue;

					/* 
					 * XXX: Do we wish to keep the data lying
					 * around when an ep temporarily is not
					 * connected so that we can sent it later
					 */
					len = (uintptr_t)&data->buf[data->msg_len] -
						(uintptr_t)data;

					ret = bufferevent_write(host_data->bev_write,
								(char *)data, len);

					if (ret < 0) {
						hostLog(host_data, LOG_WARN, false,  
							"Sent corrupt data");
					
						host_connect_terminate_now(host_data);
					}
				}
			}

			free(data);

			handle->num_msg_sent++;

		} else if (ch == HOST_END_VAL[0]) {

			for (i = 0; i < NUM_EPS; i++) {
				for (j = 0; j < NUM_SWITCHES; j++) {
		
					host_data_t *host_data = &handle->host_data[i][j];
					
					if (!host_data->is_connected)
						continue;

					host_connect_terminate_defer(host_data);
				}
			}

		} else {
			assert(0 && "Unknown message");
		}
	}	
}

/* Called when host gets heartbeats */
static void host_got_heartbeat(struct bufferevent *bev, void *arg)
{
	(void)bev;
	(void)arg;
}


/* Function used by list package to free up data when deleting list */
static void data_free_fn(void *arg)
{
	comm_data_t *data = (comm_data_t *)arg;
	free(data);
}

/*
 * This function is called on any error while host is writing to ep
 */
static void host_event(struct bufferevent *bev, short event, void *arg)
{
	host_data_t *host_data = (host_data_t *)arg;

	(void)bev;

    	if (event & BEV_EVENT_EOF) {
		/* EP disconnected */
		host_connect_terminate_now(host_data);
		hostLog(host_data, LOG_WARN, false,
				"EP connection terminated");

	} else if (event & BEV_EVENT_ERROR || event & BEV_EVENT_WRITING) {
		/* Some other error occurred */
		host_connect_terminate_now(host_data);
		hostLog(host_data, LOG_WARN, false,
				"Socket failure, disconnecting ep");
	} else {
		host_connect_terminate_now(host_data);
		hostLog(host_data, LOG_WARN, false,
				"Unknown error on socket, disconnecting ep");
	}

	return;
}

/* 
 * Called when connection is to be terminated after sending pending data 
 * This is voluntary closing of connection
 */
static void host_connect_terminate_defer(host_data_t *host_data)
{
	comm_handle_t *handle = host_data->handle;
	struct evbuffer *output = bufferevent_get_output(host_data->bev_write);
	size_t len = evbuffer_get_length(output);

	host_data->is_connected = false;

	if (len == 0) {
		bufferevent_free(host_data->bev_write);
	} else {
		bufferevent_setcb(host_data->bev_write,
					host_end_connection,
					host_end_connection,
					host_end_connection_event,
					host_data);
	}

	pthread_mutex_lock(&handle->lock);
	handle->num_succ_conns--;
	pthread_mutex_unlock(&handle->lock);
}

/* Called when connection to host terminated quickly */
static void host_connect_terminate_now(host_data_t *host_data)
{
	comm_handle_t *handle = host_data->handle;

	host_data->is_connected = false;

	bufferevent_free(host_data->bev_write);

	pthread_mutex_lock(&handle->lock);
	handle->num_succ_conns--;
	pthread_mutex_unlock(&handle->lock);

	host_err(host_data, HOST_CONNECT_TERMINATE);
}


/* Tries to reconnect after failed attempt - Doesn't immedaitely reconnect */
static bool host_try_reconnect(int sockfd, host_data_t *host_data)
{
	if (--host_data->retries_left > 0) {

		struct timeval tv;
		host_data->ev_connect =
			event_new(host_data->handle->ev_base, sockfd,
					0, host_connect_cb,
					host_data);
				
		tv.tv_sec = MAX_CONN_RETRY_TIMEOUT_SEC;
		tv.tv_usec = 0;

		event_add(host_data->ev_connect, &tv);
		return true;
	}

	hostLog(host_data, LOG_WARN, false, "Couldn't connect to ep");
	host_err(host_data, HOST_CONNECT_FAIL);

	sem_post(&host_data->handle->connect_sem);

	return false;
}

/* Call this after connection estabished by host with ep */
static void host_connected(int sockfd, host_data_t *host_data)
{
	comm_handle_t *handle = host_data->handle;

	host_data->bev_write =
		bufferevent_socket_new(host_data->handle->ev_base, sockfd,
					BEV_OPT_CLOSE_ON_FREE);

	host_data->is_connected = true;
			
	bufferevent_setcb(host_data->bev_write,
				host_got_heartbeat,
				NULL,
				host_event,
				host_data);

	bufferevent_enable(host_data->bev_write,
				EV_READ | EV_WRITE);

	pthread_mutex_lock(&handle->lock);
	handle->num_succ_conns++;
	pthread_mutex_unlock(&handle->lock);

	sem_post(&host_data->handle->connect_sem);
}

/* Called when finally connect succeeded or timeout */
static void host_connect_cb(int sockfd, short which, void *arg)
{
	socklen_t optlen;
	int ret, optval;
	host_data_t *host_data = (host_data_t *)arg;

	(void)which;

	assert(host_data->is_connected == false);

	event_del(host_data->ev_connect);
	event_free(host_data->ev_connect);


	ret = getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &optval, &optlen);
	if (ret < 0) {
		genericLog(LOG_WARN, true, "getsockopt() failed");
		host_err(host_data, HOST_CONNECT_FAIL);
		return;
	}

	if (optval == 0) {
		/* connection successful */
		host_connected(sockfd, host_data);
		return;
	} else {

		host_try_reconnect(sockfd, host_data);
		return;
	}
	
}

/* Initialize the host. Return negative code on error */
static int host_init(comm_handle_t *handle)
{
	/* 
	 * TODO: EP might come up later than host, so might be good idea
	 * to wait for some time and then retry upto some limit
	 */

	/* Create a pipe to comm with the new thread being spawned */
	int i, j, ret;
	struct bufferevent *pair[2];
	int num_conn;

	srand(time(0));
	handle->num_msg_sent = 0;
	handle->session = rand();
	handle->num_succ_conns = 0;

	sem_init(&handle->connect_sem, 0, 0);

	ret = bufferevent_pair_new(handle->ev_base, BEV_OPT_CLOSE_ON_FREE, pair);
	if (ret < 0) {
		genericLog(LOG_FATAL, true, "Couldn't open pipes");
		return ret;
	}

	handle->host_write = pair[1]; /* Write end of pipe */
	handle->ev_outstanding = pair[0];

	bufferevent_setcb(handle->ev_outstanding, host_incoming_data, NULL, NULL,
			  handle);

	bufferevent_enable(handle->ev_outstanding, EV_READ);

	/* Initialization */
	for (i = 0; i < NUM_EPS; i++) {
		for (j = 0; j < NUM_SWITCHES; j++) {
			host_data_t *host_data = &handle->host_data[i][j];

			host_data->ep_num = i;
			host_data->ep_sw = j;
			host_data->is_connected = false;
			host_data->retries_left = MAX_CONN_RETRIES;
			host_data->ev_connect = NULL;
			host_data->handle = handle;
		}
	}

	/* Try to connect with the eps */
	for (i = 0; i < NUM_EPS; i++) {
		for (j = 0; j < NUM_SWITCHES; j++) {

			struct hostent *server;
			struct sockaddr_in serveraddr;
			int sockfd;
			char *ep_name = eps[i].ip[j];
			
			host_data_t *host_data = &handle->host_data[i][j];
					
	    		sockfd = socket(AF_INET, SOCK_STREAM, 0);
    			if (sockfd < 0) {
				hostLog(host_data, LOG_WARN, true,
						"Couldn't open socket with ep");
				host_err(host_data, HOST_CONNECT_FAIL);
				continue;
			}

			ret = set_nonblock(sockfd);
			if (ret < 0) {
				host_err(host_data, HOST_CONNECT_FAIL);
				close(sockfd);
				continue;
			}

			server = gethostbyname(ep_name);
			if (server == NULL) {
				hostLog(host_data, LOG_WARN, false, 
					"Issue in EPs ipaddr", ep_name);
				host_err(host_data, HOST_CONNECT_FAIL);

				close(sockfd);
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
    			ret = connect(sockfd, (struct sockaddr *)&serveraddr,
					sizeof(serveraddr));
			if (ret < 0 && errno == EINPROGRESS) {
				/* 
				 * Couldn't connect right away but will connect in
				 * future
				 */
				struct timeval tv;
				host_data->ev_connect =
					event_new(handle->ev_base, sockfd,
							EV_WRITE, host_connect_cb,
							host_data);
				
				tv.tv_sec = MAX_CONN_TIMEOUT_SEC;
				tv.tv_usec = 0;

				event_add(host_data->ev_connect, &tv);

				continue;

			} else if (ret < 0){
				/* Error on connecting. try again */
				host_try_reconnect(sockfd, host_data);
				continue;
			}

			host_connected(sockfd, host_data);
		}
	}

	list_new(&handle->data_list, data_free_fn);

	ret = pthread_mutex_init(&handle->lock, NULL);
	if (ret < 0) {
		genericLog(LOG_FATAL, true, "Mutex init failed");
		goto sock_err;
	}

	/* start a new thread that will handle the event loop */
	ret = pthread_create(&handle->host_event_thread, NULL,
				host_event_loop, (void *)handle);
	if (ret < 0) {
		genericLog(LOG_FATAL, true, 
				"Couldn't start a new event handler thread");
		goto thread_err;
	}

	/* Wait for all connections to be tried to be connected */
	for (i = 0; i < NUM_EPS * NUM_SWITCHES; i++) {
		ret = EINTR;
		while (ret == EINTR) {
			ret = sem_wait(&handle->connect_sem);
		}	
	}

	pthread_mutex_lock(&handle->lock);
	num_conn = handle->num_succ_conns;
	pthread_mutex_unlock(&handle->lock);

	if (num_conn == 0) {
		genericLog(LOG_FATAL, false, "No connections established");
		return -1;
	}

	return 0;

thread_err:
	pthread_mutex_destroy(&handle->lock);

sock_err:
	for (i = 0; i < NUM_EPS; i++) {
		for (j = 0; j < NUM_SWITCHES; j++) {
			if (handle->host_data[i][j].is_connected == false)
				continue;
			bufferevent_free(handle->host_data[i][j].bev_write);
		}
	}

	bufferevent_free(handle->ev_outstanding);
	bufferevent_free(handle->host_write);
	return ret;
}

/*
 * This function is called on any error while ep is reading/writing
 */
void ep_event(struct bufferevent *bev, short event, void *arg)
{
	ep_data_t *ep_data = (ep_data_t *)arg;

    	if (event & BEV_EVENT_EOF) {
		/* Host disconnected */
		epLog(ep_data, LOG_WARN, false,
			"Host connection terminated");

	} else if (event & BEV_EVENT_ERROR || event & BEV_EVENT_WRITING ||
			event & BEV_EVENT_READING) {
		/* Some other error occurred */
		epLog(ep_data, LOG_WARN, false,
			"Socket failure, disconnecting host");
	} else {
		epLog(ep_data, LOG_WARN, false,
			"Unknown error on socket, disconnecting host");
	} 

	ep_err(ep_data, EP_CONNECT_TERMINATE);

	/* Remove connection from the list */
	list_remove(&ep_data->ep_handle->conn_list, ep_data);
	bufferevent_free(bev);
	free(ep_data);
	return;
}

/*
 * This function will be called by libevent after heartbeat is sent to host
 */
void ep_write(struct bufferevent *bev, void *arg)
{
	(void)bev;
	(void)arg;
}

/*
 * This function will be called by libevent when there is a pending data to
 * be read by end point on existing connection
 */
void ep_read(struct bufferevent *bev, void *arg)
{
	ep_data_t *ep_data = (ep_data_t *)arg;
	comm_handle_t *handle = ep_data->ep_handle;
	ssize_t len, req_len;
	int is_metadata;

	while (1) {

		is_metadata = !ep_data->is_metadata_read;

		/* We need atleast certain len */
		if (!is_metadata) {
		
			req_len = ep_data->data.msg_len - ep_data->msg_read_len;
			len = bufferevent_read(bev, &ep_data->data.buf[ep_data->msg_read_len],
						req_len);

			if (req_len != len)
				goto err;

			ep_data->is_metadata_read = false;
			bufferevent_setwatermark(bev, EV_READ,
						offsetof(comm_data_t, buf), 0);
		
		} else {
			req_len = offsetof(comm_data_t, buf);
			len = bufferevent_read(bev, (char *)&ep_data->data, 
						req_len);

			if (len == 0)
				return;

			if (req_len != len)
				goto err;

			if (ep_data->data.msg_type == MSG_DATA) {
				assert(ep_data->data.msg_len != 0);

				req_len = ep_data->data.msg_len;
	        		len = bufferevent_read(bev, ep_data->data.buf,
							req_len);

				if (req_len != len) {
					ep_data->msg_read_len = len;
					ep_data->is_metadata_read = true;
					bufferevent_setwatermark(bev, EV_READ,
							 ep_data->data.msg_len - len,
							 0);
				} else {
					is_metadata = false;
				}
			}
		}


		if (ep_data->data.msg_type == MSG_HEARTBEAT_REQ) {

			comm_data_t resp_data;
			size_t len;

			assert(ep_data->data.msg_len == 0);

			resp_data.msg_type = MSG_HEARTBEAT_RESP;
			resp_data.msg_len = 0;

			len = offsetof(comm_data_t, buf); 
		      	if (bufferevent_write(bev, (char *)&resp_data, len) < 0) {
				epLog(ep_data, LOG_WARN, false,
					"Couldn't send heartbeat");
				ep_err(ep_data, EP_HEARTBEAT_FAIL);
			}	

			continue;

		} else if (ep_data->data.msg_type == MSG_DATA) {
		
			if (!is_metadata) {
				/* Call the callback indicating reception of data */
				handle->ep_callback(ep_data->host_num,
							ep_data->host_sw,
							ep_data->data.session,
							ep_data->data.msg_num,
							ep_data->data.buf,
							ep_data->data.msg_len);
			}

			continue;
		} else {
			genericLog(LOG_WARN, false, "Invalid message type: %d",
				ep_data->data.msg_type);
			/* XXX: I assume TCP and ethernet checksum are sufficient */
			assert(0);
			return;
		}
	}

err:
	genericLog(LOG_WARN, false, "Invalid packet data");
	ep_err(ep_data, EP_INVALID_MSG);

	list_remove(&ep_data->ep_handle->conn_list, ep_data);
	bufferevent_free(bev);
	free(ep_data);
	return;
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
		genericLog(LOG_WARN, true, "Accept failed");
		return;
	}

	ep_data = malloc(sizeof(ep_data_t));
	if (ep_data == NULL) {
		genericLog(LOG_WARN, false, "Out of memory");
		goto err;
	}

	ep_data->is_metadata_read = false;
	ep_data->msg_read_len = 0;

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
				ep_data->host_sw = j;
				break;
			}
		}
		if (ep_data->host_num != -1)
			break;
	}

	if (ep_data->host_num == -1) {
		genericLog(LOG_WARN, false,
				"Unknow host contacted endpoint: %s", ipstr);
		goto err;
	}

	/* Set the client socket to non-blocking mode. */
	ret = set_nonblock(hfd);
	if (ret < 0) {
		goto err;
	}


	/* Add to the connections list */
	list_append(&ep_data->ep_handle->conn_list, (void*)ep_data);

	/* 
	 * Setup the read event, libevent will call ep_read() whenever
	 * the clients socket becomes read ready.  We also make the
	 * read event persistent so we don't have to re-add after each
	 * read. 
	 */
	ep_data->bev = bufferevent_socket_new(ep_data->ep_handle->ev_base,
						hfd, BEV_OPT_CLOSE_ON_FREE);
	bufferevent_setcb(ep_data->bev, ep_read, ep_write, ep_event, ep_data);

	/* Need to get some metadata first from host before getting payload */
	bufferevent_setwatermark(ep_data->bev, EV_READ,
					offsetof(comm_data_t, buf), 0);

	bufferevent_enable(ep_data->bev, EV_READ | EV_WRITE);

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
	ep_data_t *ep_data;

	/*
	 * Setup a port, start listening on it and call the callback whenever
	 * data arrives or respond to the heartbeats
	 */
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		genericLog(LOG_FATAL, true,
				"Couldn't open socket for endpoint");
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
		genericLog(LOG_FATAL, true,
				"Endpoint couldn't bind to port: %d",
				EP_LISTEN_PORT);
		goto err;
	}

	/* 
	 * listen: make this socket ready to accept connection requests 
	 */
  	ret = listen(fd, EP_LISTEN_QUEUE_SIZE);
	if (ret < 0) {
		genericLog(LOG_FATAL, true,
				"Endpoint couldn't listen on port: %d",
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

	list_new(&handle->conn_list, NULL);

	/* Start the libevent event loop. */
	event_base_dispatch(handle->ev_base);

	/* We are now closing */

	/* Refuse new connections */
	close(fd);

	/* Close existing connections */
	while ((ep_data = (ep_data_t *)list_pop_head(&handle->conn_list)) != NULL) {
		bufferevent_free(ep_data->bev);
		free(ep_data);
	}

	return 0;
err:
	close(fd);
	return ret;
}

/* Initializes the module. Return negative code on error */
int comm_init(comm_handle_t *handle, comm_err_callback_t err_callback,
		comm_ep_data_callback_t ep_callback)
{
	/* TODO: Run ep_init in seperate thread */

	/* Initialize the event lib */
	int ret = evthread_use_pthreads();
	if (ret < 0 || (handle->ev_base = event_base_new()) == NULL) {
		genericLog(LOG_WARN, false,
				"Libevent initialization failed");
		return -EINVAL;
	}

	handle->is_host = is_node_host();
	handle->ep_callback = ep_callback;
	handle->err_callback = err_callback;

	if (handle->is_host) {

		if (ep_callback != NULL) {
			genericLog(LOG_WARN, false,
					"Callback mentioned for a host node");
			return -EINVAL;
		}
		
		return host_init(handle);
	} else {

		if (ep_callback == NULL) {
		
			genericLog(LOG_WARN, false,
					"No callback for endpoint node");
			return -EINVAL;
		}

		return ep_init(handle);
	}
}

void comm_deinit(comm_handle_t *handle)
{
	if (!handle->is_host) {
		/* For ep, simply quit the loop. That will close all the connections */
		event_base_loopexit(handle->ev_base, NULL);
	} else {
		/* Send signal to end and force flush. Wait for response */
		bufferevent_write(handle->host_write, HOST_END_VAL , 1);
		pthread_join(handle->host_event_thread, NULL);
	}
}
