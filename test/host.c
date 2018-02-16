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
		sprintf(buf, "Hello! Message (%d/10) from host", i + 1);
		printf("Sending message %d of len %zu\n", i + 1, strlen(buf));
		host_send_msg(&handle, buf, strlen(buf) + 1);
	}

	printf("Host ended\n");
	while(1);
	return 0;
}
