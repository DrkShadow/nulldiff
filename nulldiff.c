

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

#include <sys/param.h>

#include "likely.h"

#define least(x,y) ( x < y ? x : y)
#define greatest(x,y) ( x < y ? y : x)

char *zero = NULL;

typedef struct {
		FILE *f_in;
		const size_t size;
		const int fd;
	} f_in_info_t;

static inline size_t find_next_hole(f_in_info_t *fin, size_t *const next_hole, size_t f_off) {
	if (*next_hole > f_off)
		return f_off;

	// We're at a hole. Find the next data.
	const size_t next_data = lseek(fin->fd, f_off, SEEK_DATA);
	if (next_data == (size_t)-1 && errno == ENXIO)
		return -1;

	if (next_data > f_off)
		f_off = next_data;

	// Ok, now find the next hole.
	const size_t hole = lseek(fin->fd, next_data, SEEK_HOLE);
	*next_hole = hole;

	return f_off;
}

int main(int argc, char **argv) {

	// Will compare two files, determining if they are the same except in areas of NULL
	// Does not tell you which file has the most null. Use `gzip -1 | wc -c` for that.
	if (argc < 3 || argc > 3) {
		printf("Error: You must specify two input files.\n");
		return 1;
	}

	FILE *in1 = fopen(argv[1], "rb");
	if (in1 == nullptr) {
		fprintf(stderr, "Unable to open %s", argv[1]);
		perror(", ");
		return 1;
	}
	FILE *in2 = fopen(argv[2], "rb");
	if (in2 == nullptr) {
		fclose(in1);
		fprintf(stderr, "Unable to open %s", argv[2]);
		perror(", ");
		return 1;
	}

	struct stat stat_buf;
	if (fstat(fileno(in1), &stat_buf) == -1) {
		fprintf(stderr, "Error: Unable to stat %s\n", argv[1]);
		fclose(in1);
		fclose(in2);
		return 1;
	}
	if (!S_ISREG(stat_buf.st_mode)) {
		fprintf(stderr, "Error: I'm not able to work with anything but regular files. (%s)\n", argv[1]);
		fclose(in1);
		fclose(in2);
		return 1;
	}
	const size_t in1_size = stat_buf.st_size;
	if (fstat(fileno(in2), &stat_buf) == -1) {
		fprintf(stderr, "Error: Unable to stat %s\n", argv[2]);
		fclose(in1);
		fclose(in2);
		return 1;
	}
	if (!S_ISREG(stat_buf.st_mode)) {
		fprintf(stderr, "Error: I'm not able to work with anything but regular files. (%s)\n", argv[2]);
		fclose(in1);
		fclose(in2);
		return 1;
	}
	const size_t in2_size = stat_buf.st_size;

	f_in_info_t fin1 = {
			.f_in = in1,
			.size = in1_size,
			.fd = fileno(in1)
		};
	f_in_info_t fin2 = {
			.f_in = in2,
			.size = in2_size,
			.fd = fileno(in2)
		};

	if (fin1.size == 0 || fin2.size == 0) {
		if (fin1.size == 0) {
			fclose(in1);
			fclose(in2);
			fprintf(stderr, "Error: I can't work with zero-length file %s.\n", argv[1]);
			return 1;
		}
		if (fin2.size == 0) {
			fclose(in1);
			fclose(in2);
			fprintf(stderr, "Error: I can't work with zero-length file %s.\n", argv[2]);
			return 1;
		}
	}
	if (lseek(fin1.fd, 0, SEEK_DATA) == -1 && errno == ENXIO) {
		fclose(in1);
		fclose(in2);
		fprintf(stderr, "Error: File is non-zero but is completely sparse, with no data:\n\t%s.\n", argv[1]);
		return 1;
	}
	if (lseek(fin2.fd, 0, SEEK_DATA) == -1 && errno == ENXIO) {
		fclose(in1);
		fclose(in2);
		fprintf(stderr, "Error: File is non-zero but is completely sparse, with no data:\n\t%s.\n", argv[2]);
		return 1;
	}
	

	const uint8_t *const in1map = mmap(NULL, fin1.size, PROT_READ, MAP_PRIVATE | MAP_NORESERVE | MAP_NONBLOCK, fin1.fd, 0);
	if (in1map == MAP_FAILED) {
		fclose(in1);
		fclose(in2);
		fprintf(stderr, "Error: unable to mmap %s, ", argv[1]);
		perror("");
		return 1;
	}
	const uint8_t *const in2map = mmap(NULL, fin2.size, PROT_READ, MAP_PRIVATE | MAP_NORESERVE | MAP_NONBLOCK, fin2.fd, 0);
	if (in1map == MAP_FAILED) {
		munmap((void *)in1map, in1_size);
		fclose(in1);
		fclose(in2);
		fprintf(stderr, "Error: unable to mmap %s, ", argv[2]);
		perror("");
		return 1;
	}


	const int PAGE_SIZE = sysconf(_SC_PAGESIZE);
	const int PAGE_SIZE_bits = PAGE_SIZE - 1;
	const size_t PAGE_SIZE_bits_not = (size_t)0 - PAGE_SIZE;

	zero = malloc(PAGE_SIZE);
	if (zero == nullptr) {
		fprintf(stderr, "Unable to allocate %i bytes for zero block.\n", PAGE_SIZE);
		goto err;

	}

	size_t f_off = 0;
	size_t unmap_off = 0;	// both will have the same ranges mapped.

	{
		// Set the first data block.
		size_t hole_1 = 0;
		size_t hole_2 = 0;

		// Find a byte offset that both files have
		do {
			hole_1 = lseek(fin1.fd, hole_2, SEEK_DATA);
			hole_2 = lseek(fin2.fd, hole_1, SEEK_DATA);

			if (hole_1 == (size_t)-1 || hole_2 == (size_t)-1) {
				fprintf(stderr, "Error: Files do not share any data blocks.\n");
				return -2;
			}
		} while (hole_1 != hole_2);

		f_off = MAX(hole_1, hole_2);
	}

	const off_t max_size = MIN(fin1.size, fin2.size);
	if (f_off >= max_size) {
		// this should result in an ENXIO, above
		fprintf(stderr, "Error: Files do not share any data blocks.\n");
		return -2;
	}

	// Check for holes. Start at the earliest data.
	// returns fsize if no hole.
	size_t in1_hole = lseek(fin1.fd, f_off, SEEK_HOLE);
	size_t in2_hole = lseek(fin2.fd, f_off, SEEK_HOLE);
	off_t next_hole = MIN(in1_hole, in2_hole);

	// Setup: madvise.
	//madvise(in1map, in1_size, MADV_SEQUENTIAL);
	//madvise(in2map, in2_size, MADV_SEQUENTIAL);
	{
		const size_t align_off = f_off & PAGE_SIZE_bits_not;
		const int madv_size = MIN(max_size - align_off, MIN(2 << 20, next_hole - align_off));

		madvise((void *)in1map + align_off, madv_size, MADV_SEQUENTIAL);
		madvise((void *)in2map + align_off, madv_size, MADV_SEQUENTIAL);

		const size_t remaining = MIN(4<<20, max_size - align_off);
		if (max_size - align_off > (2 << 20)) {
			madvise((void *)in1map + (2 << 20) + align_off, remaining, MADV_WILLNEED);
			madvise((void *)in2map + (2 << 20) + align_off, remaining, MADV_WILLNEED);
		}
	}

	off_t madv_off = 0;
	
	// We stop at the last part of the file with data. After that, they're null-equal.
	while (f_off < max_size) {
		// Helps find holes and data chunks. Also informational, /proc/.../fdinfo/
		lseek(fin1.fd, f_off, SEEK_SET);
		lseek(fin2.fd, f_off, SEEK_SET);

		if (f_off >= next_hole) {
			size_t in1_hole, in2_hole;

			f_off = find_next_hole(&fin1, &in1_hole, f_off);
			if (f_off == (size_t)-1)
				break;	// the rest is hole -- same, excluding nulls.

			f_off = find_next_hole(&fin2, &in2_hole, f_off);
			if (f_off == (size_t)-1)
				break;	// the rest is hole -- same, excluding nulls.

			next_hole = MIN(in1_hole, in2_hole);
		}

		// unmap_off is always aligned, so I don't need to align this.
		if (f_off - unmap_off >= PAGE_SIZE) {
			// I check this every 1MB, so int.
			const int unmap_sz = (f_off - unmap_off) & ~PAGE_SIZE_bits;
			munmap((void *)in1map + unmap_off, unmap_sz - 1);
			munmap((void *)in2map + unmap_off, unmap_sz - 1);
			unmap_off += unmap_sz;
		}

		// madv_off is the end of the _SEQUENTIAL range. update it every 1MB.
		if (f_off - madv_off > (1 >> 20)) {
			madv_off = f_off & PAGE_SIZE_bits_not;
			const int madv_len = MIN(2 << 20, next_hole - madv_off);

			// These were "willneed", now they're "sequential"
			madvise((void *)in1map + madv_off, madv_len, MADV_SEQUENTIAL);
			madvise((void *)in2map + madv_off, madv_len, MADV_SEQUENTIAL);

			if (madv_off + (2 << 20) < next_hole) {
				const int madv_len = MIN(2 << 20, next_hole - (madv_off + (2 << 20)));
				madvise((void *)in1map + madv_off + (2 << 20), madv_len, MADV_WILLNEED);
				madvise((void *)in2map + madv_off + (2 << 20), madv_len, MADV_WILLNEED);
			}
		}


		// compare 1MB at a time, and then loop for madvise.
		// Take the aligned 1MB block, add 1MB to that, and min of that and next_hole.
		int compsize = MIN((f_off & ~((size_t)(1 << 20) - 1)) + (1 << 20), next_hole) - f_off;
		int compblock = MIN(PAGE_SIZE, compsize);
		while (compsize > 0 && (
					memcmp(in1map + f_off, in2map + f_off, compblock) == 0 ||
					memcmp(in1map + f_off, zero, compblock) == 0 ||
					memcmp(in2map + f_off, zero, compblock) == 0
					)) {
			f_off += compblock;
			compsize -= compblock;

			if (unlikely(compsize < compblock))
				compblock = compsize;
		}

		if (f_off - unmap_off >= PAGE_SIZE) {
			// I check this every 1MB, so int.
			const int unmap_sz = (f_off - unmap_off) & ~PAGE_SIZE_bits;
			munmap((void *)in1map + unmap_off, unmap_sz - 1);
			munmap((void *)in2map + unmap_off, unmap_sz - 1);
			unmap_off += unmap_sz;
		}

		if (likely(compsize == 0))
			continue;
		// Else, check parts of the block.

		// The first step is to take half the buffer, as the whole block didn't check out.
		int maxcomp = MIN(PAGE_SIZE, next_hole - f_off);
		int blocksize = MIN(PAGE_SIZE, next_hole - f_off);
		int checked = 0;
		while (checked < maxcomp) {
			// This reduces the size until the mismatch is found. Then it stops.
			blocksize >>= 1;

			if (blocksize == 0) {
				//fprintf(stderr, "Breaking because 0 blocksize.\n");
				break;
			}

			// As long as we have buffer for it, use the larger comparison functions.
			// blocksize > inleft1 - checked, inleft2 - checked..
			if (blocksize + checked > maxcomp) {
				// Not enough data; shrink the block size..
				// Notice: this may shrink the block size < 16. This is permissible in the
				// end-of-file case; it'll go down to the small-comparison.
				//fprintf(stderr, "Breaking because blocksize too big.\n");
				continue;
			}

			const size_t blockoff = f_off + checked;
			if (blocksize >= 16) {
				if (memcmp(in1map + blockoff, in2map + blockoff, blocksize) == 0) {
					checked += blocksize;
					//fwrite(in1buf + checked, blocksize, 1, stdout);
					goto recalc_blocksize;
				}
				if (memcmp(in1map + blockoff, zero, blocksize) == 0) {
					checked += blocksize;
					//fwrite(in2buf + checked, bufavail, 1, stdout);
					goto recalc_blocksize;
				}
				if (memcmp(in2map + blockoff, zero, blocksize) == 0) {
					checked += blocksize;
					//fwrite(in1buf + checked, bufavail, 1, stdout);
					goto recalc_blocksize;
				}

				// We didn't goto, ...
				// File mismatch detected in this block. Shrink block size and try again.
				//fprintf(stderr, "sizing down blocksize for mismatch block.\n");
				continue;
			}

			// really small size. Just do it byte-for-byte.

			//fprintf(stderr, "curblock: %i; Checking small at index %i; block size: %i\n", curblock, checked, blocksize);
			const uint8_t *const in1buf = in1map + blockoff;
			const uint8_t *const in2buf = in2map + blockoff;
			for (int i = 0; i < blocksize; i++) {
				if (in1buf[i] == in2buf[i]) {
					//fwrite(in1buf + checked + i, 1, 1, stdout);
					continue;
				}
				else if (in1buf[i] == 0) {
					//fwrite(in2buf + checked + i, 1, 1, stdout);
					continue;
				}
				else if (0 == in2buf[i]) {
					//fwrite(in1buf + checked + i, 1, 1, stdout);
					continue;
				}

				// We have a file mis-match. This isn't permissible.
				fprintf(stderr, "Files mismatch\n");
				fprintf(stderr, "Files mismatch (at byte %li)\n", blockoff + i);

				munmap((void *)in1map + unmap_off, fin1.size - unmap_off);
				munmap((void *)in2map + unmap_off, fin2.size - unmap_off);
				fclose(fin1.f_in);
				fclose(fin2.f_in);

				free(zero);

				return 1;
			}
			checked += blocksize;
			//fprintf(stderr, "done: Checking small at index %i; block size: %i\n", curblock, checked, blocksize);

recalc_blocksize:
			if (checked == PAGE_SIZE) {
				// done.
				//fprintf(stderr, "Checked == BUF_SIZE\n");
				continue;
			}

			while ( blocksize <= maxcomp - checked && blocksize <= PAGE_SIZE) {
				// We just completed a full block. Bump the blocksize back up.
				// This lets us check the second half of this failed block.
				blocksize <<= 1;
			}
			//fprintf(stderr, "Set blocksize to %i\n", blocksize);
		} // While we haven't checked whole files..

		f_off += checked;

		// Now that we've left the comparison loop, either one file is done or both blocks match.
	} // while there is a not-eof file

	// TODO: Insert file-length check.

	fprintf(stderr, "Files are the same, possibly excluding null bytes.\n");

	if (f_off < fin1.size)
		munmap((void *)in1map + unmap_off, fin1.size - unmap_off);
	if (f_off < fin2.size)
		munmap((void *)in2map + unmap_off, fin2.size - unmap_off);

	fclose(fin1.f_in);
	fclose(fin2.f_in);
	return 0;

err:
	if (zero != nullptr)
		free(zero);

	if (in1map != nullptr)
		munmap((void *)in1map + unmap_off, fin1.size - unmap_off);
	if (in2map != nullptr)
		munmap((void *)in2map + unmap_off, fin2.size - unmap_off);
	fclose(fin1.f_in);
	fclose(fin2.f_in);

	return 1;
}
