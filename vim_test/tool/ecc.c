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

static int max_ecc_errbits = -1;	// !-1 means generate rand errbits in the randge of [0, max_ecc_errbits]
static int fixed_ecc_errbits = -1;	// !-1 means generate fixed errbits
static int fixed_ecc_ratio = -1;	// !-1 means ratio = 1, otherwise ratio = 1/fixed_ecc_ratio

static int org_ncodeword = 0, org_codeword_size = 0;

/*-----------------------------------------------------------------------------------------------------------*/
static void __modify_error_bit(__u8 *data, int num)
{
	int i, off, byte, mask;

	off = rand();

	for (i = 0; i < num; i++, off++) {
		byte = get_nrand(off) / 8;
		mask = 1 << (get_nrand(off) % 8);
		// printf("%d ", get_nrand(off));

		if (data[byte] & mask)
			data[byte] &= ~mask;
		else
			data[byte] |= mask;
	}
	// printf("\n");
}

static int __ecc_noecc_write(struct shannon_dev *dev, int head, int blk, int page, int seed)
{
	int i, rc = 0;
	__u8 *data;
	struct shannon_request *chunk_head_req, *req, *tmp;
	int lun, plane, ppa;
	struct list_head req_head, tmp_head;

	assert(org_ncodeword != 0 && org_codeword_size != 0);

	ppa = blk * dev->config->nplane * dev->flash->npage + page;

	INIT_LIST_HEAD(&req_head);
	head = (head & ~HEAD_MASK) | INDEP_HEAD;

	for_dev_each_lun(dev, lun) {
		if (is_bad_lunblock(dev, lun, blk))
			continue;

		chunk_head_req = NULL;

		/* requests of cacheread */
		for (plane = 0; plane < dev->config->nplane; plane++) {					// cacheread
			int ns, bs = 0;
			int remain_ns = dev->config->page_nsector;

			data = malloc(dev->config->page_nsector * (dev->config->sector_size + METADATA_SIZE));

			while (remain_ns) {
				ns = (remain_ns >= 8) ? 8 : remain_ns;

				req = alloc_request(dev, sh_cacheread_cmd, lun, ppa + plane * dev->flash->npage, head, bs, 0);
				if (NULL == req) {
					rc = ALLOCMEM_FAILED;
					goto free_req_out;
				}
				req->bfree = 0;
				req->rw_entire_buffer = 1;
				req->nsector = ns;
				req->data = data + bs * (dev->config->sector_size + METADATA_SIZE);
				list_add_tail(&req->list, &req_head);

				bs += ns;
				remain_ns -= ns;
			}

			/* requests of write without ecc*/
			req = alloc_request(dev, sh_write_cmd, lun, ppa + plane * dev->flash->npage, head, 0, 0);
			if (NULL == req) {
				rc = ALLOCMEM_FAILED;
				goto free_req_out;
			}
			req->rw_entire_buffer = 1;
			req->nsector = dev->config->page_nsector;
			req->data = data;

			if (NULL != chunk_head_req) {
				list_add_tail(&req->chunk_list, &chunk_head_req->chunk_list);
			} else {
				chunk_head_req = req;
				list_add_tail(&chunk_head_req->list, &req_head);
			}
		}
	}

	/* execute cachereead first */
	list_for_each_entry(req, &req_head, list) {
		if (req->opcode == sh_cacheread_cmd) {
			if ((rc = dev->submit_request(req)))
				goto free_req_out;
		}
	}

	for_dev_each_lun(dev, lun)
		update_cmdqueue(dev, lun);

	for_dev_each_lun(dev, lun) {
		if ((rc = poll_cmdqueue(dev, lun)))
			goto free_req_out;
	}

	list_for_each_entry_safe(req, tmp, &req_head, list) {
		if (req->opcode == sh_cacheread_cmd) {
			list_del(&req->list);
			free_request(req);
		}
	}

	/* make error bits */
	INIT_LIST_HEAD(&tmp_head);
	srand(seed);
	assert(max_ecc_errbits != -1 || fixed_ecc_errbits != -1);

	list_for_each_entry(chunk_head_req, &req_head, list) {	// now only write requests are remained in the req_head link
		list_add_tail(&tmp_head, &chunk_head_req->chunk_list);
		list_for_each_entry(req, &tmp_head, chunk_list) {
			for (i = 0; i < org_ncodeword; i++) {
				if (max_ecc_errbits != -1) {
					__modify_error_bit(req->data + i * org_codeword_size, rand() % (max_ecc_errbits + 1));
				} else {
					if (-1 == fixed_ecc_ratio) {
						__modify_error_bit(req->data + i * org_codeword_size, fixed_ecc_errbits);
					} else {
						if (0 == rand() % fixed_ecc_ratio)
							__modify_error_bit(req->data + i * org_codeword_size, fixed_ecc_errbits);
					}
				}
			}
		}
		list_del(&tmp_head);
	}

	/* write to flash with ecc disabled */
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

free_req_out:
	list_for_each_entry_safe(req, tmp, &req_head, list) {
		list_del(&req->list);
		free_request(req);
	}
	return rc;
}

static int __ecc_dummy_write(struct shannon_dev *dev, int head, int blk, int page, int seed)
{
	int i, rc = 0;
	int sector_seed;
	int lun, plane, ppa;
	struct shannon_request *chunk_head_req, *req, *tmp;
	struct list_head req_head;

	assert(dev->config->ecc_mode != ECCMODE_DISABLE);

	head |= (1 << SH_WRITE_DUMMY_SHIFT);
	ppa = blk * dev->config->nplane * dev->flash->npage + page;

	INIT_LIST_HEAD(&req_head);

	/* raid init if needed */
	if (dev->config->raid_mode && (head & HEAD_MASK) != INDEP_HEAD) {
		req = alloc_request(dev, sh_raidinit_cmd, superblock_paritylun(dev, blk), ppa, head, 0, superblock_ndatalun(dev, blk));
		if (NULL == req)  {
			rc = ALLOCMEM_FAILED;
			goto free_req_out;
		}
		list_add_tail(&req->list, &req_head);
	}

	/* write chunk */
	for_dev_each_lun(dev, lun) {
		if (is_bad_lunblock(dev, lun, blk))
			continue;

		if (dev->config->raid_mode && (head & HEAD_MASK) != INDEP_HEAD &&  superblock_paritylun(dev, blk) == lun)	// raid lun
			continue;

		chunk_head_req = alloc_request(dev, sh_write_cmd, lun, ppa, head, 0, dev->config->page_nsector);
		if (NULL == chunk_head_req) {
			rc = ALLOCMEM_FAILED;
			goto free_req_out;
		}
		for (i = 0; i < dev->config->page_nsector; i++) {
			sector_seed = (ppa * dev->config->page_nsector + i) + ((lun + seed)<<24);
			srand(sector_seed);
			pad_rand(chunk_head_req->data + i * dev->config->sector_size, dev->config->sector_size);
			pad_rand(chunk_head_req->metadata + i, METADATA_SIZE);
			// pad_same_u32(chunk_head_req->data + i * dev->config->sector_size, dev->config->sector_size, sector_seed);
			// pad_same_u32(chunk_head_req->metadata + i, METADATA_SIZE, sector_seed);
		}
		list_add_tail(&chunk_head_req->list, &req_head);

		for (plane = 1; plane < dev->config->nplane; plane++) {
			req = alloc_request(dev, sh_write_cmd, lun, ppa + plane * dev->flash->npage, head, 0, dev->config->page_nsector);
			if (NULL ==req) {
				rc = ALLOCMEM_FAILED;
				goto free_req_out;
			}
			for (i = 0; i < dev->config->page_nsector; i++) {
				sector_seed = ((ppa + plane * dev->flash->npage) * dev->config->page_nsector + i) + ((lun + seed)<<24);
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
		chunk_head_req = alloc_request(dev, sh_raidwrite_cmd, superblock_paritylun(dev, blk), ppa, head, 0, 0);
		if (NULL == chunk_head_req) {
			rc = ALLOCMEM_FAILED;
			goto free_req_out;
		}
		list_add_tail(&chunk_head_req->list, &req_head);

		for (plane = 1; plane < dev->config->nplane; plane++) {
			req = alloc_request(dev, sh_raidwrite_cmd, superblock_paritylun(dev, blk), ppa + plane * dev->flash->npage, head, 0, 0);
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
		if (sh_raidinit_cmd == req->opcode)	// no need to check status for raid-init
			continue;
		rc |= check_req_status_silent(req);
	}

	/* free request */
	list_for_each_entry_safe(req, tmp, &req_head, list) {
		list_del(&req->list);
		free_request(req);
	}

free_req_out:
	list_for_each_entry_safe(req, tmp, &req_head, list) {
		list_del(&req->list);
		free_request(req);
	}
	return rc;
}

/*-----------------------------------------------------------------------------------------------------------*/
static void shannon_fake_ecc_usage(void)
{
	printf("Description:\n");
	printf("\tFake specified number of ecc errbits in flash cell\n\n");

	printf("Usage:\n");
	printf("\tfake-ecc [option] begin_chunkblock count\n\n");

	printf("Option:\n");
	printf("\t-f, --luninfo=luninfo-file\n"
		"\t\tSpecify luninfo which includes ndatalun/parity_lun/sb_bbt, otherwise assume all blocks are valid and parity_lun is the last lun\n\n");
	printf("\t-e, --seed=SEDD\n"
		"\t\tSeed for generate random data write to flash. Default use gettimeofday.tv_usec\n\n");
	printf("\t-H, --head=HEAD\n"
		"\t\tSelect head, default is head 2.If head is 0 or 1, hardware config of raid should be enabled and will write with raid\n\n");
	printf("\t-F, --fixed-ecc-errbits=num\n"
		"\t\tSet per-codeword has fixed number of ecc errbits\n\n");
	printf("\t-R, --fixed-ecc-ratio=ratio\n"
		"\t\tSet ratio of fixed number ecc errbits as 1/ratio. Default 1 if no this option, only valid when -F/--fixed-ecc-errbits\n\n");
	printf("\t-M, --max-ecc-errbits=num\n"
		"\t\tWill generate rand number of ecc errbits for per-codeword, this option specify max value\n\n");
	printf("\t-N, --noprogress\n"
		"\t\tDisable show progress\n\n");
	printf("\t-t, --absent-luns=loglun|phylun:x1-x2,x3,x4-x5...\n"
		"\t\tSpecify absent luns\n\n");
	printf("\t-T, --present-luns=loglun|phylun:x1-x2,x3,x4-x5...\n"
		"\t\tSpecify present luns\n\n");
	printf("\t-h, --help\n"
		"\t\tDisplay this help and exit\n\n");

	printf("NOTE:\n\tBlock index is count by chunk.\n\tDo super-erase previous this command!!!\n");
}

int shannon_fake_ecc(struct shannon_dev *dev, int argc, char **argv)
{
	struct option longopts [] = {
		{"luninfo", required_argument, NULL, 'f'},
		{"seed", required_argument, NULL, 'e'},
		{"head", required_argument, NULL, 'H'},
		{"fixed-ecc-errbits", required_argument, NULL, 'F'},
		{"fixed-ecc-ratio", required_argument, NULL, 'R'},
		{"max-ecc-errbits", required_argument, NULL, 'M'},
		{"noprogress", no_argument, NULL, 'N'},
		{"absent-luns", required_argument, NULL, 't'},
		{"present-luns", required_argument, NULL, 'T'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int rc = 0;
	int opt;
	char *luninfo_file;
	int inc, seed, seed_way;
	int begin_chunkblock, count;
	int head, blk, page; 
	int noprogress, pre_cent, now_cent;
	int itmp, fake_oob_size;

	if (ECCMODE_DISABLE == dev->config->ecc_mode) {
		printf("Ecc must be enabled\n");
		return ERR;
	}

	luninfo_file = NULL;
	inc = 0;
	seed = getseed(0);
	seed_way = 0;
	head = INDEP_HEAD;
	noprogress = 0;

	while ((opt = getopt_long(argc, argv, ":f:e:H:F:R:M:Nt:T:h", longopts, NULL)) != -1) {
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
		case 'F':
			if (max_ecc_errbits != -1) {
				printf("Don`t set option 'max-ecc-errbits' and 'fixed-ecc-errbits' at the same time\n");
				return ERR;
			}
			fixed_ecc_errbits = strtoul(optarg, NULL, 10);
			if (fixed_ecc_errbits < 0 || fixed_ecc_errbits > dev->tmode) {
				printf("This value should be [0, %d]\n", dev->tmode);
				return ERR;
			}
			break;
		case 'R':
			fixed_ecc_ratio = strtoul(optarg, NULL, 10);
			if (fixed_ecc_ratio <= 0) {
				printf("This value should > 0\n");
				return ERR;
			}
			break;
		case 'M':
			if (fixed_ecc_errbits != -1) {
				printf("Don`t set option 'max-ecc-errbits' and 'fixed-ecc-errbits' at the same time\n");
				return ERR;
			}
			max_ecc_errbits = strtoul(optarg, NULL, 10);
			if (max_ecc_errbits < 0 || max_ecc_errbits > dev->tmode) {
				printf("This value should be [0, %d]\n", dev->tmode);
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
		case 'h':
			shannon_fake_ecc_usage();
			return 0;
		default:
			shannon_fake_ecc_usage();
			return ERR;
		}
	}

	if (argc - optind != 2) {
		shannon_fake_ecc_usage();
		return ERR;
	}

	if (-1 == max_ecc_errbits && -1 == fixed_ecc_errbits) {
		printf("Please set option [-F, fixed-ecc-errbits] or [-M, max-ecc-errbits]\n");
		return ERR;
	}

	begin_chunkblock = strtoul(argv[optind], NULL, 10);
	count = strtoul(argv[optind + 1], NULL, 10);

	if ((begin_chunkblock + count) > dev->flash->nblk/dev->config->nplane) {
		printf("Overflow address\n");
		return OVERFLOW_ADDR;
	}

	if (HEAD0 == head || HEAD1 == head) {
		if (!dev->config->raid_mode) {
			printf("You must enable raid when write by head0 or head1. Please rm file 'config' then reconfig hardware\n");
			return ERR;
		}
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

	if (dev->config->nplane > 1)
		head |= (1 << SH_WRITE_PLANE_SHIFT);

	if (!noprogress) {
		pre_cent = now_cent = 0;
		print("Fake-ecc in process...%%%02d", now_cent);
	}

	/* calculate org_ncodeword, org_codeword_size and fake_oob_size */
	org_codeword_size = dev->config->codeword_size;
	org_ncodeword = dev->config->page_nsector * dev->config->sector_ncodeword;

	dev->config->ecc_mode = ECCMODE_DISABLE;
	calculate_indirect_config(dev);
	itmp = (org_ncodeword * org_codeword_size + dev->config->sector_ncodeword * dev->config->codeword_size - 1) /
			(dev->config->sector_ncodeword * dev->config->codeword_size);
	fake_oob_size = itmp * dev->config->sector_ncodeword * dev->config->codeword_size - dev->flash->page_size;
	if (fake_oob_size < 0)
		fake_oob_size = dev->flash->oob_size;

	dev->config->ecc_mode = dev->config_bakup->ecc_mode;
	dev->config->sector_ncodeword = dev->config_bakup->sector_ncodeword;
	calculate_indirect_config(dev);	// restore

	/* pre-generate no-repeat random data */
	init_nrand(org_codeword_size * 8, seed);

	/* manipulate process */
	for (blk = begin_chunkblock; blk < begin_chunkblock + count; blk++) {
		if (is_bad_superblock(dev, blk))
			continue;

		for_dev_each_page(dev, page) {
			/* dummy write to get origin data and its ecc data */
			dev->flash->oob_size = dev->flash_bakup->oob_size;
			dev->flash->entire_page_size = dev->flash->page_size + dev->flash->oob_size;

			dev->config->raid_mode = dev->config_bakup->raid_mode;
			dev->config->ecc_mode = dev->config_bakup->ecc_mode;
			dev->config->sector_ncodeword = dev->config_bakup->sector_ncodeword;
			re_init_device(dev);

			if ((rc = __ecc_dummy_write(dev, head, blk, page, seed))) {
				printf("__ecc_dummy_write() failed\n");
				return rc;
			}

			/* fake errbits in origin data and write with ecc disable */
			dev->flash->oob_size = fake_oob_size;
			dev->flash->entire_page_size = dev->flash->page_size + dev->flash->oob_size;

			dev->config->raid_mode = RAIDMODE_DISABLE;
			dev->config->ecc_mode = ECCMODE_DISABLE;
			re_init_device(dev);

			if ((rc = __ecc_noecc_write(dev, head, blk, page, seed + inc))) {
				printf("__ecc_noecc_write() failed\n");
				return rc;
			}

			/* helper opertions */
			inc++;

			if (noprogress)
				continue;

			now_cent = 100 * ((blk - begin_chunkblock) * dev->flash->npage + page + 1) / (count * dev->flash->npage);

			if (now_cent > pre_cent) {
				print("\b\b\b%%%02d", now_cent);
				pre_cent = now_cent;
			}
		}
	}

	printf("\n");

	exit_nrand();
	return rc;
}

/*-----------------------------------------------------------------------------------------------------------*/
static int prepare_skip_empty_block(struct shannon_dev *dev)
{
	int i, j, active;
	int head, page, ppa, plane, blk, lun, valid_blks;
	struct shannon_request *chunk_head_req, *req, *tmp;
	struct list_head req_head;
	unsigned long cur_blk_bitmap[MAX_LUN_NLONG];
	unsigned long saved_blk_bitmap[MAX_LUN_NLONG];
	int flag;

	if (shannon_mpt_readbbt(dev, 0)) {
		printf("Read bbt failed\n");
		return ERR;
	}

	memcpy(dev->config, dev->config_bakup, sizeof(*dev->config));
	if (re_init_device(dev)) {
		printf("Re-init-device (WRITE-MBR) failed\n");
		return ERR;
	}
	dev->clear_queue(dev);

	active = 0;
	head = INDEP_HEAD;
	if (dev->config->nplane > 1)
		head |= (1 << SH_READ_PLANE_SHIFT);
	INIT_LIST_HEAD(&req_head);

	print("%s()...", __func__);

	/* skip MBR blocks */
	for (blk = 0; blk < 4 / dev->config->nplane; blk++) {
		for_dev_each_lun(dev, lun)
			set_bit(lun, dev->sb[blk].sb_luninfo.sb_bbt);
	}

	/* skip superblock wtich less than total_luns/2 */
	for_dev_each_block(dev, blk) {
		valid_blks = 0;

		for_dev_each_lun(dev, lun) {
			if (!test_bit(lun, dev->sb[blk].sb_luninfo.sb_bbt))
				valid_blks++;
		}

		if (valid_blks > dev->config->luns / 2) {
			dev->sb[blk].sb_luninfo.luns = valid_blks;
		} else {
			// printf("skip superblock blk=%d which less than total_luns / 2\n", blk);
			dev->sb[blk].sb_luninfo.luns = 0;

			for_dev_each_lun(dev, lun)
				set_bit(lun, dev->sb[blk].sb_luninfo.sb_bbt);
		}
	}

#if 0
{
	int lb;

	printf("BBT map\n");
	for_dev_each_block(dev, lb)
		printf("blk=%04d: %016lX\n", lb, dev->sb[lb].sb_luninfo.sb_bbt[0]);
	printf("\n");
}
#endif

	/* skip empty blocks */
	for_dev_each_block(dev, blk) {
		if (is_bad_superblock(dev, blk))
			continue;

		page = 0;
		ppa = blk * dev->flash->npage * dev->config->nplane;

		memcpy(saved_blk_bitmap, dev->sb[blk].sb_luninfo.sb_bbt, sizeof(saved_blk_bitmap));
last_page:
		memset(cur_blk_bitmap, 0x00, sizeof(cur_blk_bitmap));

		for_dev_each_lun(dev, lun) {
			if (test_bit(lun, dev->sb[blk].sb_luninfo.sb_bbt))	// will skip empty block when read last page
				continue;

			chunk_head_req = alloc_request(dev, sh_preread_cmd, lun, ppa + page, head, 0, 0);
			if (NULL == chunk_head_req)
				return ERR;
			list_add_tail(&chunk_head_req->list, &req_head);

			for (plane = 1; plane < dev->config->nplane; plane++) {
				req = alloc_request(dev, sh_preread_cmd, lun, ppa + page + plane * dev->flash->npage, head, 0, 0);
				if (NULL == req)
					return ERR;
				list_add_tail(&req->chunk_list, &chunk_head_req->chunk_list);
			}

			for (plane = 0; plane < dev->config->nplane; plane++) {
				int ns, bs = 0;
				int remain_ns = dev->config->page_nsector;

				while (remain_ns) {
					ns = (remain_ns >= 8) ? 8 : remain_ns;

					req = alloc_request(dev, sh_cacheread_cmd, lun, ppa + page + plane * dev->flash->npage, head, bs, ns);
					if (NULL == req)
						return ERR;
					list_add_tail(&req->list, &req_head);

					bs += ns;
					remain_ns -= ns;
				}
			}
		}

		list_for_each_entry(req, &req_head, list) {
			if (dev->submit_request(req)) {
				printf("submit_request failed\n");
				return ERR;
			}
		}

		for_dev_each_lun(dev, lun)
			update_cmdqueue(dev, lun);

		for_dev_each_lun(dev, lun) {
			if (poll_cmdqueue(dev, lun)) {
				printf("poll_cmdqueue failed\n");
				return ERR;
			}
		}

		list_for_each_entry_safe(req, tmp, &req_head, list) {
			if (sh_cacheread_cmd == req->opcode) {
				for (i = 0; i < req->nsector; i++) {
					if (0xFB == req->ecc[i]) {	// empty block
						set_bit(req->lun, dev->sb[req->chunk_block].sb_luninfo.sb_bbt);
						set_bit(req->lun, cur_blk_bitmap);
						if (0 != page) {
							for (j = 0; j < active; j++) {
								if (req->chunk_block == dev->active_block[j])
									break;
							}
							if (j == active)
								dev->active_block[active++] = req->chunk_block;
						}
					} else if (req->ecc[i] > 0xFB) {
						printf("INVALID ecc value block=%d ecc=0x%02X\n", req->chunk_block, req->ecc[i]);
						return ERR;
					}
				}
			}
			list_del(&req->list);
			free_request(req);
		}

		/* check FTL driver fatal */
		for_dev_each_lun (dev, lun) {
			if (!test_bit(lun, saved_blk_bitmap)) {
				flag = test_bit(0, cur_blk_bitmap);
				break;
			}
		}

		for_dev_each_lun(dev, lun) {
			if (test_bit(lun, saved_blk_bitmap))
				continue;

			if (test_bit(lun, cur_blk_bitmap) != flag) {
				printf("FTL bug: find unsealed page lun=%d blk=%d page=%d\n", lun, blk, page);
				return ERR;
			}
		}

		/* read last page per-block */
		if (0 == page) {
			page = dev->flash->npage - 1;
			goto last_page;
		}
	}

#if 0
{
	int lb;

	printf("BBT | empty map\n");
	for_dev_each_block(dev, lb)
		printf("blk=%04d: %016lX\n", lb, dev->sb[lb].sb_luninfo.sb_bbt[0]);
	printf("\n");
}
#endif

	if (active > 2) {
		printf("ERR: detect more than 2 active block %d\n", active);
		return ERR;
	} else {
		printf("active_block is: ");
		for (i = 0; i < active; i++)
			printf("%d ", dev->active_block[i]);
		printf("\n");
	}

	/* re-calculate invalid blocks per-strip */
	for_dev_each_block(dev, blk) {
		valid_blks = 0;

		for_dev_each_lun(dev, lun) {
			if (!test_bit(lun, dev->sb[blk].sb_luninfo.sb_bbt))
				valid_blks++;
		}
		dev->sb[blk].sb_luninfo.luns = valid_blks;
	}

	return 0;
}

#define	get_data_buf()	(data + page * superpage_size +					\
			lun * (dev->config->chunk_ndata + dev->config->chunk_nmeta) +	\
			plane * (dev->config->ndata + dev->config->nmeta))		\

static void shannon_rmw_fake_ecc_usage(void)
{
	printf("Description:\n");
	printf("\tRead, modify error bits then erase/write flash to fake error bits\n\n");

	printf("Usage:\n");
	printf("\trmw-fake-ecc [option] begin_chunkblock count\n\n");

	printf("Option:\n");
	printf("\t-e, --seed=SEED\n"
		"\t\tSeed for random location of error bit. Default is gettimeofday.tv_usec\n\n");
	printf("\t-F, --fixed-ecc-errbits=num\n"
		"\t\tSet per-codeword has fixed number of ecc errbits\n\n");
	printf("\t-R, --fixed-ecc-ratio=ratio\n"
		"\t\tSet ratio of fixed number ecc errbits as 1/ratio. Default 1 if no this option, only valid when -F/--fixed-ecc-errbits\n\n");
	printf("\t-M, --max-ecc-errbits=num\n"
		"\t\tWill generate rand number of ecc errbits for per-codeword, this option specify max value\n\n");
	printf("\t-p, --page-range=num\n"
		"\t\tOnly fake the first num pages in the block\n\n");
	printf("\t-a, --enable-fd\n"
		"\t\tEnable fake ecc=FD\n\n");
	printf("\t-o,--overflow-ecc-bits=num\n"
		"\t\tThe randge of overflow ecc bits, default 32 means ecc=0xFD may have errbits [tmode + 1, tmode + 32]\n\n");
	printf("\t-n, --fd-num=num\n"
		"\t\tThe number of ecc=FD per-pagestrip, default is a rand integer between 0 and sector number per-chunk\n\n");
	printf("\t-x, --fd-fixed-lun=lun\n"
		"\t\tFix ecc=FD just on this lun\n\n");
	printf("\t-r, --fd-ratio\n"
		"\t\tThe ratio of fd based on pagestrip if enable-fd is 1/ratio, default is 1\n\n");
	printf("\t-d, --disable-same-lun\n"
		"\t\tDefault we will fake ecc=FD in the same lun randomly, the option can disable this function\n\n");
	printf("\t-N, --noprogress\n"
		"\t\tDisable show progress\n\n");
	printf("\t-t, --absent-luns=loglun|phylun:x1-x2,x3,x4-x5...\n"
		"\t\tSpecify absent luns\n\n");
	printf("\t-T, --present-luns=loglun|phylun:x1-x2,x3,x4-x5...\n"
		"\t\tSpecify present luns\n\n");
	printf("\t-h, --help\n"
		"\t\tDisplay this help and exit\n\n");

	printf("NOTE:\n\tBlock index is count by chunk\n");
}

int shannon_rmw_fake_ecc(struct shannon_dev *dev, int argc, char **argv)
{
	struct option longopts [] = {
		{"seed", required_argument, NULL, 'e'},
		{"noprogress", no_argument, NULL, 'N'},
		{"fixed-ecc-errbits", required_argument, NULL, 'F'},
		{"fixed-ecc-ratio", required_argument, NULL, 'R'},
		{"max-ecc-errbits", required_argument, NULL, 'M'},
		{"page-range", required_argument, NULL, 'p'},
		{"enable-fd", no_argument, NULL, 'a'},
		{"overflow-ecc-bits", required_argument, NULL, 'o'},
		{"fd-ratio", required_argument, NULL, 'r'},
		{"fd-num", required_argument, NULL, 'n'},
		{"fd-fixed-lun", required_argument, NULL, 'x'},
		{"disable-same-lun", no_argument, NULL, 'd'},
		{"noprogress", no_argument, NULL, 'N'},
		{"absent-luns", required_argument, NULL, 't'},
		{"present-luns", required_argument, NULL, 'T'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int opt;
	int i, rc = 0;
	int seed;
	int rmw_fix, rmw_fix_ratio, rmw_max;
	int begin_chunkblock, count;
	int tn, oob_size;
	int native_ncodeword, native_codeword_size, native_pz;	// native data page size
	int native_sector_ncodeword, native_page_nsector;
	__u8 *data;
	long superpage_size, superblock_size;
	int lun, blk, plane, page, ppa, head;
	struct shannon_request *chunk_head_req, *req, *tmp;
	struct list_head req_head;
	int noprogress, pre_cent, now_cent;
	int pagecnt;
	int disable_same_lun, target_lun, fd_num, full_fd, sector_off, cw_off, *uniq_buffer;
	int lun_cw_size, lun_sector_size;
	int enable_fd, fd_ratio, fd_num_flag;
	unsigned char *lun_cw_map;
	int cur_errbits, overbits;
	int fd_fixed_lun;

	seed = getseed(0);
	noprogress = 0;
	rmw_fix = rmw_fix_ratio = rmw_max = -1;
	init_nrand(dev->config->codeword_size * 8, seed);
	pagecnt = dev->flash->npage;
	disable_same_lun = 0;
	full_fd = 0;
	enable_fd = 0;
	fd_ratio = 1;
	fd_num_flag = 0;
	overbits = 32;
	fd_fixed_lun = -1;

	while ((opt = getopt_long(argc, argv, ":e:F:R:M:p:ao:r:n:x:dNt:T:h", longopts, NULL)) != -1) {
		switch (opt) {
		case 'e':
			seed = strtoul(optarg, NULL, 10);
			break;
		case 'F':
			if (rmw_max != -1) {
				printf("Don`t set option 'max-ecc-errbits' and 'fixed-ecc-errbits' at the same time\n");
				return ERR;
			}
			rmw_fix = strtoul(optarg, NULL, 10);
			if (rmw_fix < 0 || rmw_fix > dev->tmode) {
				printf("This value should be [0, %d]\n", dev->tmode);
				return ERR;
			}
			break;
		case 'R':
			rmw_fix_ratio = strtoul(optarg, NULL, 10);
			if (rmw_fix_ratio <= 0) {
				printf("This value should > 0\n");
				return ERR;
			}
			break;
		case 'M':
			if (rmw_fix != -1) {
				printf("Don`t set option 'max-ecc-errbits' and 'fixed-ecc-errbits' at the same time\n");
				return ERR;
			}
			rmw_max = strtoul(optarg, NULL, 10);
			if (rmw_max < 0 || rmw_max > dev->tmode) {
				printf("This value should be [0, %d]\n", dev->tmode);
				return ERR;
			}
			break;
		case 'p':
			pagecnt = strtoul(optarg, NULL, 10);
			if (pagecnt < 1 || pagecnt > dev->flash->npage) {
				printf("Invalid page select\n");
				return ERR;
			}
			break;
		case 'a':
			enable_fd = 1;
			break;
		case 'o':
			overbits = strtoul(optarg, NULL, 10);
			if (overbits < 0 || overbits > 1024) {
				printf("Invalid option value for 'overflow-ecc-bits'\n");
				return ERR;
			}
			break;
		case 'r':
			fd_ratio = strtoul(optarg, NULL, 10);
			break;
		case 'n':
			fd_num = strtoul(optarg, NULL, 10);
			fd_num_flag = 1;
			break;
		case 'x':
			fd_fixed_lun = strtoul(optarg, NULL, 10);
			if (fd_fixed_lun < 0 || fd_fixed_lun > dev->config->luns) {
				printf("No such lun\n");
				return ERR;
			}
			break;
		case 'd':
			disable_same_lun = 1;
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
		case 'h':
			shannon_rmw_fake_ecc_usage();
			return 0;
		default:
			shannon_rmw_fake_ecc_usage();
			return ERR;
		}
	}

	if (2 != (argc - optind)) {
		shannon_rmw_fake_ecc_usage();
		return ERR;
	}

	begin_chunkblock = strtoul(argv[optind], NULL, 10);
	count = strtoul(argv[optind + 1], NULL, 10);
	if ((begin_chunkblock + count) > dev->flash->nblk / dev->config->nplane) {
		printf("Overflow address\n");
		return ERR;
	}

	if (-1 == rmw_fix && -1 == rmw_max) {
		printf("Please set option [-F, fixed-ecc-errbits] or [-M, max-ecc-errbits]\n");
		return ERR;
	}

	if (enable_fd) {
		if (prepare_skip_empty_block(dev)) {
			printf("prepare skip empty block failed\n");
			exit(EXIT_FAILURE);
		}
	}

	/* calculate variables when bypass ecc */
	native_ncodeword = dev->config->page_nsector * dev->config->sector_ncodeword;
	native_codeword_size = dev->config->codeword_size;
	native_pz = native_ncodeword * native_codeword_size;

	native_page_nsector = dev->config->page_nsector;
	native_sector_ncodeword = dev->config->sector_ncodeword;

	uniq_buffer = malloc(dev->config->chunk_nsector * sizeof(int));
	lun_sector_size = dev->config->luns * dev->config->chunk_nsector;
	lun_cw_size = lun_sector_size * dev->config->sector_ncodeword;
	lun_cw_map = malloc(lun_cw_size);
	if (NULL == uniq_buffer || NULL == lun_cw_map) {
		printf("malloc failed\n");
		exit(EXIT_FAILURE);
	}

	dev->config->ecc_mode = ECCMODE_DISABLE;
	dev->config->raid_mode = RAIDMODE_DISABLE;
	calculate_indirect_config(dev);

	tn = (native_pz + dev->config->sector_ncodeword * dev->config->codeword_size - 1) /
			(dev->config->sector_ncodeword * dev->config->codeword_size);

	oob_size = tn * dev->config->sector_ncodeword * dev->config->codeword_size - dev->flash->page_size;
	if (oob_size > 0)
		dev->flash->oob_size = oob_size;
	dev->flash->entire_page_size = dev->flash->page_size + dev->flash->oob_size;
	// dev->config->sector_ncodeword = native_page_nsector;
	if (re_init_device(dev)) {
		printf("%s() reinit device failed\n", __func__);
		return ERR;
	}

	superpage_size = dev->config->luns * (dev->config->chunk_ndata + dev->config->chunk_nmeta);
	superblock_size = dev->flash->npage * superpage_size;
	data = malloc(superblock_size);
	if (NULL == data) {
		printf("malloc buffer for superblock failed\n");
		rc = ALLOCMEM_FAILED;
		return ERR;
	}

	srand(seed);

	/* bypass ecc read superblock then modify error bits */
	if (!noprogress) {
		pre_cent = now_cent = 0;
		print("Rmw-fake-ecc in process...%%%02d", now_cent);
	}

	head = INDEP_HEAD;
	if (dev->config->nplane > 1)
		head |= (1 << SH_READ_PLANE_SHIFT);
	INIT_LIST_HEAD(&req_head);
	blk = begin_chunkblock;

next_block:
	if (is_bad_superblock(dev, blk))
		goto skip_bad_sb;

	ppa = blk * dev->flash->npage * dev->config->nplane;

	for (page = 0; page < pagecnt; page++) {
		memset(lun_cw_map, 0xFF, lun_cw_size);		// 0xFF means bad lun-block

		for_dev_each_lun(dev, lun) {
			if (is_bad_lunblock(dev, lun, blk))
				continue;

			chunk_head_req = alloc_request(dev, sh_preread_cmd, lun, ppa + page, head, 0, 0);
			if (NULL == chunk_head_req) {
				rc = ALLOCMEM_FAILED;
				goto free_req_out;
			}
			list_add_tail(&chunk_head_req->list, &req_head);

			for (plane = 1; plane < dev->config->nplane; plane++) {
				req = alloc_request(dev, sh_preread_cmd, lun, ppa + page + plane * dev->flash->npage, head, 0, 0);
				if (NULL == req) {
					rc = ALLOCMEM_FAILED;
					goto free_req_out;
				}
				list_add_tail(&req->chunk_list, &chunk_head_req->chunk_list);
			}

			for (plane = 0; plane < dev->config->nplane; plane++) {
				int ns, bs = 0;
				int remain_ns = dev->config->page_nsector;

				while (remain_ns) {
					ns = (remain_ns >= 8) ? 8 : remain_ns;

					req = alloc_request(dev, sh_cacheread_cmd, lun, ppa + page + plane * dev->flash->npage, head, bs, 0);
					if (NULL == req) {
						rc = ALLOCMEM_FAILED;
						goto free_req_out;
					}
					req->bfree = 0;
					req->rw_entire_buffer = 1;
					req->nsector = ns;
					req->data = get_data_buf() + bs * (dev->config->sector_ncodeword * dev->config->codeword_size);
					list_add_tail(&req->list, &req_head);

					bs += ns;
					remain_ns -= ns;
				}
			}
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

		list_for_each_entry_safe(req, tmp, &req_head, list) {
			list_del(&req->list);
			free_request(req);
		}

		/* modify ecc bits */
		if (enable_fd && (0 == rand() % fd_ratio)) {
			if (fd_num_flag) {
				if (fd_num < 0 || fd_num > native_page_nsector * dev->config->nplane)	// fix fd_num per-pagestrip
					fd_num = native_page_nsector * dev->config->nplane;
			} else {
				fd_num = rand() % (native_page_nsector * dev->config->nplane) + 1;	// ecc=0xFD per-pagestrip
			}

			uniq_rand(native_page_nsector * dev->config->nplane, 0, uniq_buffer);
			if (!disable_same_lun) {	// that is enable same lun
				if (0 == rand() % 32) {
					target_lun = rand() % dev->config->luns;
					if (full_fd++ == 3) {
						full_fd = 0;
						fd_num = native_page_nsector * dev->config->nplane;
					}
				} else {
					target_lun = -1;
				}
			}

			for (i = 0; i < fd_num; i++) {
				if (disable_same_lun)	// that is disable same lun
					lun = rand() % dev->config->luns;
				else
					lun = (target_lun == -1) ? (rand() % dev->config->luns) : target_lun;

				if (-1 != fd_fixed_lun)
					lun = fd_fixed_lun;

				if (is_bad_lunblock(dev, lun, blk))
					continue;

				plane = uniq_buffer[i] / native_page_nsector;
				sector_off = uniq_buffer[i] % native_page_nsector;
				cw_off = rand() % native_sector_ncodeword;

				__modify_error_bit(get_data_buf() + (sector_off * native_sector_ncodeword + cw_off) * native_codeword_size, (cur_errbits = ((rand() % overbits + 1) + dev->tmode)));
				lun_cw_map[lun * native_ncodeword * dev->config->nplane + plane * native_ncodeword + sector_off * native_sector_ncodeword + cw_off] = cur_errbits;
			}
		}

		for_dev_each_lun(dev, lun) {
			for (plane = 0; plane < dev->config->nplane; plane++) {
				for (i = 0; i < native_ncodeword; i++) {
					cur_errbits = 0;

					if (is_bad_lunblock(dev, lun, blk))
						continue;

					if (enable_fd) {
						if (0xFF != lun_cw_map[lun * native_ncodeword * dev->config->nplane + plane * native_ncodeword + i])
							continue;
					}

					if (-1 != rmw_fix) {
						if (-1 != rmw_fix_ratio) {
							if (0 == rand() % rmw_fix_ratio)
								__modify_error_bit(get_data_buf() + i * native_codeword_size, (cur_errbits = rmw_fix));
						} else {
							__modify_error_bit(get_data_buf() + i * native_codeword_size, (cur_errbits = rmw_fix));
						}
					} else {
						__modify_error_bit(get_data_buf() + i * native_codeword_size, (cur_errbits = (rand() % (rmw_max + 1))));
					}

					lun_cw_map[lun * native_ncodeword * dev->config->nplane + plane * native_ncodeword + i] = cur_errbits;
				}
			}
		}

		now_cent = 100 * (2 * (blk - begin_chunkblock) * dev->flash->npage +
			page + 1) / (count * dev->flash->npage * 2);
		if (now_cent > pre_cent) {
			print("\b\b%02d", now_cent);
			pre_cent = now_cent;
		}
	} /* end (page = 0; page < pagecnt; page++) */

	/* erase superblock */
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

	list_for_each_entry_safe(req, tmp, &req_head, list) {
		list_del(&req->list);
		free_request(req);
	}

	/* bypass ecc write superblock */
	for (page = 0; page < pagecnt; page++) {
		for_dev_each_lun(dev, lun) {
			if (is_bad_lunblock(dev, lun, blk))
				continue;

			chunk_head_req = alloc_request(dev, sh_write_cmd, lun, ppa + page, head, 0, 0);
			if (NULL == chunk_head_req) {
				rc = ALLOCMEM_FAILED;
				goto free_req_out;
			}
			plane = 0;	// for get_data_buf()
			chunk_head_req->bfree = 0;
			chunk_head_req->rw_entire_buffer = 1;
			chunk_head_req->nsector = dev->config->page_nsector;
			chunk_head_req->data = get_data_buf();
			list_add_tail(&chunk_head_req->list, &req_head);

			for (plane = 1; plane < dev->config->nplane; plane++) {
				req = alloc_request(dev, sh_write_cmd, lun, ppa + page + plane * dev->flash->npage, head, 0, 0);
				if (NULL == req) {
					rc = ALLOCMEM_FAILED;
					goto free_req_out;
				}
				req->bfree = 0;
				req->rw_entire_buffer = 1;
				req->nsector = dev->config->page_nsector;
				req->data = get_data_buf();
				list_add_tail(&req->chunk_list, &chunk_head_req->chunk_list);
			}
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

		list_for_each_entry_safe(req, tmp, &req_head, list) {
			list_del(&req->list);
			free_request(req);
		}

		now_cent = 100 * (2 * (blk - begin_chunkblock) * dev->flash->npage + dev->flash->npage +
			page + 1) / (count * dev->flash->npage * 2);
		if (now_cent > pre_cent) {
			print("\b\b%02d", now_cent);
			pre_cent = now_cent;
		}
	}

skip_bad_sb:
	blk++;
	if (blk < (begin_chunkblock + count))
		goto next_block;

	if (!noprogress)
		printf("\n");

	/* exit */
free_req_out:
	list_for_each_entry_safe(req, tmp, &req_head, list) {
		list_del(&req->list);
		free_request(req);
	}
	free(data);
	exit_nrand();
	if (rc)
		printf("%s() encounter some error\n", __func__);
	return rc;
}

/*-----------------------------------------------------------------------------------------------------------*/
