#include <stdio.h>
#include <unistd.h>

#include "comm.h"


void callback(int host_num, char *buf, int len)
{
	(void)len;
	printf("EP received from host %d, msg |%s|\n",
		host_num, buf);
}

int main(void)
{
	comm_handle_t handle;

	printf("Endpoint starting\n");

	comm_init(&handle, callback);
	
	printf("Endpoint ended\n");

	return 0;
}
