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

int main(int argc, char **argv)
{
	comm_handle_t handle;
	int i;
	char buf[100];
	
	parse_inputs(argc, argv);

	comm_init(&handle, NULL);

	if (flags.from_stdin) {

		char *mbuf;
		size_t size;

		while (getline(&mbuf, &size, stdin) > 0 && mbuf[size - 1] == '\n') {
			mbuf[size - 1] = '\0';
			host_send_msg(&handle, mbuf, size);
			printf("Message:%s\n", mbuf);
		}
	} else {
		for (i = 0; i < flags.count; i++) {
			sleep(1);
			sprintf(buf, "%d", i + 1);
			host_send_msg(&handle, buf, strlen(buf) + 1);
			printf("Message:%s\n", buf);
		}
	}

	comm_deinit(&handle);

	return 0;
}
