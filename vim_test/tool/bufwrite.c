#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include "tool.h"

/*-----------------------------------------------------------------------------------------------------------*/
int dio(struct shannon_dev *dev, int opcode, int head, int begin, int count)
{
	char **argv;
	int i, rc, argc = 5;

	argv = malloc(sizeof(*argv) * argc);
	assert (NULL != argv);

	for (i = 0; i < argc; i++) {
		argv[i] = malloc(64);
		assert(NULL != argv[i]);
	}

	assert(HEAD0 == head || HEAD1 == head);
	assert(sh_write_cmd == opcode || sh_read_cmd == opcode);

	sprintf(argv[0], "dio");
	sprintf(argv[1], "%s", (sh_write_cmd == opcode) ? "write" : "read");
	sprintf(argv[2], "--head=%d", head);
	sprintf(argv[3], "--just-start=1982");
	sprintf(argv[4], "--sample-block-zone=%d,%d", begin, count);

	for (i = 0; i < argc; i++)
		printf("%s ", argv[i]);
	printf("\n");

	optind = 1;
	rc = shannon_dio(dev, argc, argv);

	for (i = 0; i < argc; i++)
		free(argv[i]);
	free(argv);

	return rc;
}

static void shannon_bufwrite_usage(void)
{
	printf("Description:\n");
	printf("\tWrite single lun page\n\n");

	printf("Usage:\n");
	printf("\tbufwrite [option] lun block page\n\n");

	printf("Option:\n");
	printf("\t-e, --seed=SEED\n"
		"\t\tset the seed of generate data, default use gettimeofday.tv_usec\n\n");
	printf("\t-p, --print\n"
		"\t\tprint the write data and metadata\n\n");
	printf("\t-H, --head=HEAD\n"
		"\t\tselect head, default is head 0\n\n");
	printf("\t-h, --help\n"
		"\t\tdisplay this help and exit\n");
}

int shannon_bufwrite(struct shannon_dev *dev, int argc, char **argv)
{
	struct option longopts [] = {
		{"seed", required_argument, NULL, 'e'},
		{"print", no_argument, NULL, 'p'},
		{"head", required_argument, NULL, 'H'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int opt;
	int seed, pr, head;
	int rc = 0;
	int lun, block, page, sector;
	struct shannon_request *req, *tmp;
	struct list_head req_head;

	/* analyse argument */
	pr = 0;
	seed = getseed(0);
	head = HEAD0;

	while ((opt = getopt_long(argc, argv, ":e:pH:h", longopts, NULL)) != -1) {
		switch (opt) {
		case 'e':
			seed = strtoul(optarg, NULL, 10);
			break;
		case 'p':
			pr = 1;
			break;
		case 'H':
			head = strtoul(optarg, NULL, 10);
			if (HEAD0 != head && HEAD1 != head) {
				printf("Bufwrite head choices: 0 1\n");
				return ERR;
			}
			break;
		case 'h':
			shannon_bufwrite_usage();
			return 0;
		default:
			shannon_bufwrite_usage();
			return ERR;
		}
	}

	if ((argc - optind) != 3) {
		shannon_bufwrite_usage();
		return ERR;
	}

	lun = strtoul(argv[optind], NULL, 10);
	block = strtoul(argv[optind + 1], NULL, 10);
	page = strtoul(argv[optind + 2], NULL, 10);

	if (lun > dev->config->luns || block > dev->flash->nblk / dev->config->nplane || page > dev->flash->npage) {
		printf("Overflow address\n");
		return OVERFLOW_ADDR;
	}

	/* bufwrite must with raid, so do raid-init first */
	if (!dev->config->raid_mode) {
		printf("Bufwrite: RAID must be enabled\n");
		return ERR;
	}

	req = alloc_request(dev, sh_raidinit_cmd, superblock_paritylun(dev, block),
			block * dev->config->nplane * dev->flash->npage + page, head, 0, superblock_ndatalun(dev, block));
	if (NULL == req) {
		rc = ALLOCMEM_FAILED;
		goto out;
	}

	if ((rc = dev->submit_request(req))) {
		free_request(req);
		goto out;
	}

	if ((rc = update_poll_cmdqueue(dev, superblock_paritylun(dev, block)))) {
		free_request(req);
		goto out;
	}

	/* bufwrite requests */
	srand(seed);
	INIT_LIST_HEAD(&req_head);

	// for (sector = 0; sector < dev->config->chunk_nsector; sector++) {
	for (sector = 0; sector < 2; sector++) {
		req = alloc_request(dev, sh_bufwrite_cmd, lun,
			// (block * dev->config->nplane + sector / dev->config->page_nsector) * dev->flash->npage + page, head, sector, 1);
			block * dev->config->nplane * dev->flash->npage + page, head, sector, 1);
		if (NULL == req) {
			rc = ALLOCMEM_FAILED;
			goto out;
		}
		pad_rand(req->data, dev->config->sector_size);
		pad_rand(req->metadata, METADATA_SIZE);
		list_add_tail(&req->list, &req_head);
	}

	/* execute */
	list_for_each_entry(req, &req_head, list) {
		if ((rc = dev->submit_request(req)))
			goto free_req_out;
	}

	if ((rc = update_poll_bufcmdqueue(dev, head)))
		goto free_req_out;

	list_for_each_entry(req, &req_head, list) {
		if ((rc = check_req_status(req)))
			goto free_req_out;
	}

	/* printf data if need */
	if (pr) {
		printf("#metadata:");			// make print format to match with read
		list_for_each_entry(req, &req_head, list) {
			if (0 == (req->bsector % dev->config->page_nsector) ||
				0 == ((req->bsector % dev->config->page_nsector) % 8))
				printf("\n");

			printf("%016lX ", req->metadata[0]);
		}
		printf("\n");

		list_for_each_entry(req, &req_head, list) {
			printf("#sector: %d\n", req->bsector);
			pr_u8_array_noprefix(req->data, dev->config->sector_size, 32);
		}
	}

	// dio(dev, sh_write_cmd, HEAD0, 0, 1024);

free_req_out:
	list_for_each_entry_safe(req, tmp, &req_head, list) {
		list_del(&req->list);
		free_request(req);
	}
out:
	if (rc != FAILED_FLASH_STATUS && rc != 0)
		printf("Bufwrite: encounter some error\n");
	return rc;
}

/*-----------------------------------------------------------------------------------------------------------*/
