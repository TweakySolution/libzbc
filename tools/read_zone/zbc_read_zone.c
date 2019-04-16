/*
 * This file is part of libzbc.
 *
 * Copyright (C) 2009-2014, HGST, Inc. All rights reserved.
 * Copyright (C) 2016, Western Digital. All rights reserved.
 *
 * This software is distributed under the terms of the
 * GNU Lesser General Public License version 3, "as is," without technical
 * support, and WITHOUT ANY WARRANTY, without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. You should have
 * received a copy of the GNU Lesser General Public License along with libzbc.
 * If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Damien Le Moal (damien.lemoal@wdc.com)
 *         Christophe Louargant (christophe.louargant@wdc.com)
 */

#define _GNU_SOURCE     /* O_LARGEFILE & O_DIRECT */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#include <libzbc/zbc.h>

/**
 * I/O abort.
 */
static int zbc_read_zone_abort = 0;

/**
 * System time in usecs.
 */
static inline unsigned long long zbc_read_zone_usec(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);

	return (unsigned long long) tv.tv_sec * 1000000LL +
		(unsigned long long) tv.tv_usec;
}

/**
 * Signal handler.
 */
static void zbc_read_zone_sigcatcher(int sig)
{
	zbc_read_zone_abort = 1;
}

int main(int argc, char **argv)
{
	struct zbc_device_info info;
	struct zbc_device *dev = NULL;
	unsigned long long elapsed;
	unsigned long long bcount = 0;
	unsigned long long brate;
	int zidx;
	int fd = -1, i;
	ssize_t ret = 1;
	size_t iosize;
	void *iobuf = NULL;
	ssize_t sector_count;
	unsigned long long ionum = 0, iocount = 0;
	struct zbc_zone *zones = NULL;
	struct zbc_zone *iozone = NULL;
	unsigned int nr_zones;
	char *path, *file = NULL;
	long long sector_ofst = 0;
	long long sector_max = 0;
	int flags = O_RDONLY;
	bool fvio = false;

	/* Check command line */
	if (argc < 4) {
usage:
		printf("Usage: %s [options] <dev> <zone no> <I/O size (B)>\n"
		       "  Read a zone up to the current write pointer\n"
		       "  or the number of I/O specified is executed\n"
		       "Options:\n"
		       "    -v         : Verbose mode\n"
			   "    -vio       : Use vector I/O interface\n"
		       "    -dio       : Use direct I/Os\n"
		       "    -nio <num> : Limit the number of I/Os to <num>\n"
		       "    -f <file>  : Write the content of the zone to <file>\n"
		       "                 If <file> is \"-\", the zone content is\n"
		       "                 written to the standard output\n"
		       "    -ofst      : sector offset from the start sector of\n"
		       "                 the zone (default 0 or write pointer)\n",
		       argv[0]);
		return 1;
	}

	/* Parse options */
	for (i = 1; i < (argc - 1); i++) {

		if (strcmp(argv[i], "-v") == 0) {

			zbc_set_log_level("debug");

		} else if (strcmp(argv[i], "-dio") == 0) {

			flags |= O_DIRECT;

		} else if (strcmp(argv[i], "-vio") == 0) {

			fvio = true;

		} else if (strcmp(argv[i], "-nio") == 0) {

			if (i >= (argc - 1))
				goto usage;
			i++;

			ionum = atoi(argv[i]);
			if (ionum <= 0) {
				fprintf(stderr, "Invalid number of I/Os\n");
				return 1;
			}

		} else if (strcmp(argv[i], "-f") == 0) {

			if (i >= (argc - 1))
				goto usage;
			i++;

			file = argv[i];

		} else if (strcmp(argv[i], "-ofst") == 0) {

			if (i >= (argc - 1))
				goto usage;
			i++;

			sector_ofst = atoll(argv[i]);
			if (sector_ofst < 0) {
				fprintf(stderr, "Invalid sector offset\n");
				return 1;
			}

		} else if (argv[i][0] == '-') {

			fprintf(stderr, "Unknown option \"%s\"\n", argv[i]);
			goto usage;

		} else {

			break;

		}

	}

	if (i != (argc - 3))
		goto usage;

	/* Get parameters */
	path = argv[i];
	zidx = atoi(argv[i + 1]);
	if (zidx < 0) {
		fprintf(stderr, "Invalid zone number %s\n", argv[i + 1]);
		return 1;
	}

	iosize = atol(argv[i + 2]);
	if (!iosize) {
		fprintf(stderr, "Invalid I/O size %s\n", argv[i + 2]);
		return 1;
	}

	/* Setup signal handler */
	signal(SIGQUIT, zbc_read_zone_sigcatcher);
	signal(SIGINT, zbc_read_zone_sigcatcher);
	signal(SIGTERM, zbc_read_zone_sigcatcher);

	/* Open device */
	ret = zbc_open(path, flags, &dev);
	if (ret != 0) {
		if (ret == -ENODEV)
			fprintf(stderr,
				"Open %s failed (not a zoned block device)\n",
				path);
		else
			fprintf(stderr, "Open %s failed (%s)\n",
				path, strerror(-ret));
		return 1;
	}

	zbc_get_device_info(dev, &info);

	printf("Device %s:\n", path);
	zbc_print_device_info(&info, stdout);

	/* Get zone list */
	ret = zbc_list_zones(dev, 0, ZBC_RO_ALL, &zones, &nr_zones);
	if (ret != 0) {
		fprintf(stderr, "zbc_list_zones failed\n");
		ret = 1;
		goto out;
	}

	/* Get target zone */
	if ((unsigned int)zidx >= nr_zones) {
		fprintf(stderr, "Target zone not found\n");
		ret = 1;
		goto out;
	}
	iozone = &zones[zidx];

	if (zbc_zone_conventional(iozone))
		printf("Target zone: Conventional zone %d / %d, "
		       "sector %llu, %llu sectors\n",
		       zidx,
		       nr_zones,
		       zbc_zone_start(iozone),
		       zbc_zone_length(iozone));
	else
		printf("Target zone: Zone %d / %d, type 0x%x (%s), "
		       "cond 0x%x (%s), rwp %d, non_seq %d, "
		       "sector %llu, %llu sectors, wp %llu\n",
		       zidx,
		       nr_zones,
		       zbc_zone_type(iozone),
		       zbc_zone_type_str(zbc_zone_type(iozone)),
		       zbc_zone_condition(iozone),
		       zbc_zone_condition_str(zbc_zone_condition(iozone)),
		       zbc_zone_rwp_recommended(iozone),
		       zbc_zone_non_seq(iozone),
		       zbc_zone_start(iozone),
		       zbc_zone_length(iozone),
		       zbc_zone_wp(iozone));

	/* Check I/O alignment and get an I/O buffer */
	if (iosize % info.zbd_lblock_size) {
		fprintf(stderr,
			"Invalid I/O size %zu (must be a multiple of %u B)\n",
			iosize,
			(unsigned int) info.zbd_lblock_size);
		ret = 1;
		goto out;
	}

	ret = posix_memalign((void **) &iobuf, sysconf(_SC_PAGESIZE), iosize);
	if ( ret != 0 ) {
		fprintf(stderr, "No memory for I/O buffer (%zu B)\n", iosize);
		ret = 1;
		goto out;
	}

	/* Open the file to write, if any */
	if (file) {

		if (strcmp(file, "-") == 0) {

			fd = fileno(stdout);
			printf("Writting target zone %d to standard output, "
			       "%zu B I/Os\n",
			       zidx, iosize);

		} else {

			fd = open(file,
				  O_CREAT | O_TRUNC | O_LARGEFILE | O_WRONLY,
				  S_IRUSR | S_IWUSR | S_IRGRP);
			if (fd < 0) {
				fprintf(stderr, "Open file \"%s\" failed %d (%s)\n",
					file,
					errno,
					strerror(errno));
				ret = 1;
				goto out;
			}

			printf("Writting target zone %d to file \"%s\", "
			       "%zu B I/Os\n",
			       zidx, file, iosize);

		}

	} else if (!ionum) {

		printf("Reading target zone %d, %zu B I/Os\n",
		       zidx, iosize);

	} else {

		printf("Reading target zone %d, %llu I/Os of %zu B\n",
		       zidx, ionum, iosize);

	}

	if (zbc_zone_sequential_req(iozone) &&
	    !zbc_zone_full(iozone))
		sector_max = zbc_zone_wp(iozone) - zbc_zone_start(iozone);
	else
		sector_max = zbc_zone_length(iozone);

	elapsed = zbc_read_zone_usec();

	while (!zbc_read_zone_abort &&
	       sector_ofst < sector_max) {

		sector_count = iosize >> 9;
		if (sector_ofst + sector_count > sector_max)
			sector_count = sector_max - sector_ofst;

		/* Read zone */
		if (fvio &&
			sector_count >= 2 * (info.zbd_lblock_size >> 9)) {

			struct iovec iov[2];
			iov[0].iov_base = iobuf;
			iov[0].iov_len = sector_count / 2;
			iov[1].iov_base = (uint8_t *) iobuf + (iov[0].iov_len << 9);
			iov[1].iov_len = sector_count - sector_count / 2;

			ret = zbc_preadv(dev, iov, 2, 
					zbc_zone_start(iozone) + sector_ofst);
		} else
			ret = zbc_pread(dev, iobuf, sector_count,
					zbc_zone_start(iozone) + sector_ofst);
		if (ret <= 0) {
			fprintf(stderr, "zbc_pread failed %zd (%s)\n",
				-ret, strerror(-ret));
			ret = 1;
			break;
		}
		sector_count = ret;

		if (file) {
			/* Write zone data to output file */
			ret = write(fd, iobuf, sector_count << 9);
			if (ret < 0) {
				fprintf(stderr, "Write file \"%s\" failed %d (%s)\n",
					file,
					errno, strerror(errno));
				ret = 1;
				break;
			}
		}

		sector_ofst += sector_count;
		bcount += sector_count << 9;
		iocount++;
		ret = 0;

		if (ionum > 0 && iocount >= ionum)
			break;

	}

	elapsed = zbc_read_zone_usec() - elapsed;
	if (elapsed) {
		printf("Read %llu B (%llu I/Os) in %llu.%03llu sec\n",
		       bcount,
		       iocount,
		       elapsed / 1000000,
		       (elapsed % 1000000) / 1000);
		printf("  IOPS %llu\n",
		       iocount * 1000000 / elapsed);
		brate = bcount * 1000000 / elapsed;
		printf("  BW %llu.%03llu MB/s\n",
		       brate / 1000000,
		       (brate % 1000000) / 1000);
	} else {
		printf("Read %llu B (%llu I/Os)\n",
		       bcount,
		       iocount);
	}

out:

	if ( file && (fd > 0) ) {
		if (fd != fileno(stdout))
			close(fd);
		if (ret != 0)
			unlink(file);
	}

	if (iobuf)
		free(iobuf);

	if (zones)
		free(zones);

	zbc_close(dev);

	return ret;
}

