

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
#include <sys/mount.h>	// block size ioctl
#include <sys/ioctl.h>

#include "likely.h"

#define least(x,y) ( x < y ? x : y)
#define greatest(x,y) ( x < y ? y : x)

char zerobuf[8192] = {0};
char *zero = zerobuf;

size_t num_blk_4	= 0;	// Number of 4096-byte blocks with data -- blocks without holes or zero-matches.

typedef struct {
		int f_in;
		const size_t size;
		const int fd;
	} f_in_info_t;

int main(int argc, char **argv) {

	// Will compare two files, determining if they are the same except in areas of NULL
	// Does not tell you which file has the most null. Use `gzip -1 | wc -c` for that.
	if (argc < 2 || argc > 2) {
		printf("Error: You must specify one input file.\n");
		return -1;
	}

	// Args:
	// -5: show only 512-byte block diff/same
	// -d: show only diff
	// -s: show only same
	// -r: show only ratio
	// -4: show only 4096-byte block diff/same
	// -n: don't count null blocks as indifferent
	// -

	int in1 = open(argv[1], O_NOATIME, O_RDONLY);
	if (in1 == -1) {
		fprintf(stderr, "Unable to open %s", argv[1]);
		perror(", ");
		return -1;
	}

	struct stat stat_buf;
	if (fstat(in1, &stat_buf) == -1) {
		fprintf(stderr, "Error: Unable to stat %s\n", argv[1]);
		close(in1);
		return -1;
	}
	if (!S_ISREG(stat_buf.st_mode)) {
		fprintf(stderr, "Error: I'm not able to work with anything but regular files. (%s)\n", argv[1]);
		close(in1);
		return -1;
	}
	const size_t in1_size = stat_buf.st_size;
	f_in_info_t fin1 = {
			.f_in = in1,
			.size = in1_size,
			.fd = in1
		};

	if (fin1.size == 0) {
		// No null blocks.
		close(in1);
		return 0;
	}

	if (lseek(fin1.fd, 0, SEEK_DATA) == -1 && errno == ENXIO) {
		close(in1);
		// No null blocks. Only holes.
		return 0;
	}
	

	const uint8_t *const in1map = mmap(NULL, fin1.size, PROT_READ, MAP_PRIVATE | MAP_NORESERVE | MAP_NONBLOCK, in1, 0);
	if (in1map == MAP_FAILED) {
		close(in1);
		fprintf(stderr, "Error: unable to mmap %s, ", argv[1]);
		perror("");
		return -1;
	}

	if (unlikely_err(madvise((void *)in1map, fin1.size, MADV_SEQUENTIAL) != 0)) {
		perror("Unable to madvise0,");
	}


	//int bksize;
	//int iores = ioctl(in1, BLKSSZGET, &bksize);
	//if (iores != 0) {
	//	perror(NULL);
	//}
	//close(in1);
	//return 0;

	const int PAGE_SIZE = stat_buf.st_blksize; // MIN(MIN(sysconf(_SC_PAGESIZE), 4096);
	const int PAGE_SIZE_bits = PAGE_SIZE - 1;
	const size_t PAGE_SIZE_bits_not = -PAGE_SIZE;

	if (unlikely(sizeof(zerobuf) < PAGE_SIZE)) {
		zero = malloc(PAGE_SIZE);
		if (zero == nullptr) {
			fprintf(stderr, "Unable to allocate %i bytes for zero block.\n", PAGE_SIZE);
			close(in1);
			munmap((void *)in1map, fin1.size);

			return -1;
		}
	}

	size_t f_off = 0;
	size_t unmap_off = 0;	// both will have the same ranges mapped.

	size_t next_hole = lseek(in1, 0, SEEK_HOLE);
	f_off = lseek(in1, 0, SEEK_DATA);

	while (f_off < fin1.size && f_off >= 0) {
		if (unlikely(f_off == (size_t)-1)) {
			// No more blocks, no more zero-blocks.
			break;
		}
		if (unlikely(next_hole <= f_off)) {
			f_off = lseek(in1, f_off, SEEK_DATA);
			// if there's no more data,
			if (f_off == -1) {
				break;
			}

			// Because a hole is at least a page size?
			const size_t munmap_size = (f_off - unmap_off) & PAGE_SIZE_bits_not;
			if (likely(munmap_size > 0)) {
				munmap((void *)(in1map + unmap_off), munmap_size);
				unmap_off = f_off;

				if (unlikely_err(madvise((void *)(in1map + f_off), MIN(2 << 20, fin1.size - f_off), MADV_WILLNEED) != 0)) {
					fprintf(stderr, "%i-hl: %p, %li; %li, %li; is ENOMEM? %i; ", errno, in1map, MIN(2 << 20, fin1.size - f_off), f_off, fin1.size, errno == ENOMEM);
					perror("Unable to madvise-hole");
				}
			}

			// there's always a next hole.
			next_hole = lseek(in1, f_off, SEEK_HOLE);

			// Re-check vs size.
			continue;
		}

		const int blocksize = MIN(fin1.size - f_off, PAGE_SIZE);
		if (unlikely(memcmp(zero, in1map + f_off, blocksize) == 0)) {
			// Oh hey -- found a null block! Report true.
			munmap((void *)(in1map + unmap_off), fin1.size - unmap_off);
			close(in1);
			return 1;
		}

		f_off += PAGE_SIZE;
		lseek(in1, f_off, SEEK_SET);


		if (likely(f_off < fin1.size)) {
			if (unlikely((f_off & 0xFFFFFF) == 0)) {
				munmap((void *)(in1map + unmap_off), f_off - unmap_off);
				unmap_off = f_off;

				if (unlikely_err(madvise((void *)(in1map + f_off), MIN(2 << 20, fin1.size - f_off), MADV_WILLNEED) != 0)) {
					fprintf(stderr, "%i-1: %p, %li; %li, %li; is ENOMEM? %i; ", errno, in1map, MIN(2 << 20, fin1.size - f_off), f_off, fin1.size, errno == ENOMEM);
					perror("Unable to madvise");
				}
			}
			else if (unlikely((f_off & 0xEFFFFF) == 0)) {
				if (unlikely_err(madvise((void *)(in1map + f_off), MIN(2 << 20, fin1.size - f_off), MADV_WILLNEED) != 0)) {
					fprintf(stderr, "%i-2: %p, %li; %li, %li; ", errno, in1map, MIN(2 << 20, fin1.size - f_off), f_off, fin1.size);
					perror("Unable to madvise2");
				}
			}
		}
	}


	if (unmap_off < fin1.size)
		munmap((void *)(in1map + unmap_off), fin1.size - unmap_off);

	// No nulls found.
	close(in1);
	return 0;
}
