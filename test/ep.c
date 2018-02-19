#include <stdio.h>
#include <unistd.h>

#include "comm.h"


void callback(int host_num, int host_sw, int session, 
		int msg_num, char *buf, int len)
{
	(void)len;
	printf("Host(%d:%d): Session(%d): MsgNum(%d): Msg(%s)\n",
		host_num, host_sw, session, msg_num, buf);
}

/* Error */
void err_callback(int node_num, int sw, int reason)
{
	printf("ERROR_CALLABCK(%d): HOST(%d:%d) ", reason, node_num, sw);

	switch (reason) {
	case EP_HEARTBEAT_FAIL:
		printf("Problem detected with connection\n");
		break;
	case EP_CONNECT_TERMINATE:
		printf("Host Connection failed\n");
		break;
	case EP_INVALID_MSG:
		printf("Invalid message received from host\n");
		break;
	default:
		printf("Unknown error\n");
		break;
	}
}


int main(void)
{
	comm_handle_t handle;

	comm_init(&handle, err_callback, callback);
	
	return 0;
}
