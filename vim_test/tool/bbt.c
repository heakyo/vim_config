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
#include <sys/mman.h>

#include "tool.h"

/*------------------------------------------------------------------------------------------------------------------------------------------*/
int erase_scan(struct shannon_dev *dev, struct shannon_bbt *bbt)
{
	int rc = 0;
	int head, lun, plane, blk, ppa;
	struct shannon_request *chunk_head_req, *req, *tmp;
	struct list_head req_head;
	int pre_cent, now_cent;

	head = INDEP_HEAD;
	if (dev->config->nplane > 1)
		head |= (1 << SH_ERASE_PLANE_SHIFT);

	INIT_LIST_HEAD(&req_head);

	if (bbt->nblock == dev->flash->nblk) {
		pre_cent = now_cent = 0;
		print("All blocks erase scan...%%%02d", now_cent);
	}

	assert(bbt->nblock != 0);
	for (blk = 0; blk < bbt->nblock / dev->config->nplane; blk++) {
		ppa = blk * dev->config->nplane * dev->flash->npage;

		/* alloc erase request */
		for_dev_each_lun(dev, lun) {
			if (dev->targetlun[lun].blk_hole_count && blk*dev->config->nplane >= dev->targetlun[lun].blk_hole_begin) {
				set_bit(lun, bbt->sb_bbt[blk]);
				continue;
			}

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

		/* summit all request and execute them */
		list_for_each_entry(req, &req_head, list) {
			if ((rc = dev->submit_request(req)))
				goto free_req_out;
			update_cmdqueue(dev, req->lun);
		}

		// for_dev_each_lun(dev, lun)
		//	update_cmdqueue(dev, lun);

		for_dev_each_lun(dev, lun) {
			if ((rc = poll_cmdqueue(dev, lun)))
				goto free_req_out;
		}

		/* check status */
		list_for_each_entry(req, &req_head, list) {
			if (check_req_status_silent(req))
				set_bit(req->lun, bbt->sb_bbt[blk]);
		}

		/* print progress */
		if (bbt->nblock == dev->flash->nblk) {
			now_cent = 100 * (blk + 1) / (dev->flash->nblk / dev->config->nplane);

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

	if (bbt->nblock == dev->flash->nblk)
		printf("\n");

free_req_out:
	list_for_each_entry_safe(req, tmp, &req_head, list) {
		list_del(&req->list);
		free_request(req);
	}
	return rc;
}

/*
 * XXX: This function will change hardware configuration
 */
int flagbyte_scan(struct shannon_dev *dev, struct shannon_bbt *bbt)
{
	int i, rc = 0;
	int bs, ns, bb, nrow, j;
	int lun, plane, blk, ppa, head;
	struct shannon_request *chunk_head_req, *req, *tmp;
	struct list_head req_head;
	int pre_cent, now_cent;

	/* round up flash entire-page-size */
	dev->config->ecc_mode = ECCMODE_DISABLE;
	dev->config->raid_mode = RAIDMODE_DISABLE;
	assert(dev->config->sector_size_shift == 12);
	calculate_indirect_config(dev);
	dev->flash->oob_size = ((dev->flash->entire_page_size + dev->config->full_sector_size - 1) /
		dev->config->full_sector_size) * dev->config->full_sector_size - dev->flash->page_size;
	dev->flash->entire_page_size = dev->flash->page_size + dev->flash->oob_size;
	// printf("%s(): page_size = %d, oob_size = %d, entire_page_size = %d\n", __func__,
	//	dev->flash->page_size, dev->flash->oob_size, dev->flash->entire_page_size);
	if (re_init_device(dev)) {
		printf("%s(): Re-init-device failed\n", __func__);
		return ERR;
	}

	/* read and check flagbyte */
	head = INDEP_HEAD;
	if (dev->config->nplane > 1)
		head |= (1 << SH_READ_PLANE_SHIFT);

	nrow = 0;
	for (i = 0; dev->flash->factory_ivb[i].row != -1; i++)
		nrow++;

	INIT_LIST_HEAD(&req_head);

	if (bbt->nblock == dev->flash->nblk) {
		pre_cent = now_cent = 0;
		print("ALL blocks flagbyte scan...%%%02d", now_cent);
	}

	assert(bbt->nblock != 0);
	for (blk = 0; blk < bbt->nblock / dev->config->nplane; blk++) {
		for (i = 0; dev->flash->factory_ivb[i].row != -1; i++) {
			ppa = blk * dev->flash->npage * dev->config->nplane + dev->flash->factory_ivb[i].row;

			/* calculate location range of flagbyte */
			bs = dev->flash->factory_ivb[i].lo_col / dev->config->full_sector_size;
			ns = (dev->flash->factory_ivb[i].hi_col + dev->config->full_sector_size) / dev->config->full_sector_size - bs;
			assert(ns <= 8);
			bb = dev->flash->factory_ivb[i].lo_col % dev->config->full_sector_size;
			// if (0 == blk)
			//	printf("%s(): page=%d bs=%d, ns=%d, bb=%d, cnt=%d\n", __func__, dev->flash->factory_ivb[i].row, bs, ns, bb,
			//		dev->flash->factory_ivb[i].hi_col - dev->flash->factory_ivb[i].lo_col + 1);

			/* requests */
			for_dev_each_lun(dev, lun) {
				if (dev->targetlun[lun].blk_hole_count && blk*dev->config->nplane >= dev->targetlun[lun].blk_hole_begin) {
					set_bit(lun, bbt->sb_bbt[blk]);
					continue;
				}

				chunk_head_req = alloc_request(dev, sh_preread_cmd, lun, ppa, head, 0, 0);	// preread
				if (NULL == chunk_head_req) {
					rc = ALLOCMEM_FAILED;
					goto free_req_out;
				}
				list_add_tail(&chunk_head_req->list, &req_head);

				for (plane = 1; plane < dev->config->nplane; plane++) {
					req = alloc_request(dev, sh_preread_cmd, lun, ppa + plane * dev->flash->npage, head, 0, 0);
					if (NULL == req) {
						rc = ALLOCMEM_FAILED;
						goto free_req_out;
					}
					list_add_tail(&req->chunk_list, &chunk_head_req->chunk_list);
				}

				req = alloc_request(dev, sh_cacheread_cmd, lun, ppa, head, bs, 0);		// cacheread
				if (NULL == req) {
					rc = ALLOCMEM_FAILED;
					goto free_req_out;
				}
				req->rw_entire_buffer = 1;
				req->nsector = ns;
				req->data = malloc(ns * dev->config->full_sector_size);
				if (NULL == req->data) {
					rc = ALLOCMEM_FAILED;
					goto free_req_out;
				}
				list_add_tail(&req->list, &req_head);

				for (plane = 1; plane < dev->config->nplane; plane++) {
					req = alloc_request(dev, sh_cacheread_cmd, lun, ppa + plane * dev->flash->npage, head, bs, 0);
					if (NULL == req) {
						rc = ALLOCMEM_FAILED;
						goto free_req_out;
					}
					req->rw_entire_buffer = 1;
					req->nsector = ns;
					req->data = malloc(ns * dev->config->full_sector_size);
					if (NULL == req->data) {
						rc = ALLOCMEM_FAILED;
						goto free_req_out;
					}
					list_add_tail(&req->list, &req_head);
				}
			}

			/* summit all request and execute them */
			list_for_each_entry(req, &req_head, list) {
				if ((rc = dev->submit_request(req)))
					goto free_req_out;
				update_cmdqueue(dev, req->lun);
			}

			// for_dev_each_lun(dev, lun)
			//	update_cmdqueue(dev, lun);

			for_dev_each_lun(dev, lun) {
				if ((rc = poll_cmdqueue(dev, lun)))
					goto free_req_out;
			}

			/* check preread status and flagbyte */
			list_for_each_entry(req, &req_head, list) {
				if (req->opcode == sh_preread_cmd) {		// check preread status
					if (check_req_status_silent(req))
						set_bit(req->lun, bbt->sb_bbt[blk]);
					continue;
				}

				// printf("lun=%d, block=%d, page=%d, plane=%d:\n",
				//	req->lun, req->chunk_block, req->page, req->chunk_plane);
				// pr_u8_array_noprefix(req->data, dev->flash->factory_ivb[i].hi_col - dev->flash->factory_ivb[i].lo_col + 1, 32);

				for (j = 0; j < dev->flash->factory_ivb[i].hi_col - dev->flash->factory_ivb[i].lo_col + 1; j++) {
					//if (0xFF == req->data[j + bb]) {
					if (0x00 != req->data[j + bb]) {
						set_bit(req->lun, bbt->sb_bbt[blk]);
						break;
					}
				}
			}

			/* print progress */
			if (bbt->nblock == dev->flash->nblk) {
				now_cent = 100 * ((blk + 1) * nrow + i) / ((dev->flash->nblk / dev->config->nplane) * nrow);

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
	} /* end for_dev_each_block */

	if (bbt->nblock == dev->flash->nblk)
		printf("\n");

free_req_out:
	list_for_each_entry_safe(req, tmp, &req_head, list) {
		list_del(&req->list);
		free_request(req);
	}
	dev->flash->oob_size = dev->flash_bakup->oob_size;
	dev->flash->entire_page_size = dev->flash_bakup->entire_page_size;
	return rc;
}

void human_bbt_info(char *bbtfile)
{
	int fd;
	struct stat stat;
	struct shannon_bbt *bbt;
	int lun, blk;

	if ((fd = open(bbtfile, O_RDONLY)) < 0) {
		perror("Open bbt file failed:");
		return;
	}

	if (fstat(fd, &stat)) {
		perror("Fstat bbt file failed:");
		return;
	}
	assert(stat.st_size != 0);

	// printf("bbt file size=%d\n", (int)stat.st_size);
	if ((bbt = mmap(0, stat.st_size, PROT_READ, MAP_SHARED, fd, 0)) == NULL) {
		perror("Mmap bbt file failed:");
		close(fd);
		return;
	}

	if (strcmp("shannon-bbt", bbt->name)) {
		printf("Invalid bbtfile\n");
		munmap(bbt, stat.st_size);
		close(fd);
		return;
	}

	printf("nchannel=%d\n", bbt->nchannel);
	printf("nthread=%d\n", bbt->nthread);
	printf("nlun=%d\n", bbt->nlun);
	printf("nplane=%d\n", bbt->nplane);
	printf("nblock=%d\n", bbt->nblock);
	printf("npage=%d\n", bbt->npage);
	printf("size=%d\n", bbt->size);
	assert(bbt->size == stat.st_size);

	for (lun = 0; lun < bbt->nchannel * bbt->nthread *bbt->nlun; lun++) {
		printf("Lun-%3d:", lun);
		for (blk = 0; blk < bbt->nblock/bbt->nplane; blk++) {
			if (test_bit(lun, bbt->sb_bbt[blk]))
				printf(" %d", blk);
		}
		printf("\n");
	}

	munmap(bbt, stat.st_size);
	close(fd);
}

/*
 * bbt operations
 */
void shannon_bbt_ops_usage(void)
{
	printf("Description:\n");
	printf("\tScan flash to get bad block table (bbt)\n\n");

	printf("Usage:\n");
	printf("\tbbt bbtfile [option]\n\n");

	printf("Option:\n");
	printf("\t-e, --erase\n"
		"\t\tErase flash and put per-blcok erase status into bbt file\n\n");
	printf("\t-f, --flagbyte\n"
		"\t\tRead bad blcok flagbyte and put it into bbt file\n\n");
	printf("\t-g, --singleplane\n"
		"\t\tUse singleplane cmd to manipulate flash and get singleplane bbt file. If no this option, use plane configuration read from 'config' file\n\n");
	printf("\t-h, --help\n"
		"\t\tdisplay this help and exit\n");
}

int shannon_bbt_ops(struct shannon_dev *dev, int argc, char **argv)
{
	struct option longopts [] = {
		{"erase", no_argument, NULL, 'e'},
		{"flagbyte", no_argument, NULL, 'f'},
		{"singleplane", no_argument, NULL, 'g'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int opt;
	int rc = ERR;
	struct shannon_bbt *bbt;
	int fd, bbt_size;
	int erase, flagbyte, singleplane;

	erase = 0;
	flagbyte = 0;
	singleplane = 0;

	while ((opt = getopt_long(argc, argv, ":efgh", longopts, NULL)) != -1) {
		switch (opt) {
		case 'e':
			erase = 1;
			break;
		case 'f':
			flagbyte = 1;
			break;
		case 'g':
			singleplane = 1;
			break;
		case 'h':
			shannon_bbt_ops_usage();
			return 0;
		default:
			shannon_bbt_ops_usage();
			return ERR;		
		}
	}

	if ((argc - optind) != 1) {
		shannon_bbt_ops_usage();
		return ERR;
	}

	/* scan flash to get bbt file */
	if ((fd = open(argv[optind], O_CREAT|O_TRUNC|O_RDWR, 0666)) < 0) {
		perror("Create bbt file");
		return ERR;
	}

	/* re-config as default mode */
	dev->config->sector_size_shift = 12;
	dev->config->sector_ncodeword = 1;
	dev->config->ecc_mode = 1;
	dev->config->raid_mode = 0;
	if (singleplane)
		dev->config->nplane = 1;

	if (re_init_device(dev)) {
		close(fd);
		printf("Re-init-device failed\n");
		return ERR;
	}

	/* bbt struct */
	bbt_size = sizeof(*bbt) + (dev->flash->nblk / dev->config->nplane) * MAX_LUN_NBYTE;
	bbt = zmalloc(bbt_size);
	if (NULL == bbt)
		return ALLOCMEM_FAILED;
	sprintf(bbt->name, "shannon-bbt");
	bbt->nchannel = dev->config->nchannel;
	bbt->nthread = dev->config->nthread;
	bbt->nlun = dev->config->nlun;
	bbt->nplane = dev->config->nplane;
	bbt->nblock = dev->flash->nblk;
	bbt->npage = dev->flash->npage;
	bbt->size = bbt_size;

	/* scan */
	if (!erase && !flagbyte) {
		printf("Please select scan way: -e(--erase) or -f(--flagbyte)\n");
		goto out;
	}

	if (erase) {
		if (erase_scan(dev, bbt)) {
			printf("Erase scan failed\n");
			goto out;
		}
	}

	if (flagbyte) {
		if (flagbyte_scan(dev, bbt)) {
			printf("Flagbyte scan failed\n");
			goto out;
		}
	}

	/* save bbt file */
	if (write(fd, bbt, bbt_size) != bbt_size) {
		perror("Write bbt file failed\n");
		goto out;
	}

	rc = 0;
out:
	free(bbt);
	close(fd);
	return rc;
}

int check_bbt(struct shannon_dev *dev, struct shannon_bbt *bbt)
{
	if (strcmp("shannon-bbt", bbt->name))
		return ERR;
	if (dev->config->nchannel != bbt->nchannel)
		return ERR;
	if (dev->config->nthread != bbt->nthread)
		return ERR;
	if (dev->config->nlun != bbt->nlun)
		return ERR;
	if (dev->config->nplane != bbt->nplane)
		return ERR;
	if (dev->flash->nblk != bbt->nblock)
		return ERR;
	if (dev->flash->npage != bbt->npage)
		return ERR;
	return 0;
}

/*------------------------------------------------------------------------------------------------------------------------------------------*/
void human_luninfo_info(char *luninfo_file)
{
	int fd;
	struct stat stat;
	struct shannon_luninfo *luninfo;
	int blk;

	if ((fd = open(luninfo_file, O_RDONLY)) < 0) {
		perror("Open luninfo file failed:");
		return;
	}

	if (fstat(fd, &stat)) {
		perror("Fstat luninfo file failed:");
		close(fd);
		return;
	}
	assert(stat.st_size != 0);

	// printf("luninfo file size=%d\n", (int)stat.st_size);
	if ((luninfo = mmap(0, stat.st_size, PROT_READ, MAP_SHARED, fd, 0)) == NULL) {
		perror("Mmap luninfo file failed:");
		close(fd);
		return;
	}

	if (strcmp("shannon-luninfo", luninfo->name)) {
		printf("Invalid luninfo file\n");
		munmap(luninfo, stat.st_size);
		close(fd);
		return;
	}

	printf("nchannel=%d\n", luninfo->nchannel);
	printf("nthread=%d\n", luninfo->nthread);
	printf("nlun=%d\n", luninfo->nlun);
	printf("nplane=%d\n", luninfo->nplane);
	printf("nblock=%d\n", luninfo->nblock);
	printf("npage=%d\n", luninfo->npage);
	printf("size=%d\n", luninfo->size);
	assert(luninfo->size == stat.st_size);

	for (blk = 0; blk < luninfo->nblock / luninfo->nplane; blk++) {
		printf("super-blk %4d: ", blk);
		printf("luns=%2d parity_lun=%2d bbt=", luninfo->sb_luninfo[blk].luns, luninfo->sb_luninfo[blk].parity_lun);
		pr_u8_array_noprefix(luninfo->sb_luninfo[blk].sb_bbt,  MAX_LUN_NBYTE, MAX_LUN_NBYTE);
	}

	munmap(luninfo, stat.st_size);
	close(fd);
}

static void shannon_luninfo_ops_usage(void)
{
	printf("Description:\n");
	printf("\tFake luninfo file which inclues nlun/ndatalun/parity/bbt_bitmap\n\n");

	printf("Usage:\n");
	printf("\tluninfo luninfo-file bbt-file [option]\n\n");

	printf("Option:\n");
	printf("\t-e, --bbt-seed=SEED\n"
		"\t\tPer-super-block, bbt = random data OR bbt read from bbtfile. If no select, just use bbt from bbtfile\n\n");
	printf("\t-E, --parity-lun-seed=SEED\n"
		"\t\tHow to get parity_lun, if no select this option, parity_lun is the last valid lun in this super-block\n\n");
	printf("\t-h, --help\n"
		"\t\tdisplay this help and exit\n");
}

int shannon_luninfo_ops(struct shannon_dev *dev, int argc, char **argv)
{
	struct option longopts [] = {
		{"bbt-seed", required_argument, NULL, 'e'},
		{"parity-lun-seed", required_argument, NULL, 'E'},
		{0, 0, 0, 0},
	};
	int opt;
	int i, rc = ERR;
	int fd, bbt_fd;
	int lun, blk;
	int bbt_seed, parity_lun_seed;
	struct shannon_bbt *bbt;
	struct stat stat;
	int luninfo_size;
	struct shannon_luninfo *luninfo;

	bbt_seed = parity_lun_seed = 0;

	while ((opt = getopt_long(argc, argv, ":e:E:h", longopts, NULL)) != -1) {
		switch (opt) {
		case 'e':
			bbt_seed = strtoul(optarg, NULL, 10);
			break;
		case 'E':
			parity_lun_seed = strtoul(optarg, NULL, 10);
			break;
		case 'h':
			shannon_luninfo_ops_usage();
			return 0;
		default:
			shannon_luninfo_ops_usage();
			return ERR;
		}
	}

	if ((argc - optind) != 2) {
		shannon_luninfo_ops_usage();
		return ERR;
	}

	/* open bbtfile */
	if ((bbt_fd = open(argv[optind + 1], O_RDONLY)) < 0) {
		perror("Open bbt file failed:");
		return ERR;
	}

	if (fstat(bbt_fd, &stat)) {
		perror("Fstat bbt file failed:");
		close(bbt_fd);
		return ERR;
	}

	bbt = malloc(stat.st_size);
	if (NULL == bbt) {
		close(bbt_fd);
		printf("Alloc bbt memory failed");
		return ERR;
	}

	if (read(bbt_fd, bbt, stat.st_size) != stat.st_size) {
		printf("Read bbt file failed\n");
		goto close_bbt;
	}
	assert(bbt->size == stat.st_size);

	if (check_bbt(dev, bbt)) {
		printf("The config of bbt doesn`t match present hardware config, please use command 'bbt' to generate present bbtfile\n");
		goto close_bbt;
	}

	/* create sb_luninfo file*/
	if ((fd = open(argv[optind], O_CREAT|O_TRUNC|O_RDWR, 0666)) < 0) {
		perror("Create sb_luninfo_file failed:");
		goto close_bbt;
	}

	luninfo_size = sizeof(*luninfo) + (dev->flash->nblk / dev->config->nplane) * sizeof(struct shannon_sb_luninfo);
	luninfo = zmalloc(luninfo_size);
	if (NULL == luninfo)
		goto close_fd;
	sprintf(luninfo->name, "shannon-luninfo");
	luninfo->nchannel = dev->config->nchannel;
	luninfo->nthread = dev->config->nthread;
	luninfo->nlun = dev->config->nlun;
	luninfo->nplane = dev->config->nplane;
	luninfo->nblock = dev->flash->nblk;
	luninfo->npage = dev->flash->npage;
	luninfo->size = luninfo_size;

	/* fake sb_luninfo bbt */
	if (bbt_seed) {
		srand(bbt_seed);

		for_dev_each_block(dev, blk) {
			__u8 p[MAX_LUN_NBYTE];

			pad_rand(p, MAX_LUN_NBYTE);

			for (i = 0; i < MAX_LUN_NBYTE; i++)
				 ((__u8 *)bbt->sb_bbt[blk])[i] |= p[i];
		}
	}

	/* caculate sb_luninfo */
	srand(parity_lun_seed);

	for_dev_each_block(dev, blk) {
		int off;

		for_dev_each_lun(dev, lun) {
			if (!test_bit(lun, bbt->sb_bbt[blk]))
				luninfo->sb_luninfo[blk].luns++;
			else
				set_bit(lun, luninfo->sb_luninfo[blk].sb_bbt);
		}

		if (luninfo->sb_luninfo[blk].luns > 1) {
			luninfo->sb_luninfo[blk].ndatalun = luninfo->sb_luninfo[blk].luns - 1;
		} else {
			luninfo->sb_luninfo[blk].luns = 0;
			continue;
		}

		/* fake parity_lun idx*/
		if (parity_lun_seed)
			off = rand() % luninfo->sb_luninfo[blk].luns;
		else
			off = luninfo->sb_luninfo[blk].luns - 1;

		for_dev_each_lun(dev, lun) {
			if (!test_bit(lun, bbt->sb_bbt[blk]))
				if (off-- == 0)
					break;
		}
		luninfo->sb_luninfo[blk].parity_lun = lun;
	}

	if (write(fd, luninfo, luninfo_size) != luninfo_size) {
		perror("Write luninfo file failed:");
		goto free_luninfo;
	}

	rc = 0;
free_luninfo:
	free(luninfo);
close_fd:
	close(fd);
close_bbt:
	free(bbt);
	close(bbt_fd);
	return rc;
}

int check_luninfo(struct shannon_dev *dev, struct shannon_luninfo *luninfo)
{
	if (strcmp("shannon-luninfo", luninfo->name))
		return ERR;
	if (dev->config->nchannel != luninfo->nchannel)
		return ERR;
	if (dev->config->nthread != luninfo->nthread)
		return ERR;
	if (dev->config->nlun != luninfo->nlun)
		return ERR;
	if (dev->config->nplane != luninfo->nplane)
		return ERR;
	if (dev->flash->nblk != luninfo->nblock)
		return ERR;
	if (dev->flash->npage != luninfo->npage)
		return ERR;
	return 0;
}
/*------------------------------------------------------------------------------------------------------------------------------------------*/
