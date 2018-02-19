/* 
 * This file defines macros for the communication between Hosts and Endpoints 
 * (RPIs) 
 */
#ifndef __COMM_H__
#define __COMM_H__

#include <event.h>
#include <arpa/inet.h>

#include "list.h"

#include <pthread.h>
#include <semaphore.h>

/**** Configurable parameters ****/

/* Enable this macro only if testing */
#define TESTING

#define NUM_HOSTS	4
#define NUM_EPS		3

/* Using two different switches */
#define NUM_SWITCHES	2

#define MAX_NODE_NAME	25

#define HOST_NAME_PREFIX	"host"
#define EP_NAME_PREFIX		"rpi"

/* Port on which end point listens */
#define EP_LISTEN_PORT		14700

/* All hosts - Keep in sorted order*/
#define HOST_NODES_LIST 						\
	{								\
		{HOST_NAME_PREFIX "1", {"192.168.1.1", "192.168.2.1"}},	\
		{HOST_NAME_PREFIX "2", {"192.168.1.2", "192.168.2.2"}},	\
		{HOST_NAME_PREFIX "3", {"192.168.1.3", "192.168.2.3"}},	\
		{HOST_NAME_PREFIX "4", {"192.168.1.4", "192.168.2.4"}},	\
	}

/* All endpoints - Keep in sorted order */
#define EP_NODES_LIST							\
	{								\
		{EP_NAME_PREFIX "1", {"192.168.1.11", "192.168.2.11"}},	\
		{EP_NAME_PREFIX "2", {"192.168.1.12", "192.168.2.12"}},	\
		{EP_NAME_PREFIX "3", {"192.168.1.13", "192.168.2.13"}},	\
	}

#define MAX_DATA_LEN	4096

/* Maximum seconds to wait for connection to be established */
#define MAX_CONN_TIMEOUT_SEC		5

/* Interval between retrying to connect */
#define MAX_CONN_RETRY_TIMEOUT_SEC	5

/* Maximum times to try to reconnect */
#define MAX_CONN_RETRIES		3

/**** End of configurable paramters ****/

/* Error Code */
#define HOST_CONNECT_FAIL	1
#define HOST_CONNECT_TERMINATE	2
#define EP_CONNECT_TERMINATE	4
#define EP_HEARTBEAT_FAIL	5
#define EP_INVALID_MSG		6

/* Logging Type */
#define LOG_FATAL	1
#define LOG_WARN	2

/* ID of different nodes */
typedef struct {
	char name[MAX_NODE_NAME + 1];
	char ip[NUM_SWITCHES][INET_ADDRSTRLEN];
} nodes_t;


/* How many connections to queue up - Extra, just to be safe */
#define EP_LISTEN_QUEUE_SIZE	(2 * NUM_SWITCHES * NUM_HOSTS)


/* Callback function called by comm module for ep when host sends data */
typedef void (*comm_ep_data_callback_t)(int host_num, int sw, int session,
					int msg_num, char *buf, int len);

/* Callback called by comm module when host/ep notice connection failure */
typedef void (*comm_err_callback_t)(int node_num, int sw, int reason);

/* Different types of messages */
#define MSG_INVALID_TYPE	0
#define MSG_HEARTBEAT_REQ	1
#define MSG_HEARTBEAT_RESP	2
#define MSG_DATA		3

/* The communication format - Don't change the order*/
typedef struct {
	int msg_type;
	int msg_len;
	int msg_num;
	/* Different instance of same host have different sessions */
	int session;
	char buf[MAX_DATA_LEN];
} comm_data_t;

struct comm_handle;

/* Data kept around in host (per ep) */
typedef struct {
	int ep_num;
	int ep_sw;

	bool is_connected;
	int connect_fd;
	int retries_left;
	struct event *ev_connect;
	struct bufferevent *bev_write;

	struct comm_handle *handle;
} host_data_t;

#define HOST_TRIGGER_VAL	"t"
#define HOST_END_VAL		"e"

/* Handle to the state of comm module */
typedef struct comm_handle {
	bool is_host;
	struct event_base *ev_base;
	comm_err_callback_t err_callback;

	pthread_t host_event_thread;
	struct bufferevent *host_write;		/* Write to this pipe initiates writes to eps */
	struct bufferevent *ev_outstanding;	/* Event for incoming data to send out */
	
	pthread_mutex_t lock;
	list_t data_list;			/* Pending data to be sent */
	int num_succ_conns;			/* Total number of successful conn */

	host_data_t host_data[NUM_EPS][NUM_SWITCHES];
	int num_msg_sent;
	int session;
	sem_t connect_sem;			/* Semaphore to wait for all connections */

	struct event *ev_accept;
	list_t conn_list;			/* List of all the current connections */
	comm_ep_data_callback_t ep_callback;		/* Callback for ep when data arrives */

} comm_handle_t;

/* Data kept around in ep (per host) */
typedef struct {
	int host_num;
	int host_sw;

	bool is_metadata_read;
	struct bufferevent *bev;
	int msg_read_len;			/* How much have been read already */
	comm_data_t data;

	comm_handle_t *ep_handle;
} ep_data_t;

/* Function declarations */
int comm_init(comm_handle_t *handle, comm_err_callback_t err_callback,
		comm_ep_data_callback_t ep_data_callback);
int host_send_msg(comm_handle_t *handle, char *buf, size_t len);
void comm_deinit(comm_handle_t *handle);

#endif /* __COMM_H__ */
