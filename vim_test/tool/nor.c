#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#include "tool.h"

#define	NOR_ERASE_ADDR	0x34
#define	NOR_ERASE_CTRL	0x35

#define	NOR_WRITE_ADDR	0x36
#define	NOR_WRITE_CTRL	0x37

#define	NOR_READ_ADDR	0x38
#define	NOR_READ_LENGTH	0x39
#define	NOR_READ_CTRL	0x3A

#define	NOR_TIMEOUT	100000	// timeout about 5s
/*-----------------------------------------------------------------------------------------------------------*/
struct norflash {
	char name[32];
	int total_block;
	int total_size;
	int block_type;
	int dbuf_size;

	struct norblock {
		int count;
		int size;
	} block[0];
};

enum {
	nor_erase_type = 0,
	nor_write_type,
	nor_read_type,
};

static int nor_buf_addr = 0;
/*-----------------------------------------------------------------------------------------------------------*/
/*
 * convert u8 from little endian to big-endian
 */
void __u8_le2be(void *buf, int count)
{
	int i, k;
	__u8 data, t;

	for (i = 0; i < count; i++) {
		t = 0;
		data = ((__u8 *)buf)[i];

		for (k = 0; k < 8; k++) {
			if (data & (1 << k))
				t |= (1 << (7 - k));
		}

		((__u8 *)buf)[i] = t;
	}
}

/*-----------------------------------------------------------------------------------------------------------*/
static struct norflash *init_norflash(char *name)
{
	struct norflash *nor = NULL;

	if (!strcmp(name, "spi")) {
		nor = zmalloc(sizeof(*nor) + sizeof(struct norblock) * (1/*64KB*/ + 1/*4KB*/ + 1/*tag*/));
		if (NULL == nor)
			return NULL;

		sprintf(nor->name, "%s", name);

		nor->total_block = 542;
		nor->total_size = 510 * 64 * 1024 + 32 * 4 * 1024;	// byte
		nor->block_type = 2;

		nor->block[0].count = 510;
		nor->block[0].size = 64 * 1024;

		nor->block[1].count = 32;
		nor->block[1].size = 4 * 1024;

		nor->block[2].count = 0;
		nor->block[2].size = 0;

		nor->dbuf_size = 256;

	} else if (!strcmp(name, "bpi")) {
		nor = zmalloc(sizeof(*nor) + sizeof(struct norblock) * (1/*128KB*/ + 1/*tag*/));
		if (NULL == nor)
			return NULL;

		sprintf(nor->name, "%s", name);

		nor->total_block = 256;
		nor->total_size = 256 * 128 * 1024;	// byte
		nor->block_type = 1;

		nor->block[0].count = 256;
		nor->block[0].size = 128 * 1024;

		nor->block[1].count = 0;
		nor->block[1].size = 0;

		nor->dbuf_size = 64;
	}

	return nor;
}

static void exit_norflash(struct norflash *nor)
{
	if (NULL != nor)
		free(nor);
}

/*-----------------------------------------------------------------------------------------------------------*/
/* 
 * NOR erase
 * start, byte address
 * length, byte number
 */
static int nor_erase(struct shannon_dev *dev, struct norflash *nor, int start, int length)
{
	int timeout;
	int size, ssize, type, stype;
	int begin, remain;
	int ncount, count, now, pre;

	if ((start + length) > nor->total_size) {
		printf("Overflow NOR address\n");
		return ERR;
	}

	for (size = 0, type = 0; type < nor->block_type; type++) {
		size += nor->block[type].size * nor->block[type].count;
		if (start < size)
			break;
	}
	assert(type < nor->block_type);
	stype = type;
	ssize = size;

	if (start % nor->block[type].size) {
		printf("Start address should align 0x%X\n", nor->block[type].size);
		return ERR;
	}

	/* check block number to be erased */
	ncount = 0;
	begin = start;
	remain = length;

	while (remain > 0) {
		begin += nor->block[type].size;
		remain -= nor->block[type].size;

		if (begin >= size) {
			type++;
			size += nor->block[type].size * nor->block[type].count;
		}

		ncount++;
	}

	/* erase block one by one */
	type = stype;
	size = ssize;
	count = now = pre = 0;
	begin = start;
	remain = length;
	print("Erase: %%%02d", now);

	while (remain > 0) {
		// printf("begin=0x%X length=0x%X\n", begin, nor->block[type].size);
		dev->iowrite32(dev, begin, NOR_ERASE_ADDR);
		dev->iowrite32(dev, 0, NOR_ERASE_CTRL);
		dev->iowrite32(dev, 1, NOR_ERASE_CTRL);

		timeout = 0;
		while (!(dev->ioread32(dev, NOR_ERASE_CTRL) & 0x02)) {
			usleep(10);
			if (timeout++ > NOR_TIMEOUT) {
				printf("%s() timeout\n", __func__);
				return ERR;
			}
		}

		now = ++count * 100 / ncount;
		if (now > pre) {
			print("\b\b%02d", now);
			pre = now;
		}

		begin += nor->block[type].size;
		remain -= nor->block[type].size;

		if (begin >= size) {
			type++;
			size += nor->block[type].size * nor->block[type].count;
		}
	}

	printf("\n");
	return 0;
}

/* 
 * NOR write
 * start, byte address
 * length, byte number
 */
static int nor_write(struct shannon_dev *dev, struct norflash *nor, void *data, int start, int length)
{
	int timeout;
	int n, begin, remain;
	__u32 buf[nor->dbuf_size / 4];
	int ncount, count, now, pre;

	if ((start + length) > nor->total_size) {
		printf("Overflow NOR address\n");
		return ERR;
	}

	begin = start;
	remain = length;
	memset(buf, 0xFF, sizeof(buf));

	ncount = (length + nor->dbuf_size - 1) / nor->dbuf_size;
	count = now = pre = 0;
	print("Write: %%%02d", now);

	while (remain > 0) {
		n = (remain >= nor->dbuf_size) ? nor->dbuf_size : remain;
		// printf("begin=0x%X length=%d\n", begin, n);

		memcpy(buf, data + (begin - start), n);
		if (dev->private_int)
			__u8_le2be(buf, n);
		dev->multi_raw_writel(dev, buf, nor_buf_addr, (n + 3) / 4);

		dev->iowrite32(dev, begin, NOR_WRITE_ADDR);
		dev->iowrite32(dev, 0, NOR_WRITE_CTRL);
		dev->iowrite32(dev, 1, NOR_WRITE_CTRL);

		timeout = 0;
		while (!(dev->ioread32(dev, NOR_WRITE_CTRL) & 0x02)) {
			usleep(10);
			if (timeout++ > NOR_TIMEOUT) {
				printf("%s() timeout\n", __func__);
				return ERR;
			}
		}

		now = ++count * 100 / ncount;
		if (now > pre) {
			print("\b\b%02d", now);
			pre = now;
		}

		begin += n;
		remain -= n;
	}

	printf("\n");
	return 0;
}

/* 
 * NOR read
 * start, byte address
 * length, byte number
 */
static int nor_read(struct shannon_dev *dev, struct norflash *nor, void *data, int start, int length, int quiet)
{
	int timeout;
	int n, begin, remain;
	__u32 buf[nor->dbuf_size / 4];
	int ncount, count, now, pre;

	if ((start + length) > nor->total_size) {
		printf("Overflow NOR address\n");
		return ERR;
	}

	begin = start;
	remain = length;
	memset(buf, 0xFF, sizeof(buf));

	ncount = (length + nor->dbuf_size - 1) / nor->dbuf_size;
	count = now = pre = 0;
	if (!quiet)
		print("Read: %%%02d", now);

	while (remain > 0) {
		n = (remain >= nor->dbuf_size) ? nor->dbuf_size : remain;
		// printf("begin=0x%X length=0x%XB\n", begin, n);

		dev->iowrite32(dev, begin, NOR_READ_ADDR);
		dev->iowrite32(dev, 4 * ((n + 3) / 4), NOR_READ_LENGTH);
		dev->iowrite32(dev, 0, NOR_READ_CTRL);
		dev->iowrite32(dev, 1, NOR_READ_CTRL);

		timeout = 0;
		while (!(dev->ioread32(dev, NOR_READ_CTRL) & 0x02)) {
			usleep(10);
			if (timeout++ > NOR_TIMEOUT) {
				printf("%s() timeout\n", __func__);
				return ERR;
			}
		}

		dev->multi_raw_readl(dev, buf, nor_buf_addr, (n + 3) / 4);
		if (dev->private_int)
			__u8_le2be(buf, n);
		memcpy(data + (begin - start), buf, n);

		now = ++count * 100 / ncount;
		if (!quiet && now > pre) {
			print("\b\b%02d", now);
			pre = now;
		}

		begin += n;
		remain -= n;
	}

	if (!quiet)
		printf("\n");
	return 0;
}

static void shannon_nor_usage(void)
{
	printf("Usage:\n");
	printf("\t nor [option] erase start(Hex-byte-address) length(Dec-byte-number)\n");
	printf("\t nor [option] write start(Hex-byte-address) file\n");
	printf("\t nor [option] read  start(Hex-byte-address) length(Dec-byte-number) file\n");
	// printf("\t nor check-endian\n\n");

	printf("Option:\n");
	printf("\t-b, --big-endian\n"
		"\t\tread/write 16bit data with big-endian\n\n");
	printf("\t-h, --help\n"
		"\t\tdisplay this help and exit\n");
}

static struct norflash *shannon_nor_init(struct shannon_dev *dev)
{
	__u32 typeid;
	char nortype[16];
	struct norflash *nor = NULL;

	typeid = dev->ioread32(dev, 0x32);

	if (0x01800119 == typeid) {
		sprintf(nortype, "spi");

		nor_buf_addr = 0x800;
	} else if (0x017E2201 == typeid) {
		sprintf(nortype, "bpi");

		nor_buf_addr = 0x800;
	} else {
		sprintf(nortype, "bpi");

		if(0 == dev->bar_dwlen[1]) {
			printf("Hardware don`t support this feature\n");
			exit(EXIT_FAILURE);
		}
		nor_buf_addr = dev->bar_dwlen[0];
	}

	nor = init_norflash(nortype);
	if (NULL == nor) {
		printf("init_norflash failed\n");
		exit(EXIT_FAILURE);
	}

	return nor;
}

int shannon_nor(struct shannon_dev *dev, int argc, char **argv)
{	
	struct option longopts [] = {
		{"big-endian", no_argument, NULL, 'b'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int opt;
	struct norflash *nor = NULL;
	int fd, start, length;
	struct stat sta;
	int rc = 0;
	void *buf;

	dev->private_int = 0;

	while ((opt = getopt_long(argc, argv, ":bh", longopts, NULL)) != -1) {
		switch (opt) {
		case 'b':
			dev->private_int = 1;
			break;
		case 'h':
			shannon_nor_usage();
			return 0;
		default:
			shannon_nor_usage();
			return ERR;
		}
	}

	if (1 != (argc - optind) && 3 != (argc - optind) && 4 != (argc - optind)) {
		shannon_nor_usage();
		return ERR;
	}

	if (!strcmp("erase", argv[optind])) {
		nor = shannon_nor_init(dev);

		if ((argc - optind) != 3) {
			shannon_nor_usage();
			return ERR;
		}

		start = strtoul(argv[optind + 1], NULL, 0x10);
		length = strtoul(argv[optind + 2], NULL, 10);

		rc = nor_erase(dev, nor, start, length);
	} else if (!strcmp("write", argv[optind])) {
		nor = shannon_nor_init(dev);

		if ((argc - optind) != 3) {
			shannon_nor_usage();
			return ERR;
		}

		start = strtoul(argv[optind + 1], NULL, 0x10);
		fd = open(argv[optind + 2], O_RDONLY, 0666);
		if (fd < 0) {
			perror("Open file failed: ");
			return ERR;
		}
		if (fstat(fd, &sta)) {
			perror("Fstat file failed: ");
			return ERR;
		}

		buf = malloc(sta.st_size);
		if (NULL == buf) {
			printf("Malloc failed\n");
			return ERR;
		}
		if (read(fd, buf, sta.st_size) != sta.st_size) {
			perror("Read file failed: ");
			return ERR;
		}

		rc = nor_write(dev, nor, buf, start, sta.st_size);
		free(buf);
	} else if (!strcmp("read", argv[optind])) {
		nor = shannon_nor_init(dev);

		if ((argc - optind) != 4) {
			shannon_nor_usage();
			return ERR;
		}

		start = strtoul(argv[optind + 1], NULL, 0x10);
		length = strtoul(argv[optind + 2], NULL, 10);
		fd = open(argv[optind + 3], O_RDWR|O_TRUNC|O_CREAT, 0666);
		if (fd < 0) {
			perror("Open file failed: ");
			return ERR;
		}

		buf = malloc(length);
		if (NULL == buf) {
			printf("Malloc failed\n");
			return ERR;
		}
		rc = nor_read(dev, nor, buf, start, length, 0);

		if (!rc) {
			if (write(fd, buf, length) != length) {
				perror("Write file failed: ");
				return ERR;
			}
		}
		free(buf);
	} else if (!strcmp("reload", argv[optind])) {
		if ((argc - optind) != 1)
			return ERR;
		dev->iowrite32(dev, 0x80000000, 0x33);
	} else if (!strcmp("ckload", argv[optind])) {
		if ((argc - optind) != 1)
			return ERR;
		printf("%08X\n", dev->ioread32(dev, 0x33));
	} else if (!strcmp("check-endian", argv[optind])) {
		nor = shannon_nor_init(dev);

		print("%d\n", !strcmp(nor->name, "spi") ? 0 : 1);
	} else {
		shannon_nor_usage();
		return ERR;
	}

	exit_norflash(nor);
	return rc;
}

int shannon_read_nor(struct shannon_dev *dev, void *buf, int start, int len)
{
	int rc = ERR;
	struct norflash *nor;

	nor = shannon_nor_init(dev);
	rc = nor_read(dev, nor, buf, start, len, 1);

	exit_norflash(nor);
	return rc;
}
/*-----------------------------------------------------------------------------------------------------------*/
