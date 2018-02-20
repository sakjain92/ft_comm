#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>

#include "comm.h"

/* This represents the host application using the library to comm. with eps */

struct flags_t {

	bool from_stdin;
	int count;

} flags = {false, 10};

void usage(char **argv)
{
	fprintf(stderr,
		"%s: Usage:\n"
		"-i: Take input from stdin\n"
		"-n <number>: Number of messages to be sent <Fixed, if not from stdin>\n",
		argv[0]);
}

void parse_inputs(int argc, char **argv)
{
	int c;
	
	opterr = 0;

	while ((c = getopt (argc, argv, "in:")) != -1) {
		switch (c) {
		case 'i':
			flags.from_stdin = true;
			break;
		case 'n':
			errno = 0;
			flags.count = strtol(optarg, NULL, 10);
			if (errno != 0 || flags.count <= 0) {
				usage(argv);
				exit(-1);
			}
			break;
		default:
			usage(argv);
			exit(-1);
		}
	}
}

/* Error */
void err_callback(int node_num, int sw, int reason)
{
	printf("ERROR_CALLABCK(%d): EP(%d:%d) ", reason, node_num, sw);

	switch (reason) {
	case HOST_CONNECT_FAIL:
		printf("Couldn't connect to EP\n");
		break;
	case HOST_CONNECT_TERMINATE:
		printf("EP Connection failed\n");
		break;
	default:
		printf("Unknown error\n");
		break;
	}
}

int main(int argc, char **argv)
{
	comm_handle_t handle;
	int i, ret;
	char buf[100];
	
	parse_inputs(argc, argv);

	ret = comm_init(&handle, err_callback, NULL);
	if (ret < 0)
		return ret;

	printf("Host started\n");

	if (flags.from_stdin) {

		char *mbuf = NULL;
		size_t size = 0;
		ssize_t ret;


		while (1) {

			ret = getline(&mbuf, &size, stdin);
			
			if (ret <= 0 || mbuf[ret - 1] != '\n')
				break;
	
			mbuf[ret - 1] = '\0';
			printf("Message:%s\n", mbuf);
			host_send_msg(&handle, mbuf, size);

			free(mbuf);
			mbuf = NULL;
			size = 0;
		}
	} else {
		for (i = 0; i < flags.count; i++) {
			sleep(1);
			sprintf(buf, "%d", i + 1);
			printf("Message:%s\n", buf);
			host_send_msg(&handle, buf, strlen(buf) + 1);
		}
	}

	comm_deinit(&handle);

	return 0;
}
