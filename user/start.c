#include <start.h>
#include <lib.h>
#include <stdlib.h>

void start(int argc, char *argv[]) {
	if (mm_init() < 0)
		exit(-1);
	int ret = main(argc, argv);
	exit(ret);
}
