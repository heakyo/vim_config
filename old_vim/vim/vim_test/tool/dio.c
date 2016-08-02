#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#include "tool.h"

struct shannon_dev *gdev;

static void sigint_handler(int signo)
{
	struct shannon_dev *dev = gdev;
	struct direct_io dio;

	dio.type = DIRECT_IO_STOP;
	printf("\nStop measure\n");
	dev->do_direct_io(dev, &dio);

	exit(EXIT_SUCCESS);
}


static void shannon_dio_usage(void)
{
	printf("Description:\n");
	printf("\tMeasure read/write bandwidth by direct IO\n\n");

	printf("Usage:\n");
	printf("\tdio write/read [option]\n\n");

	printf("Options:\n");
	printf("\t-t, --absent-luns=loglun|phylun:x1-x2,x3,x4-x5...\n"
		"\t\tSpecify absent luns\n\n");
	printf("\t-T, --present-luns=loglun|phylun:x1-x2,x3,x4-x5...\n"
		"\t\tSpecify present luns\n\n");
	printf("\t-h, --help\n"
		"\t\tdisplay this help and exit\n\n");
}

int shannon_dio(struct shannon_dev *dev, int argc, char **argv)
{
	struct option longopts [] = {
		{"absent-luns", required_argument, NULL, 't'},
		{"present-luns", required_argument, NULL, 'T'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int lun;
	int opt;
	struct direct_io dio;
	struct timeval init_tv, tv;
	unsigned long done_ppa, init_done_ppa;
	int ts, tms, avtms, speed, avspeed;
	char display[1024];
	int validluns, doneluns;

	while ((opt = getopt_long(argc, argv, ":f:aNst:T:h", longopts, NULL)) != -1) {
		switch (opt) {
		case 't':
			present_absent_luns(dev, optarg, 1);
			break;
		case 'T':
			present_absent_luns(dev, optarg, 0);
			break;
		case 'h':
			shannon_dio_usage();
			return 0;
		default:
			shannon_dio_usage();
			return ERR;
		}
	}

	gdev = dev;
	if (signal(SIGINT, sigint_handler) == SIG_ERR) {
		printf("Can`t register user`s SIGINT handler\n");
		exit(EXIT_FAILURE);
	}

	dio.type = DIRECT_IO_START;
	dio.opcode = sh_read_cmd;
	dio.head = 0;
	dio.nblk = dev->flash->nblk;
	dio.npage = dev->flash->npage;
	dio.nplane = dev->config->nplane;
	dio.nsector = dev->config->page_nsector;
	dio.sector_size = dev->config->sector_size;

	memset(dio.phylun_bitmap, 0xFF, sizeof(dio.phylun_bitmap));
	for_dev_each_lun(dev, lun)
		clear_bit(log2phy_lun(dev, lun), dio.phylun_bitmap);

	dev->do_direct_io(dev, &dio);
	sleep(1);

	/* poll then print bandwidth */
	ts = 0;
	tms = 0;

	dio.type = DIRECT_IO_POLL;
	dev->do_direct_io(dev, &dio);

	init_tv.tv_sec = tv.tv_sec = dio.tv.tv_sec;
	init_tv.tv_usec = tv.tv_usec = dio.tv.tv_usec;
	init_done_ppa = done_ppa = dio.done_ppa;

	printf("%s %s ", (dev->config->nplane > 1) ? "MP" : "SP", (sh_write_cmd == dio.opcode) ? "WR" : "RD");
	printf("\033[s");	// display: save cursor location

	validluns = 0;
	for_dev_each_lun(dev, lun)
		validluns++;

	while (1) {
		sleep(1);
		ts++;

		dio.type = DIRECT_IO_POLL;
		dev->do_direct_io(dev, &dio);
		tms = ((dio.tv.tv_sec * 1000000 + dio.tv.tv_usec) - (tv.tv_sec * 1000000 + tv.tv_usec)) / 1000;
		avtms = ((dio.tv.tv_sec * 1000000 + dio.tv.tv_usec) - (init_tv.tv_sec * 1000000 + init_tv.tv_usec)) / 1000;
		tv.tv_usec = dio.tv.tv_usec;
		tv.tv_sec = dio.tv.tv_sec;

		speed = ((dio.done_ppa - done_ppa) * dev->config->ndata) / (tms * 1024);
		avspeed = ((dio.done_ppa - init_done_ppa) * dev->config->ndata) / (avtms * 1024);
		done_ppa = dio.done_ppa;

		doneluns = 0;
		for_dev_each_lun(dev, lun) {
			if (test_bit(log2phy_lun(dev, lun), dio.done_phylun_bitmap)) {
				// printf("lun %d phylun %d complete\n", lun, log2phy_lun(dev, lun));
				doneluns++;
			}
		}

		sprintf(display, "[%ds]: %dMB/s, average %dMB/s, %d luns complete", ts, speed, avspeed, doneluns);
		printf("\033[u");	// display: restore cursor location
		printf("\033[K");	// display: clear cursor to line tail
		print("%s", display);

		if (doneluns >= validluns)
			break;
	}
	printf("\n");

	/* stop dio if here */
	dio.type = DIRECT_IO_STOP;
	dev->do_direct_io(dev, &dio);
	return 0;
}
/*-----------------------------------------------------------------------------------------------------------*/
