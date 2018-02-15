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

	comm_init(&handle, callback);
	
	printf("Endpoint started: Will wait forever\n");
	
	while(1);

	return 0;
}
