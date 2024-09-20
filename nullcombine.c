

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define least(x,y) ( x < y ? x : y)
#define greatest(x,y) ( x < y ? y : x)

// Buf size must be a power of 2.
// It's important to set this to the cluster/sector size.
// If it is that value, then we can write nulls without worrying about sparse-ness.
#define BUF_SIZE	4096	// 2^13

char zero[BUF_SIZE] = {0};

int main(int argc, char **argv) {
	int prefer_side = 0; // -1 if prefer first file; -2 if prefer second
	int argused = 0;
	FILE *in1;
	FILE *in2;

	if (argc > 1 && argv[1][0] == '-') {
		if (argv[1][1] == '1' && argv[1][2] == '\0') {
			argused++;
			prefer_side = -1;
		}
		else if (argv[1][1] == '2' && argv[1][2] == '\0') {
			argused++;
			prefer_side = 1;
		}
	}

	if (argc < 3 + argused || argc > 3 + argused) {
		fprintf(stderr, "Error: You must specify two input files.\n");
		return 1;
	}

	in1 = fopen(argv[1 + argused], "rb");
	in2 = fopen(argv[2 + argused], "rb");

	if (NULL == in1 || ferror(in1)) {
		fprintf(stderr, "Error opening %s\n", argv[1]);
		return 1;
	}
	if (NULL == in2 || ferror(in2)) {
		fprintf(stderr, "Error opening %s\n", argv[2]);
		return 1;
	}
	// Read each, compare
	
	int curblock = 0;
	while (!feof(in1) || !feof(in2)) {
		char in1buf[BUF_SIZE];
		char in2buf[BUF_SIZE];

		int inleft1 = fread(in1buf, 1, BUF_SIZE, in1);
		int inleft2 = fread(in2buf, 1, BUF_SIZE, in2);
		curblock++;

		//fprintf(stderr, "inleft1 %i; inleft2 %i\n", inleft1, inleft2);
		if (inleft2 == 0) {
			if(feof(in1) && feof(in2))
				break;

			do {
				// handle sparse blocks
				if (!memcmp(in1buf, zero, inleft1)) {
					fseek(stdout, BUF_SIZE, SEEK_CUR);
				}
				else {
					fwrite(in1buf, inleft1, 1, stdout);
				}
			} while ((inleft1 = fread(in1buf, 1, BUF_SIZE, in1)) > 0);
			continue;
		}
		else if (inleft1 == 0) {
			if (feof(in2) && feof(in1))
				break;

			do {
				// handle sparse blocks
				if (!memcmp(in2buf, zero, inleft2)) {
					fseek(stdout, BUF_SIZE, SEEK_CUR);
				}
				else {
					fwrite(in2buf, inleft2, 1, stdout);
				}
			} while ((inleft2 = fread(in2buf, 1, BUF_SIZE, in2)) > 0);
			continue;
		}

		// Base case -- blocks are the same
		else if (inleft1 == inleft2) {
			if (!memcmp(in1buf, in2buf, inleft1)) {
				//fprintf(stderr, "curblock: %i; block is the same.\n", curblock);

				if (memcmp(in1buf, zero, inleft1)) {
					// Blocks are the same, so just write one of them.
					fwrite(in1buf, inleft1, 1, stdout);
				}
				else {
					// sparse block
					//fprintf(stderr, "Seeking for sparse in output -- same-buffers\n");
					fseek(stdout, inleft1, SEEK_CUR);
				}

				continue;
			}
		}
		// Else, check parts of the block.

		// The full buffer didn't check out. The first step is to halve the block size.
		int blocksize = BUF_SIZE;
		int checked = 0;
		//fprintf(stderr, "curblock: %i; blocksize: %i\n", curblock, blocksize);
		// blocksize == 0 when we've exhausted a file.
		while (checked < inleft1 && checked < inleft2) {
			blocksize >>= 1;

			// If blocksize is zero, we resized it there because inleftX is none.
			if (blocksize == 0)
				break;

			// As long as we have buffer for it, use the larger comparison functions.
			if (blocksize > inleft1 - checked || blocksize > inleft2 - checked) {
				// Not enough data; shrink the block size..
				// Notice: this may shrink the block size < 16. This is permissible in the
				// end-of-file case; it'll go down to the small-comparison.
				continue;
			}
			//fprintf(stderr, "curblock: %i; blocksize: %i\n", curblock, blocksize);

			// Bufavailable could be zero if we've finished one file.
			if (blocksize >= 16) {
				if (! memcmp(in1buf + checked, in2buf + checked, blocksize)) {
					// The two blocks are the same
					if (!memcmp(in1buf + checked, zero, blocksize)) {
						// zero block. Handle sparseness properly..
						fseek(stdout, blocksize, SEEK_CUR);
						//fprintf(stderr, "fseek bec. in1buf and same\n");
					}
					else {
						//fprintf(stderr, "fwrite same-block\n");
						fwrite(in1buf + checked, blocksize, 1, stdout);
					}
					checked += blocksize;
					goto recalc_blocksize;
				}

				// If either block is full-zero, then halve block size again.
				// This, to write as granular sparse bits as possible.
				// It may be that block1 is half-sparse and block2 full-sparse, and so sparse those portions,
				// rather than writing a full block1 which is half-sparse and half-data.
				if (!memcmp(in1buf + checked, zero, blocksize) || !memcmp(in2buf + checked, zero, blocksize))
					continue;

				if (blocksize == 16) {
					// If we still have a full sparse block, write the other.
					if (! memcmp(in1buf + checked, zero, blocksize)) {
						fwrite(in2buf + checked, blocksize, 1, stdout);
						checked += blocksize;
						goto recalc_blocksize;
					}
					if (! memcmp(in2buf + checked, zero, blocksize)) {
						fwrite(in1buf + checked, blocksize, 1, stdout);
						checked += blocksize;
						goto recalc_blocksize;
					}
				}

				// We didn't goto, ...
				// File mismatch detected in blocks. Shrink block size and try again.

				continue;
			}

			// Not full-sparse, then compare byte-for-byte.
			// Stop worrying about sparse, we don't sparse at the 8-byte level.

			//fprintf(stderr, "curblock: %i; Checking small at index %i, blocksize %i\n", curblock, checked, blocksize);
			// Stop when we've exhausted either file. We'll finish up outside the loop.
			// It's a given that blocksize <= the amount of data that we have left for both files.
			for (int i = 0; i < blocksize; i++) {
				if (in1buf[checked + i] == in2buf[checked + i] || 0 == in2buf[checked + i]) {
					fwrite(in1buf + checked + i, 1, 1, stdout);
					continue;
				}
				else if (in1buf[checked + i] == 0) {
					//fprintf(stderr, "fwrite small bec. in1buf 0.\n");
					fwrite(in2buf + checked + i, 1, 1, stdout);
					continue;
				}

				// We have a file mis-match. If we prefer one over the other, use it.
				if (prefer_side != 0) {
					//fprintf(stderr, "%li+%i: (%i+%i) %i -- %i\n", ftell(in1), checked, blocksize, i, in1buf[checked + i], in2buf[checked + i]);
					if (prefer_side == -1) {
						fwrite(in1buf + checked + i, 1, 1, stdout);
						continue;
					}
					else if (prefer_side == 1) {
						fwrite(in2buf + checked + i, 1, 1, stdout);
						continue;
					}
				}
				// If we don't prefer one file over the other, this isn't permissible.
				fprintf(stderr, "Error: Files mismatch\n");
				fprintf(stderr, "Error: Files mismatch (at byte %i)\n", curblock * BUF_SIZE + i);
				fclose(in1);
				fclose(in2);
				return 1;
			}
			checked += blocksize;

recalc_blocksize:
			if (checked == BUF_SIZE)
				continue;

			// find the next block size.
			while ( blocksize <= least(inleft1, inleft2) - checked && blocksize <= BUF_SIZE) {
				// We just completed a full block. Bump the blocksize back up.
				blocksize <<= 1;
			}
		} // while we haven't checked whole files..

		// Now that we've left the comparison loop, is one file complete?
		if (inleft1 != inleft2) {
			if (inleft1 - checked == 0) {
				//fprintf(stderr, "End of file 1; printing the remains of file2...\n");
				// write the remainder from in2
				fwrite(in2buf + checked, inleft2 - checked, 1, stdout);
				checked = inleft2;
			}
			else if (inleft2 - checked == 0) {
				// write the remainder from in1
				//fprintf(stderr, "End of file 2; printing the remains of file1...\n");
				fwrite(in1buf + checked, inleft1 - checked, 1, stdout);
				checked = inleft1;
			}
		}
	} // while not eof some file

	// We may have had nulls at the end. Set the length equal to the biggest file.
	int filepos = ftell(in1) > ftell(in2) ? ftell(in1) : ftell(in2);
	ftruncate(fileno(stdout), filepos);

	fclose(in1);
	fclose(in2);
	return 0;
}
