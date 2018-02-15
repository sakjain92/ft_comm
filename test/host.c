#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "comm.h"

int main()
{
	comm_handle_t handle;
	int i;
	char buf[100];
	comm_init(&handle, NULL);

	printf("Host starting\n");

	for (i = 0; i < 10; i++) {
		sleep(1);
		sprintf(buf, "Sending message %d/10", i + 1);
		host_send_msg(&handle, buf, strlen(buf) + 1);
	}

	printf("Host ended\n");
	return 0;
}
