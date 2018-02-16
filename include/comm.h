/* 
 * This file defines macros for the communication between Hosts and Endpoints 
 * (RPIs) 
 */
#ifndef __COMM_H__
#define __COMM_H__

#include <event.h>
#include <arpa/inet.h>

#include "list.h"

/**** Configurable parameters ****/

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

/**** End of configurable paramters ****/

/* ID of different nodes */
typedef struct {
	char name[MAX_NODE_NAME + 1];
	char ip[NUM_SWITCHES][INET_ADDRSTRLEN];
} nodes_t;


/* How many connections to queue up - Extra, just to be safe */
#define EP_LISTEN_QUEUE_SIZE	(2 * NUM_SWITCHES * NUM_HOSTS)


/* Callback function called by comm module for ep when host sends data */
typedef void (*comm_ep_callback_t)(int host_num, char *buf, int len);

/* Data kept around in host (per ep) */
typedef struct {
	int ep_num;
	int ep_fd;		/* FD for writing to ep */
	bool is_connected;
	list_t comm_data_list;	/* List of pending data to be sent */ 
	struct event *ev_write;
} host_data_t;

/* Handle to the state of comm module */
typedef struct {
	bool is_host;

	struct event_base *ev_base;

	pthread_t host_event_thread;
	int host_write_fd;			/* Write to this pipe initiates writes to eps */
	host_data_t host_data[NUM_EPS][NUM_SWITCHES];
	struct event *ev_outstanding_data;	/* Event for incoming data to send out */
	
	struct event *ev_accept;
	comm_ep_callback_t ep_callback;		/* Callback for ep when data arrives */

} comm_handle_t;

/* Data kept around in ep (per host) */
typedef struct {
	int host_num;
	struct event *ev_read;
	struct event *ev_write;
	comm_handle_t *ep_handle;
} ep_data_t;

/* Internal structure for pending data to be sent to eps */
typedef struct {
	char buf[MAX_DATA_LEN];
	int len;
	int ref;
} pending_data_t;

/* Different types of messages */
#define MSG_INVALID_TYPE	0
#define MSG_HEARTBEAT_REQ	1
#define MSG_HEARTBEAT_RESP	2
#define MSG_DATA		3

/* The communication format - Don't change the order*/
typedef struct {
	int msg_type;
	int msg_len;
	char buf[MAX_DATA_LEN];
} comm_data_t;

/* Function declarations */
int comm_init(comm_handle_t *handle, comm_ep_callback_t ep_callback);
int host_send_msg(comm_handle_t *handle, char *buf, size_t len);

#endif /* __COMM_H__ */
