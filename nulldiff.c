

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

const uint8_t *zero = NULL;

enum {
		RET_SUBSET_1	= 0b0000001,
		RET_SUBSET_2	= 0b0000010,
		RET_GREATEST_1	= 0b0000100,
		RET_GREATEST_2	= 0b0001000,
	} retcode;

typedef struct {
		FILE *const restrict f_in;
		const size_t size;
		const int fd;
	} f_in_info_t;

static inline size_t find_next_data(const f_in_info_t fin[const restrict static 1], size_t next_hole[const restrict static 1], size_t f_off) {
	if (unlikely(*next_hole > f_off))
		return f_off;

	// We're at a hole. Find the next data.
	const size_t next_data = lseek(fin->fd, f_off, SEEK_DATA);
	if (unlikely(next_data == (size_t)-1)) { // && errno == ENXIO) {
		// There's no more data, and caller need to detect past end-of-file
		return -1;
	}

	if (next_data > f_off)
		f_off = next_data;

	// Ok, now find the next hole. This will always be positive, unless error.
	*next_hole = lseek(fin->fd, f_off, SEEK_HOLE);

	return f_off;
}

static inline void mumap(const uint8_t *const restrict addr1_base, const uint8_t *const restrict addr2_base, const size_t mmap_offset, size_t unmap_offset[const restrict static 1]) {
	__auto_type const page_data = ({
				static struct {
						size_t PAGE_SIZE_bits;
						int PAGE_SIZE;
					} init = (constexpr typeof(init)){ .PAGE_SIZE_bits = 0, .PAGE_SIZE = 0};
				if (unlikely(init.PAGE_SIZE == 0)) {
					init.PAGE_SIZE = sysconf(_SC_PAGESIZE);
					init.PAGE_SIZE_bits = init.PAGE_SIZE - 1;
				}
				(const typeof(&init)) &init;
			});

	if (likely(mmap_offset - *unmap_offset > page_data->PAGE_SIZE)) {
		const size_t unmap_sz = (mmap_offset - *unmap_offset) & ~page_data->PAGE_SIZE_bits;
		munmap((void *)addr1_base + *unmap_offset, unmap_sz);
		munmap((void *)addr2_base + *unmap_offset, unmap_sz);
		*unmap_offset += unmap_sz;
	}
}

// Compare a block against null. Returns the amount of non-null in *fsz.
// Returns true if completely null, else false if not completely null.
static inline bool compnull(const size_t n, const uint8_t data[const restrict static n], size_t fsz[const restrict 1], const bool stop_on_mismatch) {
	const int PAGE_SIZE = ({
				static int pgsize = 0;
				if (unlikely(pgsize == 0))
					pgsize = sysconf(_SC_PAGESIZE);
				pgsize;
				});
	const int PAGE_SIZE_bits = PAGE_SIZE - 1;
	const size_t PAGE_SIZE_bits_not = (size_t)0 - PAGE_SIZE;

	size_t fsz_calc = 0;
	bool isnull = true;

	size_t cmpoff = 0;
	while (cmpoff < n) {
		const int compsz = MIN(n - cmpoff, PAGE_SIZE);
		if (memcmp(data + cmpoff, zero, compsz) != 0) {
			if (fsz != nullptr)
				fsz_calc += compsz;
			if (isnull)
				isnull = false;
			if (stop_on_mismatch)
				break;
		}

		cmpoff += compsz;
	}

	if (fsz != nullptr)
		fsz[0] = fsz_calc;

	return isnull;
}

int main(int argc, char **argv) {

	// Will compare two files, determining if they are the same except in areas of NULL
	// Does not tell you which file has the most null. Use `gzip -1 | wc -c` for that.
	if (argc < 3 || argc > 3) {
		printf("Error: You must specify two input files.\n");
		return 1;
	}

	static struct {
			bool show_greatest;	// The largest file is ...
			bool subset;	// -1: fin1; -2: fin2; 0: same; -3: both files have unique data compared to the other
		} settings = (constexpr typeof(settings)){.show_greatest = false, .subset = false};

	
	// -g: Return the greatest size file
	// -s: Return whether one is a subset of the other; may return both
	// return type: 7 bit return-code, or -2 on error or -1 on unreconcileable difference.
	// 0b 0 1 1 1 1 1 1 1
	// 		| | | | | | `- Set if in1 is a subset of in2. It may be that neither is a proper subset.
	// 		| | | | | `--- Set if in2 is a subset of in1. It may be that both are subset - the files are equal.
	// 		| | | | `----- Set if in1 has more data than in2.
	// 		| | | `------- Set if in2 has more data than in1.
	// 		| | `---------
	// 		| `-----------
	// 		`-------------
	// 		If a file is not a subset of the other, it includes data that the other does not have.
	// 		If neither file is a subset of the other, then both files have unique data.
	// 		If the files _differ_ -- the same offsets have non-matching, non-null data, then
	// 		the return code is -1.
	// 		-2 indicates that the files have data, but share no blocks.
	// 		-3 indicates a file type/access/other error, such as zero-length or completely sparse.
	// 		-4 indicates a system error, such as unable to mmap.
	char ci;
	while (ci = getopt(argc, argv, "gs")) {
		if (ci == -1)
			break;
		switch(ci) {
			case 'g':
				settings.show_greatest = true;
				break;
			case 's':
				settings.subset = true;

			default:
				__builtin_unreachable();
				break;
		}
	}

	FILE *in1 = fopen(argv[1], "rb");
	if (in1 == nullptr) {
		fprintf(stderr, "Unable to open %s", argv[1]);
		perror(", ");
		return -3;
	}
	FILE *in2 = fopen(argv[2], "rb");
	if (in2 == nullptr) {
		fclose(in1);
		fprintf(stderr, "Unable to open %s", argv[2]);
		perror(", ");
		return -3;
	}

	struct stat stat_buf;
	if (fstat(fileno(in1), &stat_buf) == -1) {
		fprintf(stderr, "Error: Unable to stat %s\n", argv[1]);
		fclose(in1);
		fclose(in2);
		return -3;
	}
	if (!S_ISREG(stat_buf.st_mode)) {
		fprintf(stderr, "Error: I'm not able to work with anything but regular files. (%s)\n", argv[1]);
		fclose(in1);
		fclose(in2);
		return -3;
	}
	const size_t in1_size = stat_buf.st_size;
	if (fstat(fileno(in2), &stat_buf) == -1) {
		fprintf(stderr, "Error: Unable to stat %s\n", argv[2]);
		fclose(in1);
		fclose(in2);
		return -3;
	}
	if (!S_ISREG(stat_buf.st_mode)) {
		fprintf(stderr, "Error: I'm not able to work with anything but regular files. (%s)\n", argv[2]);
		fclose(in1);
		fclose(in2);
		return -3;
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
			return -3;
		}
		if (fin2.size == 0) {
			fclose(in1);
			fclose(in2);
			fprintf(stderr, "Error: I can't work with zero-length file %s.\n", argv[2]);
			return -3;
		}
	}
	if (lseek(fin1.fd, 0, SEEK_DATA) == -1 && errno == ENXIO) {
		fclose(in1);
		fclose(in2);
		fprintf(stderr, "Error: File is non-zero but is completely sparse, with no data:\n\t%s.\n", argv[1]);
		return -3;
	}
	if (lseek(fin2.fd, 0, SEEK_DATA) == -1 && errno == ENXIO) {
		fclose(in1);
		fclose(in2);
		fprintf(stderr, "Error: File is non-zero but is completely sparse, with no data:\n\t%s.\n", argv[2]);
		return -3;
	}
	

	const uint8_t *const in1map = mmap(NULL, fin1.size, PROT_READ, MAP_PRIVATE | MAP_NORESERVE | MAP_NONBLOCK, fin1.fd, 0);
	if (in1map == MAP_FAILED) {
		fprintf(stderr, "Error: unable to mmap %s, ", argv[1]);
		perror("");
		fclose(in1);
		fclose(in2);
		return -4;
	}
	madvise((void *)in1map, fin1.size, MADV_DONTDUMP);

	const uint8_t *const in2map = mmap(NULL, fin2.size, PROT_READ, MAP_PRIVATE | MAP_NORESERVE | MAP_NONBLOCK, fin2.fd, 0);
	if (in2map == MAP_FAILED) {
		fprintf(stderr, "Error: unable to mmap %s, ", argv[2]);
		perror("");
		munmap((void *)in1map, in1_size);
		fclose(in1);
		fclose(in2);
		return -4;
	}
	madvise((void *)in2map, fin2.size, MADV_DONTDUMP);


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

	// Keep track of how much file data is in each.
	size_t procsz1 = 0, procsz2 = 0;
	bool subset1 = true, subset2=true;
	size_t next_hole = 0;
	
	{
		// Set the first data block.
		size_t data_1 = lseek(fin1.fd, 0, SEEK_DATA);
		size_t data_2 = lseek(fin2.fd, 0, SEEK_DATA);
		size_t hole_1 = lseek(fin1.fd, data_1, SEEK_HOLE);
		size_t hole_2 = lseek(fin1.fd, data_2, SEEK_HOLE);

		// Don't go beyond the start of the other file's data
		hole_1 = MIN(hole_1, data_2);
		hole_2 = MIN(hole_2, data_1);
		next_hole = MIN(hole_1, hole_2);

		madvise((void *)in1map + data_1, fin1.size - data_1, MADV_SEQUENTIAL | MADV_DONTDUMP);
		madvise((void *)in2map + data_2, fin2.size - data_2, MADV_SEQUENTIAL | MADV_DONTDUMP);

		// Find a byte offset that both files have
		do {

			if (settings.subset || settings.show_greatest) {
				// We need to do accounting - subset checking, and show-greatest.
				if (data_1 < data_2) {
					size_t datasz_off = 0;
					// Compare for null blocks in single file's data
					compnull(next_hole - data_1, in1map + data_1, &datasz_off, !settings.show_greatest);
					if (datasz_off > 0) {
						if (subset2) {
							// There is valid data in data1, before the next block of data2. So it's not a subset.
							subset2 = false;
						}
						if (settings.show_greatest)
							procsz1 += datasz_off;
					}

				}
				else if (data_2 < data_1) {
					size_t datasz_off = 0;
					compnull(next_hole - data_2, in2map + data_2, &datasz_off, !settings.show_greatest);
					if (datasz_off > 0) {
						if (subset1) {
							// data2 has data not in data1. So data2 is not a subset of 1.
							subset1 = false;
						}
						if (settings.show_greatest)
							procsz2 += datasz_off;
					}
				}
			}

			data_1 = lseek(fin1.fd, next_hole, SEEK_DATA);
			data_2 = lseek(fin2.fd, next_hole, SEEK_DATA);
			hole_1 = lseek(fin1.fd, data_1, SEEK_HOLE);
			hole_2 = lseek(fin2.fd, data_2, SEEK_HOLE);
			next_hole = MIN(hole_1, hole_2);

			if (data_1 == (size_t)-1 || data_2 == (size_t)-1) {
				fprintf(stderr, "Error: Files do not share any data blocks.\n");
				return -2;
			}
		} while (data_1 != data_2);

		// they're equal.
		f_off = data_1;

		//already set: next_hole = MIN(hole_1, hole_2);
	}

	const off_t max_size = MIN(fin1.size, fin2.size);
	if (f_off >= max_size) {
		// this should result in an ENXIO, above
		fprintf(stderr, "Error: Files do not share any data blocks.\n");
		return -2;
	}

	// Setup: madvise.
	//madvise(in1map, in1_size, MADV_SEQUENTIAL);
	//madvise(in2map, in2_size, MADV_SEQUENTIAL);
	off_t madv_off = 0;
	{
		// At the moment, f_off is a pull-page or 0, so it's aligned.
		const size_t remaining = MIN(4<<20, max_size - f_off);
		if (remaining > (4 << 20)) {
			madvise((void *)in1map + f_off, remaining, MADV_WILLNEED);
			madvise((void *)in2map + f_off, remaining, MADV_WILLNEED);
			madv_off = f_off + remaining;
		}
	}

	if (f_off > PAGE_SIZE) {
		// We haven't unmapped anything yet, so take care of it.
		mumap(in1map, in2map, f_off, &unmap_off);
	}

	// We stop at the last part of the file with data. After that, they're null-equal.
	while (f_off < max_size) {
		// Informational, /proc/.../fdinfo/
		lseek(fin1.fd, f_off, SEEK_SET);
		lseek(fin2.fd, f_off, SEEK_SET);

		if (f_off >= next_hole) {
			size_t in1_hole = 0, in2_hole = 0;

			size_t new_off = find_next_data(&fin1, &in1_hole, f_off);
			size_t new_off2 = find_next_data(&fin2, &in2_hole, f_off);
						
			if ((new_off == -1 || new_off2 == -1) && (new_off != -1 || new_off2 != -1)) {
				// accounting,
				if (settings.subset) {
					if (new_off == -1) {
						// there's data in 2 that isn't in 1.
						subset2 = true;
					}
					else if (new_off2 == -1) {
						subset1 = true;
					}
				}

				int finfd;
				size_t fin_off, fin_hole, finsize;
				const uint8_t *finmap;

				if (new_off == -1) {
					// process the remainder of file2 and add its size into the processed size
					// NOTE! We're only considering non-zero pages. Not individual bytes.
					finfd = fin2.fd;
					fin_off = new_off2;
					finsize = fin2.size;
					finmap = in2map;
				}
				else {
					// new_off2 is -1
					finfd = fin1.fd;
					fin_off = new_off;
					finsize = fin1.size;
					finmap = in1map;
				}

				do {
					lseek(finfd, fin_off, SEEK_SET);
					fin_hole = lseek(finfd, fin_off, SEEK_HOLE);

					if (fin_off - unmap_off > PAGE_SIZE)
						mumap(in1map, in2map, fin_off, &unmap_off);

					madvise((void *)finmap + fin_off, fin_hole - fin_off, MADV_WILLNEED);

					size_t zerooffcmp = fin_off;
					// Data is page-aligned, check the pages for null-pages.
					while (fin_off + zerooffcmp < fin_hole) {
						// Examine this block. Is it a zero-page? Then it *could* be a hole, but isn't.
						// Don't count it as allocated.
					   	if (memcmp(zero, finmap + fin_off + zerooffcmp, PAGE_SIZE) == 0)
							finsize -= PAGE_SIZE;

						zerooffcmp += PAGE_SIZE;
					}

					// next hole will be at eof, or earlier
					finsize += fin_hole - fin_off;
				} while ((fin_off = lseek(finfd, fin_hole, SEEK_DATA)) > fin_hole);

				
				// And we're done, because the rest of the file is the same - so we're done.
				break;
			}
			else if (new_off == -1 && new_off2 == -1) {
				// Empty to end-of-file.
				break;
			}

			f_off = MIN(new_off, new_off2);

			next_hole = MIN(in1_hole, in2_hole);

			if (unlikely(next_hole == (size_t)(off_t)-1))
				next_hole = max_size;
		}

		// unmap_off is always aligned, so I don't need to align this.
		if (f_off - unmap_off >= PAGE_SIZE) {
			// I check this every 1MB -- or after every hole
			mumap(in1map, in2map, f_off, &unmap_off);
		}

		// madv_off is the end of the _SEQUENTIAL range. update it every 1MB.
		if (f_off - madv_off > (1 << 20)) {
			madv_off = f_off & PAGE_SIZE_bits_not;
			const int madv_len = MIN(2 << 20, next_hole - madv_off);

			// These were "willneed", now they're "sequential"
			madvise((void *)in1map + madv_off, madv_len, MADV_SEQUENTIAL);
			madvise((void *)in2map + madv_off, madv_len, MADV_SEQUENTIAL);

			if (max_size > (2 << 20)) {
				const int madv_len = MIN(2 << 20, next_hole - (madv_off + (2 << 20)));
				madvise((void *)in1map + madv_off + (2 << 20), madv_len, MADV_WILLNEED);
				madvise((void *)in2map + madv_off + (2 << 20), madv_len, MADV_WILLNEED);
			}
		}


		// compare 1MB at a time, and then loop for madvise.
		// Take the aligned 1MB block, add 1MB to that, and min of that and next_hole.
		int compsize = MIN(MIN(max_size, 1 << 20), next_hole - f_off);
		//int compsize = MIN((f_off & ~((size_t)(1 << 20) - 1)) + (1 << 20), next_hole) - f_off;
		int compblock = MIN(PAGE_SIZE, compsize);
		while (compsize > 0) {
			bool is_ok = false;
		   	if (memcmp(in1map + f_off, in2map + f_off, compblock) == 0) {
				// Both null.
				is_ok = true;
			}
			else if (memcmp(in1map + f_off, zero, compblock) == 0) {
				if (settings.show_greatest)
					procsz2 += compblock;
				if (subset2)
					subset2 = false;
				is_ok = true;
			}
			else if (memcmp(in2map + f_off, zero, compblock) == 0) {
				if (settings.show_greatest)
					procsz1 += compblock;
				if (subset1)
					subset1 = false;
				is_ok = true;
			}
			if (!is_ok) {
				// The blocks mismatch and neither is null. Stop the loop!
				break;
			}

			f_off += compblock;
			compsize -= compblock;

			if (unlikely(compsize < compblock))
				compblock = compsize;
		}

		if (f_off - unmap_off >= PAGE_SIZE) {
			// I check this every 1MB -- or after every hole
			mumap(in1map, in2map, f_off, &unmap_off);
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
					if (settings.show_greatest)
						procsz2 += blocksize;	// Block2 is not null.
					if (subset2)
						subset2 = false;
					//fwrite(in2buf + checked, bufavail, 1, stdout);
					goto recalc_blocksize;
				}
				if (memcmp(in2map + blockoff, zero, blocksize) == 0) {
					checked += blocksize;
					if (settings.show_greatest)
						procsz1 += blocksize;	// Block 1 is not null.
					if (subset1)
						subset1 = false;
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
					// procsz1 += 1; procsz2 += 1;
					//fwrite(in1buf + checked + i, 1, 1, stdout);
					continue;
				}
				else if (in1buf[i] == 0) {
					//fwrite(in2buf + checked + i, 1, 1, stdout);
					if (settings.show_greatest)
						procsz2 += 1;
					if (subset2)
						subset2 = false;
					continue;
				}
				else if (0 == in2buf[i]) {
					//fwrite(in1buf + checked + i, 1, 1, stdout);
					if (settings.show_greatest)
						procsz1 += 1;
					if (subset1)
						subset1 = false;
					continue;
				}

				// We have a file mis-match. This isn't permissible.
				fprintf(stderr, "Files mismatch\n");
				fprintf(stderr, "Files mismatch (at byte %li)\n", blockoff + i);

				munmap((void *)in1map + unmap_off, fin1.size - unmap_off);
				munmap((void *)in2map + unmap_off, fin2.size - unmap_off);
				fclose(fin1.f_in);
				fclose(fin2.f_in);

				free((void *)zero);

				return -1;
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
	  
	// TODO: I've checked up to the _shared_ max size. Now I need to handle any additional, if I'm looking for
	// "greatest data size" between the files.
	if (settings.show_greatest) {
		if (fin1.size > max_size) {
			size_t f_off = max_size;
			do {
				f_off = lseek(fin1.fd, f_off, SEEK_DATA);
				if (f_off == -1)
					break;
				next_hole = lseek(fin1.fd, f_off, SEEK_HOLE);
				while (f_off < next_hole) {
					size_t computed;
					const int blocksz = MIN(PAGE_SIZE, next_hole - f_off);
					compnull(blocksz, in1map + f_off, &computed, false);
					procsz1 += computed;
					f_off += blocksz;
				}
			} while (f_off < fin1.size);
		}
		// No else - it's possible that we stopped before end-of-file on both of them, because one had
		// a hole at the end.
		if (fin2.size > max_size) {
			size_t f_off = max_size;
			do {
				f_off = lseek(fin2.fd, f_off, SEEK_DATA);
				if (f_off == -1)
					break;
				next_hole = lseek(fin2.fd, f_off, SEEK_HOLE);
				while (f_off < next_hole) {
					size_t computed;
					const int blocksz = MIN(PAGE_SIZE, next_hole - f_off);
					compnull(blocksz, in2map + f_off, &computed, false);
					procsz1 += computed;
					f_off += blocksz;
				}
			} while (f_off < fin2.size);
		}
	}

	// TODO: Insert file-length check.

	printf("Files are the same, possibly excluding null bytes.\n");

	unsigned char retcode = 0;
	if (settings.show_greatest) {
		if (procsz1 > procsz2) {
			printf("File 1 has more data that file 2.\n");
			retcode |= RET_GREATEST_1;
		}
		else if (procsz2 > procsz1) {
			printf("File 2 has more data that file 1.\n");
			retcode |= RET_GREATEST_2;
		}
	}
	if (subset1)
		retcode |= RET_SUBSET_1;
	if (subset2)
		retcode |= RET_SUBSET_2;

	free((void *)zero);

	if (fin1.size - unmap_off > 0)
		munmap((void *)in1map + unmap_off, fin1.size - unmap_off);
	if (fin2.size - unmap_off > 0)
		munmap((void *)in2map + unmap_off, fin2.size - unmap_off);

	fclose(fin1.f_in);
	fclose(fin2.f_in);

	return retcode;

err:
	if (zero != nullptr)
		free((void *)zero);

	if (in1map != nullptr && fin1.size - unmap_off > 0)
		munmap((void *)in1map + unmap_off, fin1.size - unmap_off);
	if (in2map != nullptr && fin2.size - unmap_off > 0)
		munmap((void *)in2map + unmap_off, fin2.size - unmap_off);
	fclose(fin1.f_in);
	fclose(fin2.f_in);

	return 1;
}
