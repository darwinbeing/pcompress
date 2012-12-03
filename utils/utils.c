/*
 * This file is a part of Pcompress, a chunked parallel multi-
 * algorithm lossless compression and decompression program.
 *
 * Copyright (C) 2012 Moinak Ghosh. All rights reserved.
 * Use is subject to license terms.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * moinakg@belenix.org, http://moinakg.wordpress.com/
 *      
 * This program includes partly-modified public domain source
 * code from the LZMA SDK: http://www.7-zip.org/sdk.html
 */

#include <sys/types.h>
#include <sys/param.h>
#include <fcntl.h>
#include <time.h>
#include <libgen.h>
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include <link.h>
#include <rabin_dedup.h>

#include "utils.h"

void
err_exit(int show_errno, const char *format, ...)
{
	int err = errno;
	va_list args;

	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);

	if (show_errno)
		fprintf(stderr, "\nError: %s\n", strerror(err));
	exit(1);
}

/*
 * Fetch the command name that started the current process.
 * The returned string must be freed by the caller.
 */
const char *
get_execname(const char *argv0)
{
	char path[MAXPATHLEN];
	char apath[128];
	char *tmp1, *tmp2;
	pid_t pid;

	/* The easiest case: we are in linux */
	if (readlink("/proc/self/exe", path, MAXPATHLEN) != -1) {
		return (strdup(basename(path)));
	}

	/* Next easy case: Solaris/Illumos */
	pid = getpid();
	sprintf(apath, "/proc/%d/path/a.out", pid);
	if (readlink(apath, path, MAXPATHLEN) != -1) {
		return (strdup(basename(path)));
	}

	/* Oops... not in linux, not in Solaris no  guarantee */
	/* check if we have something like execve("foobar", NULL, NULL) */
	if (argv0 == NULL) {
		/* Give up */
		return (strdup("Unknown"));
	}

	tmp1 = strdup(argv0);
	tmp2 = strdup(basename(tmp1));
	free(tmp1);
	return (tmp2);
}

/*
 * Routines to parse a numeric string which can have the following suffixes:
 * k - Kilobyte
 * m - Megabyte
 * g - Gigabyte
 *
 * The number should fit in an ssize_t data type.
 * Numeric overflow is also checked. The routine parse_numeric() returns
 * 1 if there was a numeric overflow.
 */
static int
raise_by_multiplier(ssize_t *val, int mult, int power) {
	ssize_t result;

	while (power-- > 0) {
		result = *val * mult;
		if (result/mult != *val)
			return (1);
		*val = result;
	}
	return (0);
}

int
parse_numeric(ssize_t *val, const char *str)
{
	int i, ovr;
	char *mult;

	*val = strtoll(str, &mult, 0);
	if (*mult != '\0') {
		switch (*mult) {
		  case 'k':
		  case 'K':
			ovr = raise_by_multiplier(val, 1024, 1);
			break;
		  case 'm':
		  case 'M':
			ovr = raise_by_multiplier(val, 1024, 2);
			break;
		  case 'g':
		  case 'G':
			ovr = raise_by_multiplier(val, 1024, 3);
			break;
		  default:
			ovr = 2;
		}
	}

	return (ovr);
}

/*
 * Convert number of bytes into human readable format
 */
char *
bytes_to_size(uint64_t bytes)
{
	static char num[20];
	uint64_t kilobyte = 1024;
	uint64_t megabyte = kilobyte * 1024;
	uint64_t gigabyte = megabyte * 1024;
	uint64_t terabyte = gigabyte * 1024;

	if (bytes < kilobyte) {
		sprintf(num, "%" PRIu64 " B", bytes);

	} else if (bytes < megabyte) {
		sprintf(num, "%" PRIu64 " KB", bytes / kilobyte);

	} else if (bytes < gigabyte) {
		sprintf(num, "%" PRIu64 " MB", bytes / megabyte);

	} else if (bytes < terabyte) {
		sprintf(num, "%" PRIu64 " GB", bytes / gigabyte);

	} else {
		sprintf(num, "%" PRIu64 " B", bytes);
	}
	return (num);
}

/*
 * Read/Write helpers to ensure a full chunk is read or written
 * unless there is an error.
 * Additionally can be given an offset in the buf where the data
 * should be inserted.
 */
ssize_t
Read(int fd, void *buf, size_t count)
{
	ssize_t rcount, rem;
	uchar_t *cbuf;
	va_list args;

	rem = count;
	cbuf = (uchar_t *)buf;
	do {
		rcount = read(fd, cbuf, rem);
		if (rcount < 0) return (rcount);
		if (rcount == 0) break;
		rem = rem - rcount;
		cbuf += rcount;
	} while (rem);
	return (count - rem);
}

/*
 * Read the requested chunk and return the last rabin boundary in the chunk.
 * This helps in splitting chunks at rabin boundaries rather than fixed points.
 * The request buffer may have some data at the beginning carried over from
 * after the previous rabin boundary.
 */
ssize_t
Read_Adjusted(int fd, uchar_t *buf, size_t count, ssize_t *rabin_count, void *ctx)
{
        char *buf2;
        ssize_t rcount;
        dedupe_context_t *rctx = (dedupe_context_t *)ctx;

        if (!ctx)  return (Read(fd, buf, count));
        buf2 = buf;
        if (*rabin_count) {
                buf2 = (char *)buf + *rabin_count;
                count -= *rabin_count;
        }
        rcount = Read(fd, buf2, count);
        if (rcount > 0) {
                rcount += *rabin_count;
		if (rcount == count)
			dedupe_compress(rctx, buf, &rcount, 0, rabin_count);
		else
			*rabin_count = 0;
        } else {
                if (rcount == 0) rcount = *rabin_count;
                *rabin_count = 0;
        }
        return (rcount);
}

ssize_t
Write(int fd, const void *buf, size_t count)
{
	ssize_t wcount, rem;
	uchar_t *cbuf;

	rem = count;
	cbuf = (uchar_t *)buf;
	do {
		wcount = write(fd, cbuf, rem);
		if (wcount < 0) return (wcount);
		rem = rem - wcount;
		cbuf += wcount;
	} while (rem);
	return (count - rem);
}

/*
 * Thread sizing. We want a balanced combination of chunk threads and compression
 * algorithm threads that best fit the available/allowed number of processors.
 */
void
set_threadcounts(algo_props_t *props, int *nthreads, int nprocs, algo_threads_type_t typ) {
	int mt_capable;

	if (typ == COMPRESS_THREADS)
		mt_capable = props->compress_mt_capable;
	else
		mt_capable = props->decompress_mt_capable;

	if (mt_capable) {
		int nthreads1, p_max;

		if (nprocs == 3) {
			props->nthreads = 1;
			*nthreads = 3;
			return;
		}

		if (typ == COMPRESS_THREADS)
			p_max = props->c_max_threads;
		else
			p_max = props->d_max_threads;

		nthreads1 = 1;
		props->nthreads = 1;
		while (nthreads1 < *nthreads || props->nthreads < p_max) {
			if ((props->nthreads+1) * nthreads1 <= nprocs && props->nthreads < p_max) {
				props->nthreads++;

			} else if (props->nthreads * (nthreads1+1) <= nprocs && nthreads1 < *nthreads) {
				nthreads1++;
			} else {
				break;
			}
		}
		*nthreads = nthreads1;

	} else if (props->single_chunk_mt_capable && props->is_single_chunk) {
		*nthreads = 1;
		if (typ == COMPRESS_THREADS)
			props->nthreads = props->c_max_threads;
		else
			props->nthreads = props->d_max_threads;
		if (props->nthreads > nprocs)
			props->nthreads = nprocs;
	}
}

uint64_t
get_total_ram()
{
	uint64_t phys_pages, page_size;

	page_size = sysconf(_SC_PAGESIZE);
	phys_pages = sysconf(_SC_PHYS_PAGES);
	return (phys_pages * page_size);
}