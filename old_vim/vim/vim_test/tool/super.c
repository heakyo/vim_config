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
#include <time.h>

#include "tool.h"

// #define	SUPER_DEBUG

/*-----------------------------------------------------------------------------------------------------------*/
int construct_sb_luninfo(struct shannon_dev *dev, char *luninfo_file)
{
	int fd;
	struct stat stat;
	struct shannon_luninfo *luninfo;
	int blk;

	if ((fd = open(luninfo_file, O_RDONLY)) < 0) {
		perror("Open luninfo file failed\n");
		return ERR;
	}

	if (fstat(fd, &stat)) {
		perror("Fstat luninfo file failed\n");
		close(fd);
		return ERR;
	}
	assert(stat.st_size != 0);

	if ((luninfo = mmap(0, stat.st_size, PROT_READ, MAP_SHARED, fd, 0)) == NULL) {
		perror("Mmap luninfo file failed\n");
		close(fd);
		return ERR;
	}
	assert(luninfo->size == stat.st_size);
#if 0
	if (check_luninfo(dev, luninfo)) {
		printf("The config of luninfo doesn`t match present hardware config, please use command 'luninfo' to generate present luninfo file\n");
		munmap(luninfo, stat.st_size);
		close(fd);
		return ERR;
	}
#endif
	for_dev_each_block(dev, blk)
		memcpy(&dev->sb[blk].sb_luninfo, &luninfo->sb_luninfo[blk], sizeof(struct shannon_sb_luninfo));

	munmap(luninfo, stat.st_size);
	close(fd);
	return 0;
}

/*-----------------------------------------------------------------------------------------------------------*/
/*
 * Reset all lun flash, this function only called by init_device()
 */
int shannon_super_reset(struct shannon_dev *dev)
{
	int rc = 0;
	int lun;
	struct shannon_request *req, *tmp;
	struct list_head req_head;

	INIT_LIST_HEAD(&req_head);

	for_dev_each_lun(dev, lun) {
		req = alloc_request(dev, sh_reset_cmd, lun, 0, 0, 0, 0);
		if (NULL == req) {
			rc = ALLOCMEM_FAILED;
			goto free_req_out;
		}
		list_add_tail(&req->list, &req_head);
	}

	list_for_each_entry(req, &req_head, list) {
		if ((rc = dev->submit_request(req)))
			goto free_req_out;
	}

	for_dev_each_lun(dev, lun)
		update_cmdqueue(dev, lun);

	for_dev_each_lun(dev, lun)
		poll_cmdqueue(dev, lun);

	/* skip check timeout and status and leave this to subtool */

free_req_out:
	list_for_each_entry_safe(req, tmp, &req_head, list) {
		list_del(&req->list);
		free_request(req);
	}
	if (rc)
		printf("%s() failed\n", __func__);
	return rc;
}

/*
 * Read all flash id
 */
static void shannon_super_readid_usage(void)
{
	printf("Description:\n");
	printf("\tRead strip lun id\n\n");

	printf("Usage:\n");
	printf("\tsuper-readid [option]\n");

	printf("Option:\n");
	printf("\t-t, --absent-luns=loglun|phylun:x1-x2,x3,x4-x5...\n"
		"\t\tSpecify absent luns\n\n");
	printf("\t-T, --present-luns=loglun|phylun:x1-x2,x3,x4-x5...\n"
		"\t\tSpecify present luns\n\n");
	printf("\t-h, --help\n"
		"\t\tDisplay this help and exit\n\n");
}

int shannon_super_readid(struct shannon_dev *dev, int argc, char **argv)
{
	struct option longopts [] = {
		{"absent-luns", required_argument, NULL, 't'},
		{"present-luns", required_argument, NULL, 'T'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int opt, lun, i;
	struct shannon_request *req, *tmp;
	struct list_head req_head;
	struct target_lun *tgtlun;
	int user_luns = 0;

	while ((opt = getopt_long(argc, argv, ":t:T:h", longopts, NULL)) != -1) {
		switch (opt) {
		case 't':
			present_absent_luns(dev, optarg, 1);
			user_luns = 1;
			break;
		case 'T':
			present_absent_luns(dev, optarg, 0);
			user_luns = 1;
			break;
		case 'h':
			shannon_super_readid_usage();
			return 0;
		default:
			shannon_super_readid_usage();
			return ERR;
		}
	}

	INIT_LIST_HEAD(&req_head);

	for_dev_each_lun(dev, lun) {
		req = alloc_request(dev, sh_readid_cmd, lun, 0, 0, 0, 0);
		if (NULL == req)
			malloc_failed_exit();
		list_add_tail(&req->list, &req_head);
	}

	list_for_each_entry(req, &req_head, list) {
		if (dev->submit_request(req))
			submit_failed_exit(req->lun);
	}

	for_dev_each_lun(dev, lun)
		update_cmdqueue(dev, lun);

	for_dev_each_lun(dev, lun) {
		if (poll_cmdqueue(dev, lun))
			poll_failed_exit(lun);
	}

	list_for_each_entry_safe(req, tmp, &req_head, list) {
		memcpy(dev->targetlun[req->lun].id.byteid, req->id, sizeof(req->id));
		list_del(&req->list);
		free_request(req);
	}

	for (lun = 0; lun < dev->config->luns; lun++) {
		if (user_luns && test_bit(lun, dev->lun_bitmap))
			continue;

		tgtlun = &dev->targetlun[lun];

		printf("lun-%03d phylun-%03d *hw-channel=%02d hw-thread=%02d hw-lun=%02d:", tgtlun->loglun, tgtlun->phylun,
			tgtlun->phylun / (dev->hw_nthread * dev->hw_nlun),
			(tgtlun->phylun % (dev->hw_nthread * dev->hw_nlun)) / dev->hw_nlun,
			tgtlun->phylun % dev->hw_nlun);

		for (i = 0; i < 8; i++)
			printf(" %02X", tgtlun->id.byteid[i]);
		printf("\n");
	}

	return 0;
}

/*
 * Config all lun-flash enter async mode. XXX: Present we use reset flash to let
 * flash enter async mode, so this section of code is same as shannon_super_reset()
 */
int shannon_super_async(struct shannon_dev *dev)
{
	int rc = 0;
	int lun;
	struct shannon_request *req, *tmp;
	struct list_head req_head;

	assert(1 == dev->init_done);

	if (IFMODE_TOGGLE == dev->ifmode || IFMODE_ASYNC == dev->ifmode)	// XXX: dev->ifmode present both HW and flash interface mode
		return 0;

	/* 1st: config hw as async mode */
	// dev->clear_queue(dev);
	dev->config->ifmode = IFMODE_ASYNC;
	dev->config_hardware(dev);

	/* 2nd: config flash as async mode. use reset cmd to let flash enter async mode */
	INIT_LIST_HEAD(&req_head);

	for_dev_each_lun(dev, lun) {
		req = alloc_request(dev, sh_reset_cmd, lun, 0, 0, 0, 0);
		if (NULL == req) {
			rc = ALLOCMEM_FAILED;
			goto free_req_out;
		}
		list_add_tail(&req->list, &req_head);
	}

	list_for_each_entry(req, &req_head, list) {
		if ((rc = dev->submit_request(req)))
			goto free_req_out;
	}

	for_dev_each_lun(dev, lun)
		update_cmdqueue(dev, lun);

	for_dev_each_lun(dev, lun) {
		if ((rc = poll_cmdqueue(dev, lun)))
			goto free_req_out;
	}

	list_for_each_entry(req, &req_head, list)
		rc |= check_req_status_silent(req);

	dev->ifmode = IFMODE_ASYNC;

free_req_out:
	list_for_each_entry_safe(req, tmp, &req_head, list) {
		list_del(&req->list);
		free_request(req);
	}
	if (rc)
		printf("%s() failed\n", __func__);
	return rc;
}

/*
 * Config all lun-flash enter sync mode
 */
int shannon_super_sync(struct shannon_dev *dev)
{
	int rc;
	u8 feature[4]={0x14, 0, 0, 0};

	assert(1 == dev->init_done);

	if (IFMODE_TOGGLE == dev->ifmode || IFMODE_SYNC == dev->ifmode)	// XXX: dev->ifmode present both HW and flash interface mode
		return 0;

	rc = super_set_feature(dev, 0xEF, 0x01, feature, sizeof(feature));

	if (rc) {
		printf("%s() fatal failed\n", __func__);
	} else {
		dev->config->ifmode = IFMODE_SYNC;
		dev->config_hardware(dev);

		dev->ifmode = IFMODE_SYNC;
	}
	return rc;
}

int shannon_super_toggle(struct shannon_dev *dev)
{
	int rc;
	u8 feature[4]={0x00, 0, 0, 0};

	assert(1 == dev->init_done);

	if (IFMODE_TOGGLE == dev->ifmode || IFMODE_SYNC == dev->ifmode)	// XXX: dev->ifmode present both HW and flash interface mode
		return 0;

	rc = super_set_feature(dev, 0xEF, 0x80, feature, sizeof(feature));

	if (rc) {
		printf("%s() fatal failed\n", __func__);
	} else {
		dev->config->ifmode = IFMODE_TOGGLE;
		dev->config_hardware(dev);

		dev->ifmode = IFMODE_TOGGLE;
	}

	return rc;
}

int super_set_feature(struct shannon_dev *dev, u8 cmd, u8 addr, u8 *value, int nbyte)
{
	int i, rc = 0;
	int lun;
	struct shannon_request *req, *tmp;
	struct list_head req_head;

	/*
	printf("%s addr=0x%02X: ", __func__, addr);
	for (i = 0; i < nbyte; i++)
		printf("0x%02X ", value[i]);
	printf("\n");
	*/

	assert(nbyte > 1);
	INIT_LIST_HEAD(&req_head);

	for_dev_each_lun(dev, lun) {
		req = alloc_request(dev, sh_writereg_cmd, lun, 0, 0, 0, 0);
		if (NULL == req) {
			rc = ALLOCMEM_FAILED;
			goto free_req_out;
		}
		req->wr_flash_reg_nbyte = nbyte;

		memset(req->direct_cmdqueue, 0x00, sizeof(req->direct_cmdqueue));
		req->direct_cmdqueue[0] = sh_writereg_cmd;	// opcode
		req->direct_cmdqueue[4] = addr;			// flash address
		req->direct_cmdqueue[5] = nbyte;		// nbyte to transfer
		req->direct_cmdqueue[6] = cmd;			// flash command
		req->direct_cmdqueue[7] = log2phy_lun(dev, lun);// lun number
		for (i = 0; i < nbyte; i++)
			req->direct_cmdqueue[8 + i] = value[i];	// data to write

		list_add_tail(&req->list, &req_head);
	}

	list_for_each_entry(req, &req_head, list) {
		if ((rc = dev->submit_request(req)))
			goto free_req_out;
	}

	for_dev_each_lun(dev, lun)
		update_cmdqueue(dev, lun);

	for_dev_each_lun(dev, lun) {
		if ((rc = poll_cmdqueue(dev, lun)))
			goto free_req_out;
	}
	// sh_writereg_cmd has no completion status

free_req_out:
	list_for_each_entry_safe(req, tmp, &req_head, list) {
		list_del(&req->list);
		free_request(req);
	}
	return rc;
}

/*-----------------------------------------------------------------------------------------------------------*/
/*
 * type: 0, specify present luns; 1, specify absent luns
 */
void present_absent_luns(struct shannon_dev *dev, char *value, int type)
{
	int start, end, lun, b, phy;
	char *p, *endptr;

	if (!strncmp("phylun:", value, 7)) {
		phy = 1;
	} else if (!strncmp("loglun:", value, 7)) {
		phy = 0;
	} else {
		printf("option --present-luns and --absent-luns should be prefix by 'phylun:' or 'loglun:'\n");
		exit(EXIT_FAILURE);
	}

	if (!type)
		memset(dev->lun_bitmap, 0xFF, sizeof(dev->lun_bitmap));

	p = value + 7;
	b = 0;

	fprintf(stderr, "%s %s luns: ", type ? "absent" : "present", phy ? "physical" : "logical");

	do {
		start = strtoul(p, &endptr, 10);
		if ('-' == *endptr) {
			p = endptr + 1;
			end = strtoul(p, &endptr, 10);
		} else {
			end = start;
		}

		for (lun = start; lun <= end; lun++) {
			if (lun < dev->config->luns && lun >= 0) {
				fprintf(stderr, "%d ", lun);
				if (type) {
					set_bit(phy ? phy2log_lun(dev, lun) : lun, dev->lun_bitmap);
				} else {
					if (dev->targetlun[phy ? phy2log_lun(dev, lun) : lun].shadow) {
						printf("\nShadow lun: log %d phy %d\n", phy ? phy2log_lun(dev, lun) : lun, phy ? lun : log2phy_lun(dev, lun));
						exit(EXIT_FAILURE);
					}
					clear_bit(phy ? phy2log_lun(dev, lun) : lun, dev->lun_bitmap);
				}
				b++;
			}
		}

		p = endptr + 1;
	} while (',' == *endptr);

	fprintf(stderr, "\n");
	// pr_u64_array_noprefix(dev->lun_bitmap, 32, 8);

	if (!b) {
		printf("ERR: You have no specify luns\n");
		exit(EXIT_FAILURE);
	}
}

/*-----------------------------------------------------------------------------------------------------------*/
/*
 * erase super chunk block
 */
static void shannon_super_erase_usage(void)
{
	printf("Description:\n");
	printf("\tErase strip lun blocks\n\n");

	printf("Usage:\n");
	printf("\tsuper-erase [option] begin_chunkblock count\n");
	printf("\tsuper-erase [-a, --entire-flash]\n\n");

	printf("Option:\n");
	printf("\t-f, --luninfo=luninfo-file\n"
		"\t\tSpecify luninfo which includes ndatalun/parity_lun/sb_bbt, otherwise assume all blocks are valid and parity_lun is the last lun\n\n");
	printf("\t-a, --entire-flash\n"
		"\t\tErase entire flash\n\n");
	printf("\t-N, --noprogress\n"
		"\t\tDisable show progress\n\n");
	printf("\t-s, --silent\n"
		"\t\tDisable print erase failed status\n\n");
	printf("\t-t, --absent-luns=loglun|phylun:x1-x2,x3,x4-x5...\n"
		"\t\tSpecify absent luns\n\n");
	printf("\t-T, --present-luns=loglun|phylun:x1-x2,x3,x4-x5...\n"
		"\t\tSpecify present luns\n\n");
	printf("\t-h, --help\n"
		"\t\tDisplay this help and exit\n\n");

	printf("NOTE:\n\tBlock index is count by chunk\n");
}

int shannon_super_erase(struct shannon_dev *dev, int argc, char **argv)
{
	struct option longopts [] = {
		{"luninfo", required_argument, NULL, 'f'},
		{"entire-flash", no_argument, NULL, 'a'},
		{"noprogress", no_argument, NULL, 'N'},
		{"silent", no_argument, NULL, 's'},
		{"absent-luns", required_argument, NULL, 't'},
		{"present-luns", required_argument, NULL, 'T'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int opt;
	char *luninfo_file;
	int lun, begin_chunkblock, count;
	int head, noprogress, silent, all;
	int blk, plane, ppa;
	int rc = 0;
	struct shannon_request *chunk_head_req, *req, *tmp;
	struct list_head req_head;
	int pre_cent, now_cent;		// used for show progress

	/* analyse argument */
	luninfo_file = NULL;
	head = INDEP_HEAD;
	noprogress = 0;
	silent = 0;
	all = 0;

	while ((opt = getopt_long(argc, argv, ":f:aNst:T:h", longopts, NULL)) != -1) {
		switch (opt) {
		case 'f':
			luninfo_file = optarg;
			break;
		case 'a':
			all = 1;
			break;
		case 'N':
			noprogress = 1;
			break;
		case 's':
			silent = 1;
			break;
		case 't':
			present_absent_luns(dev, optarg, 1);
			break;
		case 'T':
			present_absent_luns(dev, optarg, 0);
			break;
		case 'h':
			shannon_super_erase_usage();
			return 0;
		default:
			shannon_super_erase_usage();
			return ERR;		
		}
	}

	if (all) {
		if (argc - optind) {
			shannon_super_erase_usage();
			return ERR;
		}
		begin_chunkblock = 0;
		count = dev->flash->nblk / dev->config->nplane;
	} else {
		if ((argc - optind) != 2) {
			shannon_super_erase_usage();
			return ERR;
		}
		begin_chunkblock = strtoul(argv[optind], NULL, 10);
		count = strtoul(argv[optind + 1], NULL, 10);
	}

	if ((begin_chunkblock + count) > dev->flash->nblk/dev->config->nplane) {
		printf("Overflow address\n");
		return OVERFLOW_ADDR;
	}

	/* construct luninfo if needed */
	if (NULL != luninfo_file) {
		if (construct_sb_luninfo(dev, luninfo_file))
			return ERR;
	}

	/* manipulate erase chunk block process */
	if (dev->config->nplane > 1)
		head |= (1 << SH_ERASE_PLANE_SHIFT);

	if (!noprogress) {
		pre_cent = now_cent = 0;
		print("Super-erase in process...%%%02d", now_cent);
	}

	INIT_LIST_HEAD(&req_head);

	for (blk = begin_chunkblock; blk < begin_chunkblock + count; blk++) {
		ppa = blk * dev->config->nplane * dev->flash->npage;

		/* alloc erase request */
		for_dev_each_lun(dev, lun) {
			if (is_bad_lunblock(dev, lun, blk))
				continue;

			chunk_head_req = alloc_request(dev, sh_erase_cmd, lun, ppa, head, 0, 0);
			if (NULL == chunk_head_req) {
				rc = ALLOCMEM_FAILED;
				goto free_req_out;
			}
			list_add_tail(&chunk_head_req->list, &req_head);

			for (plane = 1; plane < dev->config->nplane; plane++) {
				req = alloc_request(dev, sh_erase_cmd, lun, ppa + plane * dev->flash->npage, head, 0, 0);
				if (NULL == req) {
					rc = ALLOCMEM_FAILED;
					goto free_req_out;
				}
				list_add_tail(&req->chunk_list, &chunk_head_req->chunk_list);
			}
		}

		/* summit all request */
		list_for_each_entry(req, &req_head, list) {
			if ((rc = dev->submit_request(req)))
				goto free_req_out;
		}

		/* update commad head and wait for completed */
		for_dev_each_lun(dev, lun)
			update_cmdqueue(dev, lun);

		for_dev_each_lun(dev, lun) {
			if ((rc = poll_cmdqueue(dev, lun)))
				goto free_req_out;
		}

		/* check status */
		list_for_each_entry(req, &req_head, list) {
			if (silent)
				rc |= check_req_status_silent(req);
			else
				rc |= check_req_status(req);
		}

		/* print progress if needed */
		if (!noprogress) {
			now_cent = 100 * (blk - begin_chunkblock + 1) / count;

			if (now_cent > pre_cent) {
				print("\b\b\b%%%02d", now_cent);
				pre_cent = now_cent;
			}
		}

		/* free req */
		list_for_each_entry_safe(req, tmp, &req_head, list) {
			list_del(&req->list);
			free_request(req);
		}
	}

	if (!noprogress)
		printf("\n");

free_req_out:
	list_for_each_entry_safe(req, tmp, &req_head, list) {
		list_del(&req->list);
		free_request(req);
	}

	if (rc != FAILED_FLASH_STATUS && rc != 0)
		printf("Super erase encounter some error\n");
	return rc;
}

/*-----------------------------------------------------------------------------------------------------------*/
static void shannon_super_write_usage(void)
{
	printf("Description:\n");
	printf("\tWrite strip lun blocks\n\n");

	printf("Usage:\n");
	printf("\tSuper-write [option] begin_chunkblock count\n\n");

	printf("Option:\n");
	printf("\t-f, --luninfo=luninfo-file\n"
		"\t\tSpecify luninfo which includes ndatalun/parity_lun/sb_bbt, otherwise assume all blocks are valid and parity_lun is the last lun\n\n");
	printf("\t-e, --seed=SEED\n"
		"\t\tSpecify rand seed of write data, otherwise use gettimeofday.tv_usec as default seed and print it\n\n");
	printf("\t-H, --head=HEAD\n"
		"\t\tSelect head, default is head 2.If head is 0 or 1, hardware config of raid should be enabled and will write with raid\n\n");
	printf("\t-N, --noprogress\n"
		"\t\tDisable show progress\n\n");
	printf("\t-t, --absent-luns=loglun|phylun:x1-x2,x3,x4-x5...\n"
		"\t\tSpecify absent luns\n\n");
	printf("\t-T, --present-luns=loglun|phylun:x1-x2,x3,x4-x5...\n"
		"\t\tSpecify present luns\n\n");
	printf("\t-A, --from-page=m\n"
		"\t\tWrite per-blcok from this page offset\n\n");
	printf("\t-B, --to-page=n\n"
		"\t\tWrite per-blcok to this page offset\n\n");
	printf("\t-h, --help\n"
		"\t\tDisplay this help and exit\n\n");

	printf("NOTE:\n\tBlock index is count by chunk\n");
}

int shannon_super_write(struct shannon_dev *dev, int argc, char **argv)
{
	struct option longopts [] = {
		{"luninfo", required_argument, NULL, 'f'},
		{"seed", required_argument, NULL, 'e'},
		{"head", required_argument, NULL, 'H'},
		{"noprogress", no_argument, NULL, 'N'},
		{"absent-luns", required_argument, NULL, 't'},
		{"present-luns", required_argument, NULL, 'T'},
		{"from-page", required_argument, NULL, 'A'},
		{"to-page", required_argument, NULL, 'B'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int i, rc = 0;
	int opt;
	char *luninfo_file;
	int seed, head, seed_way, noprogress;
	int blk, plane, ppa, page, sector_seed;
	int lun, begin_chunkblock, count;
	struct shannon_request *chunk_head_req, *req, *tmp;
	struct list_head req_head;
	int pre_cent, now_cent;		// used for show progress
	int frompage = 0, topage = dev->flash->npage - 1;

	/* analyse argument */
	luninfo_file = NULL;
	seed = getseed(0);
	head = INDEP_HEAD;
	seed_way = 0;
	noprogress = 0;

	while ((opt = getopt_long(argc, argv, ":f:e:H:Nt:T:A:B:h", longopts, NULL)) != -1) {
		switch (opt) {
		case 'f':
			luninfo_file = optarg;
			break;
		case 'e':
			seed = strtoul(optarg, NULL, 10);
			seed_way = 1;
			break;
		case 'H':
			head = strtoul(optarg, NULL, 10);
			if (HEAD0 != head && HEAD1 != head && INDEP_HEAD != head) {
				printf("Head choices: 0 1 2\n");
				return ERR;
			}
			break;
		case 'N':
			noprogress = 1;
			break;
		case 't':
			present_absent_luns(dev, optarg, 1);
			break;
		case 'T':
			present_absent_luns(dev, optarg, 0);
			break;
		case 'A':
			frompage = atoi(optarg);
			break;
		case 'B':
			topage = atoi(optarg);
			break;
		case 'h':
			shannon_super_write_usage();
			return 0;
		default:
			shannon_super_write_usage();
			return ERR;		
		}
	}

	if ((argc - optind) != 2) {
		shannon_super_write_usage();
		return ERR;
	}

	if (frompage >= dev->flash->npage || topage >= dev->flash->npage || frompage > topage) {
		printf("Invalid form-page %d to-page %d\n", frompage, topage);
		exit(EXIT_FAILURE);
	}

	begin_chunkblock = strtoul(argv[optind], NULL, 10);
	count = strtoul(argv[optind + 1], NULL, 10);

	if ((begin_chunkblock + count) > dev->flash->nblk/dev->config->nplane) {
		printf("Overflow address\n");
		return OVERFLOW_ADDR;
	}

	/* check match of head and raid */
	if (HEAD0 == head || HEAD1 == head) {
		if (!dev->config->raid_mode) {
			printf("You must enable raid when write by head0 or head1. Please rm file 'config' then reconfig hardware\n");
			return ERR;
		}
	}

	/* construct luninfo if needed */
	if (NULL != luninfo_file) {
		if (construct_sb_luninfo(dev, luninfo_file))
			return ERR;
	}

	if (!seed_way)
		print("seed=%d\n", seed);

	/* manipulate write chunk process */
	if (dev->config->nplane > 1)
		head |= (1 << SH_WRITE_PLANE_SHIFT);

	if (!noprogress) {
		pre_cent = now_cent = 0;
		print("Super-write in process...%%%02d", now_cent);
	}

	INIT_LIST_HEAD(&req_head);

	for (blk = begin_chunkblock; blk < begin_chunkblock + count; blk++) {
		if (is_bad_superblock(dev, blk))
			continue;

		ppa = blk * dev->config->nplane * dev->flash->npage;
		page = frompage;

		/* raid init before per-block if needed */
		if (dev->config->raid_mode && (head & HEAD_MASK) != INDEP_HEAD) {
			req = alloc_request(dev, sh_raidinit_cmd, superblock_paritylun(dev, blk), ppa, head, 0, superblock_ndatalun(dev, blk));
			if (NULL == req)  {
				rc = ALLOCMEM_FAILED;
				goto free_req_out;
			}
			list_add_tail(&req->list, &req_head);
		}

next_block_page: /* write chunk */
		for_dev_each_lun(dev, lun) {
			if (is_bad_lunblock(dev, lun, blk))
				continue;

			if (dev->config->raid_mode && (head & HEAD_MASK) != INDEP_HEAD &&  superblock_paritylun(dev, blk) == lun)	// raid lun
				continue;

			chunk_head_req = alloc_request(dev, sh_write_cmd, lun, ppa + page, head, 0, dev->config->page_nsector);
			if (NULL == chunk_head_req) {
				rc = ALLOCMEM_FAILED;
				goto free_req_out;
			}
			for (i = 0; i < dev->config->page_nsector; i++) {
				sector_seed = ((ppa + page) * dev->config->page_nsector + i) + ((lun + seed)<<24);
				srand(sector_seed);
				pad_rand(chunk_head_req->data + i * dev->config->sector_size, dev->config->sector_size);
				pad_rand(chunk_head_req->metadata + i, METADATA_SIZE);
				// pad_same_u32(chunk_head_req->data + i * dev->config->sector_size, dev->config->sector_size, sector_seed);
				// pad_same_u32(chunk_head_req->metadata + i, METADATA_SIZE, sector_seed);
			}
			list_add_tail(&chunk_head_req->list, &req_head);

			for (plane = 1; plane < dev->config->nplane; plane++) {
				req = alloc_request(dev, sh_write_cmd, lun, ppa + plane * dev->flash->npage + page, head, 0, dev->config->page_nsector);
				if (NULL ==req) {
					rc = ALLOCMEM_FAILED;
					goto free_req_out;
				}
				for (i = 0; i < dev->config->page_nsector; i++) {
					sector_seed = ((ppa + plane * dev->flash->npage + page) * dev->config->page_nsector + i) + ((lun + seed)<<24);
					srand(sector_seed);
					pad_rand(req->data + i * dev->config->sector_size, dev->config->sector_size);
					pad_rand(req->metadata + i, METADATA_SIZE);
					// pad_same_u32(req->data + i * dev->config->sector_size, dev->config->sector_size, sector_seed);
					// pad_same_u32(req->metadata + i, METADATA_SIZE, sector_seed);
				}
				list_add_tail(&req->chunk_list, &chunk_head_req->chunk_list);
			}
		}

		/* raid write request */
		if (dev->config->raid_mode && (head & HEAD_MASK) != INDEP_HEAD) {
			chunk_head_req = alloc_request(dev, sh_raidwrite_cmd, superblock_paritylun(dev, blk), ppa + page, head, 0, 0);
			if (NULL == chunk_head_req) {
				rc = ALLOCMEM_FAILED;
				goto free_req_out;
			}
			list_add_tail(&chunk_head_req->list, &req_head);

			for (plane = 1; plane < dev->config->nplane; plane++) {
				req = alloc_request(dev, sh_raidwrite_cmd, superblock_paritylun(dev, blk), ppa + plane * dev->flash->npage + page, head, 0, 0);
				if (NULL == req) {
					rc = ALLOCMEM_FAILED;
					goto free_req_out;
				}
				list_add_tail(&req->chunk_list, &chunk_head_req->chunk_list);
			}
		}

		/* summit and execute all request */
		list_for_each_entry(req, &req_head, list) {
			if ((rc = dev->submit_request(req)))
				goto free_req_out;
		}

		for_dev_each_lun(dev, lun)
			update_cmdqueue(dev, lun);

		for_dev_each_lun(dev, lun) {
			if ((rc = poll_cmdqueue(dev, lun)))
				goto free_req_out;
		}

		/* check status */
		list_for_each_entry(req, &req_head, list) {
			if (sh_write_cmd == req->opcode)
				check_req_status(req);
		}

		/* free request */
		list_for_each_entry_safe(req, tmp, &req_head, list) {
			list_del(&req->list);
			free_request(req);
		}

		/* print progress if needed */
		if (!noprogress) {
			now_cent = 100 * ((blk - begin_chunkblock) * dev->flash->npage + page + 1) / (count * dev->flash->npage);

			if (now_cent > pre_cent) {
				print("\b\b\b%%%02d", now_cent);
				pre_cent = now_cent;
			}
		}

		if (++page < topage + 1)
			goto next_block_page;
	}

	if (!noprogress)
		printf("\n");

free_req_out:
	list_for_each_entry_safe(req, tmp, &req_head, list) {
		list_del(&req->list);
		free_request(req);
	}

	if (rc)
		printf("Super-write encounter some error\n");
	return rc;
}

/*
 * super read chunk and check data consistence
 */
static void shannon_super_read_usage(void)
{
	printf("Description:\n");
	printf("\tDefault this command just statistic ecc result. Option [--seed] will enable data consistence check and [--raid] will enable raid parity check if data\n"
		"\tis written by super-write. Please note if data is writen with raid by super-write, [--seed] and [--raid] must be enabled and disabled at the same time.\n\n");

	printf("Usage:\n");
	printf("\tSuper-read [option] begin_chunkblock count\n\n");

	printf("Option:\n");
	printf("\t-f, --luninfo=luninfo-file\n"
		"\t\tSpecify luninfo which includes ndatalun/parity_lun/sb_bbt, otherwise assume all blocks are valid and parity_lun is the last lun\n\n");
	printf("\t-P, --print-error-location\n"
		"\t\tThis option will enable print error location, including preread failed status and uncorrectable ecc location\n\n");
	printf("\t-e, --seed=SEED\n"
		"\t\tSet this option means check data consistence and ecc result, otherwise only check ecc result\n\n");
	printf("\t-r, --raid\n"
		"\t\tSet this option means super block is written with raid-write, so read should use raid to check\n\n");
	printf("\t-N, --noprogress\n"
		"\t\tDisable show progress\n\n");
	printf("\t-D, --disable-last-cacheread\n"
		"\t\tDisable use last_cacheread command\n\n");
	printf("\t-t, --absent-luns=loglun|phylun:x1-x2,x3,x4-x5...\n"
		"\t\tSpecify absent luns\n\n");
	printf("\t-T, --present-luns=loglun|phylun:x1-x2,x3,x4-x5...\n"
		"\t\tSpecify present luns\n\n");
	printf("\t-A, --from-page=m\n"
		"\t\tRead per-blcok from this page offset\n\n");
	printf("\t-B, --to-page=n\n"
		"\t\tRead per-blcok to this page offset\n\n");
	printf("\t-p, --print-option=[D/M/E]\n"
		"\t\tPrint data/metadata/ECC or their association. This option will close statistics info\n\n");
	printf("\t-h, --help\n"
		"\t\tDisplay this help and exit\n\n");

	printf("NOTE:\n\tBlock index is count by chunk\n");
}

int shannon_super_read(struct shannon_dev *dev, int argc, char **argv)
{
	struct option longopts [] = {
		{"luninfo", required_argument, NULL, 'f'},
		{"print-error-location", no_argument, NULL, 'P'},
		{"seed", required_argument, NULL, 'e'},
		{"raid", no_argument, NULL, 'r'},
		{"noprogress", no_argument, NULL, 'N'},
		{"disable-last-cacheread", no_argument, NULL, 'D'},
		{"absent-luns", required_argument, NULL, 't'},
		{"present-luns", required_argument, NULL, 'T'},
		{"from-page", required_argument, NULL, 'A'},
		{"to-page", required_argument, NULL, 'B'},
		{"print-option", required_argument, NULL, 'p'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int i, j, rc = 0;
	int opt;
	char *luninfo_file;
	int raid, pr_error_location;
	int sector_seed, seed, check_data, head, noprogress;
	void *raid_data, *raid_metadata;
	int blk, plane, ppa, page;
	int lun, begin_chunkblock, count;
	struct shannon_request *chunk_head_req, *req, *tmp, *tmp1;
	struct list_head req_head;
	int pre_cent, now_cent;		// used for show progress
	long **lun_ecc_statistics;
	int last_cacheread = 1;
	int pr_switch = 0, pr_ecc = 0, pr_meta = 0, pr_data = 0, chunknsector;
	int frompage = 0, topage = dev->flash->npage - 1;

	/* analyse argument */
	head = 0;			// read just have multi or single plane, no head
	noprogress = 0;
	raid = 0;
	pr_error_location = 0;
	seed = -1;
	check_data = 0;
	luninfo_file = NULL;

	while ((opt = getopt_long(argc, argv, ":f:Pe:NDt:T:A:B:p:h", longopts, NULL)) != -1) {
		switch (opt) {
		case 'f':
			luninfo_file = optarg;
			break;
		case 'P':
			pr_error_location = 1;
			break;
		case 'e':
			seed = strtoul(optarg, NULL, 10);
			check_data = 1;
			break;
		case 'r':
			if (!dev->config->raid_mode) {
				printf("Hardware doesn`t enable raid, please rm file 'config' then reconfig\n");
				return ERR;
			}
			raid = 1;
			break;
		case 'N':
			noprogress = 1;
			break;
		case 'D':
			last_cacheread = 0;
			break;
		case 't':
			present_absent_luns(dev, optarg, 1);
			break;
		case 'T':
			present_absent_luns(dev, optarg, 0);
			break;
		case 'A':
			frompage = atoi(optarg);
			break;
		case 'B':
			topage = atoi(optarg);
			break;
		case 'p':
			pr_switch = 1;
			check_data = 1;
			noprogress = 1;

			for (i = 0; i < strlen(optarg); i++) {
				switch (optarg[i]) {
				case 'D':
					pr_data = 1;
					break;
				case 'M':
					pr_meta = 1;
					break;
				case 'E':
					pr_ecc = 1;
					break;
				default:
					shannon_super_read_usage();
					return ERR;
				}
			}
			break;
		case 'h':
			shannon_super_read_usage();
			return 0;
		default:
			shannon_super_read_usage();
			return ERR;		
		}
	}

	if ((argc - optind) != 2) {
		shannon_super_read_usage();
		return ERR;
	}

	if (frompage >= dev->flash->npage || topage >= dev->flash->npage || frompage > topage) {
		printf("Invalid form-page %d to-page %d\n", frompage, topage);
		exit(EXIT_FAILURE);
	}

	begin_chunkblock = strtoul(argv[optind], NULL, 10);
	count = strtoul(argv[optind + 1], NULL, 10);

	if ((begin_chunkblock + count) > dev->flash->nblk/dev->config->nplane) {
		printf("Overflow address\n");
		return OVERFLOW_ADDR;
	}

	/* statistics ecc result init */
	lun_ecc_statistics = zmalloc(dev->config->luns * sizeof(*lun_ecc_statistics));
	if (NULL == lun_ecc_statistics) {
		rc = ALLOCMEM_FAILED;
		goto out;
	}
	for_dev_each_lun(dev, lun) {
		lun_ecc_statistics[lun] = zmalloc(256 * sizeof(*lun_ecc_statistics[lun]));
		if (NULL == lun_ecc_statistics[lun]) {
			rc = ALLOCMEM_FAILED;
			goto free_lun_ecc_statistics;
		}
	}

	/* construct luninfo if needed */
	if (NULL != luninfo_file) {
		if (construct_sb_luninfo(dev, luninfo_file))
			return ERR;
	}

	if (raid) {
		raid_data = malloc(dev->config->chunk_ndata);
		if (NULL == raid_data)
			return ERR;

		raid_metadata = malloc(dev->config->chunk_nmeta);
		if (NULL == raid_metadata) {
			free(raid_data);
			return ERR;
		}
	}

	/* manipulate read chunk block and check process */
	if (dev->config->nplane > 1)
		head |= (1 << SH_READ_PLANE_SHIFT);

	if (!noprogress) {
		pre_cent = now_cent = 0;
		print("Super-read in process...%%%02d", now_cent);
	}

	INIT_LIST_HEAD(&req_head);

	for (blk = begin_chunkblock; blk < begin_chunkblock + count; blk++) {
		if (is_bad_superblock(dev, blk))
			continue;

		ppa = blk * dev->config->nplane * dev->flash->npage;
		page = frompage;

next_block_page: /* read chunk */
		for_dev_each_lun(dev, lun) {
			if (is_bad_lunblock(dev, lun, blk))
				continue;

			chunk_head_req = alloc_request(dev, sh_preread_cmd, lun, ppa + page, head, 0, 0);	// preread
			if (NULL == chunk_head_req) {
				rc = ALLOCMEM_FAILED;
				goto free_req_out;
			}
			list_add_tail(&chunk_head_req->list, &req_head);

			for (plane = 1; plane < dev->config->nplane; plane++) {
				req = alloc_request(dev, sh_preread_cmd, lun, ppa + plane * dev->flash->npage + page, head, 0, 0);
				if (NULL == req) {
					rc = ALLOCMEM_FAILED;
					goto free_req_out;
				}
				list_add_tail(&req->chunk_list, &chunk_head_req->chunk_list);
			}

			for (plane = 0; plane < dev->config->nplane; plane++) {					// cacheread
				int ns, bs = 0;
				int remain_ns = dev->config->page_nsector;

				while (remain_ns) {
					ns = (remain_ns >= 8) ? 8 : remain_ns;

					if (check_data || raid)
						req = alloc_request(dev, sh_cacheread_cmd, lun, ppa + plane * dev->flash->npage + page, head, bs, ns);
					else
						req = alloc_request(dev, sh_cacheread_cmd, lun, ppa + plane * dev->flash->npage + page, head, bs, 0);	// disable alloc memory
					if (NULL == req) {
						rc = ALLOCMEM_FAILED;
						goto free_req_out;
					}
					req->nsector = ns;
					list_add_tail(&req->list, &req_head);

					bs += ns;
					remain_ns -= ns;
				}
			}
			req->last_cacheread = last_cacheread;
		}

		/* summit and execute all request */
		list_for_each_entry(req, &req_head, list) {
			if ((rc = dev->submit_request(req)))
				goto free_req_out;
		}

		for_dev_each_lun(dev, lun)
			update_cmdqueue(dev, lun);

		for_dev_each_lun(dev, lun) {
			if ((rc = poll_cmdqueue(dev, lun)))
				goto free_req_out;
		}

		if (!pr_switch)
			goto not_print_path;

		/* just print ecc/meta/data no check */
		list_for_each_entry(req, &req_head, list) {
			if (sh_preread_cmd != req->opcode)
				continue;
			tmp1 = req;

			/* list from sh_preread_cmd */
			if (pr_error_location)
				check_req_status(req);
			else
				check_req_status_silent(req);

			/* this is first cacheread */
			tmp = list_first_entry(&req->list, struct shannon_request, list);
			assert(tmp->opcode == sh_cacheread_cmd);

			if (pr_ecc) {
				req = tmp;
				chunknsector = dev->config->chunk_nsector;

				printf("#ECC(lun=%d sb=%d page=%d):", req->lun, req->chunk_block, req->page);
				while (chunknsector) {
					for (i = 0; i < req->nsector; i++)
						printf(" %02X", req->ecc[i]);
					chunknsector -= req->nsector;
					req = list_first_entry(&req->list, struct shannon_request, list);
				}
				printf("\n");
			}

			if (pr_meta) {
				req = tmp;
				chunknsector = dev->config->chunk_nsector;

				printf("#meta(lun=%d sb=%d page=%d):", req->lun, req->chunk_block, req->page);
				while (chunknsector) {
					for (i = 0; i < req->nsector; i++)
						printf(" %016lX", req->metadata[i]);
					chunknsector -= req->nsector;
					req = list_first_entry(&req->list, struct shannon_request, list);
				}
				printf("\n");
			}

			if (pr_data) {
				req = tmp;
				chunknsector = 0;

				while (chunknsector != dev->config->chunk_nsector) {
					for (i = 0; i < req->nsector; i++) {
						printf("#sector(lun=%d sb=%d page=%d): %d", req->lun, req->chunk_block, req->page, chunknsector + i);
						for (j = 0; j < dev->config->sector_size; j++) {
							if (0 == j % 32)
								printf("\n");
							printf("%02X ", req->data[i * dev->config->sector_size + j]);
						}
						printf("\n");
					}

					chunknsector += req->nsector;
					req = list_first_entry(&req->list, struct shannon_request, list);
				}
			}

			req = tmp1;
		}
		goto skip_check_data;

		/* check status and ecc */
not_print_path:
		list_for_each_entry(req, &req_head, list) {
			if (sh_preread_cmd == req->opcode) {
				if (pr_error_location)
					check_req_status(req);
				else
					check_req_status_silent(req);
			}

			if (sh_cacheread_cmd == req->opcode) {
				for (i = 0; i < req->nsector; i++) {
					lun_ecc_statistics[req->lun][req->ecc[i]]++;

					if (req->ecc[i] < 251 || !pr_error_location)
						continue;

					if (req->ecc[i] == 251)
						printf("Super-read blank %02X. ", req->ecc[i]);
					else if (req->ecc[i] > 251)
						printf("Super-read ecc failed: %02X. ", req->ecc[i]);

					printf("lun=%d block=%d page=%d sector=%d\n",
						req->lun, req->chunk_block, req->page, i + req->bsector + req->chunk_plane * dev->config->page_nsector);
				}
			}
		}

		/* check data consistence if needed */
		if (!check_data && !raid)
			goto skip_check_data;

		if(raid) {
			memset(raid_data, 0x00, dev->config->chunk_ndata);
			memset(raid_metadata, 0x00, dev->config->chunk_nmeta);
		}

		list_for_each_entry(req, &req_head, list) {
			if (sh_cacheread_cmd != req->opcode)
				continue;

			if (raid) {
				memxor(raid_data + (req->chunk_plane * dev->config->page_nsector + req->bsector) * dev->config->sector_size, req->data, req->nsector * dev->config->sector_size);
				memxor(raid_metadata + (req->chunk_plane * dev->config->page_nsector + req->bsector) * METADATA_SIZE, req->metadata, req->nsector * METADATA_SIZE);

				if (superblock_paritylun(dev, blk) == req->lun)
					continue;
			}

			if (!check_data)
				continue;

			for (i = 0; i < req->nsector; i++) {
				__u8 ch;
				int ibx, jump = 0;

				sector_seed = (req->ppa * dev->config->page_nsector + req->bsector + i) + ((req->lun + seed)<<24);
				srand(sector_seed);

				for (ibx = 0; ibx < dev->config->sector_size; ibx++) {	// data
					if ((ch = rand()) != req->data[i * dev->config->sector_size + ibx]) {
						printf("Data mismatch: lun=%d block=%d page=%d sector=%d off=%d write=%02X read=%02X\n",
							req->lun, req->chunk_block, req->page, i + req->bsector + req->chunk_plane * dev->config->page_nsector,
							ibx, ch, req->data[i * dev->config->sector_size + ibx]);
						jump = 1;
						break;
					}
				}

				if (jump)
					continue;

				for (ibx = 0; ibx < METADATA_SIZE; ibx++) {		// metadata
					if ((ch = rand()) != ((__u8 *)req->metadata)[i * METADATA_SIZE + ibx]) {
						printf("Metadata mismatch: lun=%d block=%d page=%d sector=%d\n",
							req->lun, req->chunk_block, req->page, i + req->bsector + req->chunk_plane * dev->config->page_nsector);
						break;
					}
				}
			}
		}

		/* check raid data if needed */
		if (raid) {
			for (i = 0; i < dev->config->chunk_ndata; i++) {	// data
				if (((__u8 *)raid_data)[i] != 0) {
					printf("Raid check failed: block=%d page=%d\n", blk, list_first_entry(&req_head, struct shannon_request, list)->page);
					goto skip_check_data;
				}
			}

			for (i = 0; i < dev->config->chunk_nmeta; i++) {	// metadata
				if (((__u8 *)raid_metadata)[i] != 0) {
					printf("Raid check failed: block=%d page=%d\n", blk, list_first_entry(&req_head, struct shannon_request, list)->page);
					goto skip_check_data;
				}
			}
		}

skip_check_data: /* free request */
		list_for_each_entry_safe(req, tmp, &req_head, list) {
			list_del(&req->list);
			free_request(req);
		}

		/* print progress if needed */
		if (!noprogress) {
			now_cent = 100 * ((blk - begin_chunkblock) * dev->flash->npage + page + 1) / (count * dev->flash->npage);

			if (now_cent > pre_cent) {
				print("\b\b\b%%%02d", now_cent);
				pre_cent = now_cent;
			}
		}

		if (++page < topage + 1)
			goto next_block_page;
	}

	if (pr_switch)
		return 0;

	if (!noprogress)
		printf("\n");

	/* calculate ecc statistics */
	for_dev_each_lun(dev, lun) {
		printf("#Lun-%2d ECC result:\n", lun);

		for (i = 0; i < 256; i++) {
			if (lun_ecc_statistics[lun][i])
				printf("ecc[%2d] = %-8ld %%%.1f\n", i, lun_ecc_statistics[lun][i],
					(100.0 * lun_ecc_statistics[lun][i]) / (count * (topage - frompage + 1) * dev->config->chunk_nsector));
		}
		printf("\n");
	}

	printf("#Global ECC result:\n");
	for (i = 0; i < 256; i++) {
		long strip_cnt = 0;
		int vluns = 0;

		for_dev_each_lun(dev, lun) {
			strip_cnt += lun_ecc_statistics[lun][i];
			vluns++;
		}

		if (strip_cnt)
			printf("ecc[%2d] = %-8ld %%%.1f\n", i, strip_cnt,
				(100.0 * strip_cnt) / (vluns * count * (topage - frompage + 1)  * dev->config->chunk_nsector));
	}

	/* success return */
	rc = 0;
free_req_out:
	list_for_each_entry_safe(req, tmp, &req_head, list) {
		list_del(&req->list);
		free_request(req);
	}

	if (raid) {
		free(raid_data);
		free(raid_metadata);
	}
free_lun_ecc_statistics:
	for_dev_each_lun(dev, lun) {
		if (NULL == lun_ecc_statistics[lun])
			break;
		free(lun_ecc_statistics[lun]);
	}
	free(lun_ecc_statistics);
out:
	if (rc)
		printf("Super-read encounter some error\n");
	return rc;
}
/*-----------------------------------------------------------------------------------------------------------*/
