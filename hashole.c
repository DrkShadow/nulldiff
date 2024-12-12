

// for SEEK_HOLE, etc
#define _GNU_SOURCE

#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/param.h>

#include "likely.h"

#define least(x,y) ( x < y ? x : y)
#define greatest(x,y) ( x < y ? y : x)


int main(int argc, char **argv) {

	// Will compare two files, determining if they are the same except in areas of NULL
	// Does not tell you which file has the most null. Use `gzip -1 | wc -c` for that.
	if (argc < 2 || argc > 2) {
		printf("Error: You must specify one input file.\n");
		return 2;
	}

	// Args:
	// -5: show only 512-byte block diff/same
	// -d: show only diff
	// -s: show only same
	// -r: show only ratio
	// -4: show only 4096-byte block diff/same
	// -n: don't count null blocks as indifferent
	// -

	int in1 = open(argv[1], O_NOATIME | O_DIRECT, O_RDONLY);

	if (in1 == -1) {
		fprintf(stderr, "Unable to open %s", argv[1]);
		perror(", ");
		return 2;
	}

	struct stat stat_buf;
	if (fstat(in1, &stat_buf) == -1) {
		fprintf(stderr, "Error: Unable to stat %s\n", argv[1]);
		close(in1);
		return 2;
	}
	if (!S_ISREG(stat_buf.st_mode)) {
		fprintf(stderr, "Error: I'm not able to work with anything but regular files. (%s)\n", argv[1]);
		close(in1);
		return 2;
	}

	if (stat_buf.st_size == 0) {
		close(in1);
		fprintf(stderr, "Error: I can't work with zero-length file %s.\n", argv[1]);
		return 2;
	}
	if (lseek(in1, 0, SEEK_DATA) == -1 && errno == ENXIO) {
		close(in1);
		fprintf(stderr, "Error: File is non-zero but is completely sparse, with no data:\n\t%s.\n", argv[1]);
		return 2;
	}

	size_t hole_1 = lseek(in1, 0, SEEK_HOLE);

	close(in1);
	if (hole_1 == (size_t)-1 || hole_1 == stat_buf.st_size) {
		// No hole.
		return 0;
	}

	// Else, hole.
	return 1;
}
