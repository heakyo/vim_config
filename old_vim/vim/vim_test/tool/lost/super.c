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

// #define	SUPER_DEBUG

/*-----------------------------------------------------------------------------------------------------------*/
/*
 * erase super chunk block
 */
static void shannon_super_erase_usage(void)
{
	printf("Usage:\n");
	printf("\tsuper-erase [option] begin_chunkblock count\n\n");

	printf("Option:\n");
	printf("\t-N, --noprogress\n"
		"\t\tdisable show progress\n\n");
	printf("\t-h, --help\n"
		"\t\tdisplay this help and exit\n\n");

	printf("NOTE:\n\tBlock index is count by chunk\n");
}

int shannon_super_erase(struct shannon_dev *dev, int argc, char **argv)
{
	struct option longopts [] = {
		{"noprogress", no_argument, NULL, 'N'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int opt;
	int lun, begin_chunkblock, count;
	int head, noprogress;
	int blk, plane, ppa;
	int rc = 0;
	struct shannon_request *chunk_head_req, *req, *tmp;
	struct list_head req_head;
	int pre_cent, now_cent;		// used for show progress

	/* analyse argument */
	head = INDEP_HEAD;
	noprogress = 0;

	while ((opt = getopt_long(argc, argv, ":Nh", longopts, NULL)) != -1) {
		switch (opt) {
		case 'N':
			noprogress = 1;
			break;
		case 'h':
			shannon_super_erase_usage();
			return 0;
		default:
			shannon_super_erase_usage();
			return ERR;		
		}
	}

	if ((argc - optind) != 2) {
		shannon_super_erase_usage();
		return ERR;
	}

	begin_chunkblock = strtoul(argv[optind], NULL, 10);
	count = strtoul(argv[optind + 1], NULL, 10);

	if ((begin_chunkblock + count) > dev->flash->nblk/dev->config->nplane) {
		printf("Overflow address\n");
		return OVERFLOW_ADDR;
	}

	/* manipulate erase chunk block process */
	if (dev->config->nplane > 1)
		head |= (1 << SH_ERASE_PLANE_SHIFT);

	if (!noprogress) {
		pre_cent = now_cent = 0;
		print("In process...%%%02d", now_cent);
	}

	INIT_LIST_HEAD(&req_head);

	for (blk = begin_chunkblock; blk < begin_chunkblock + count; blk++) {
		ppa = blk * dev->config->nplane * dev->flash->npage;

		/* alloc erase request */
		for_dev_each_lun(dev, lun) {
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
		list_for_each_entry(req, &req_head, list)
			rc |= check_chunk_status(dev, req, sh_erase_cmd);

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
	printf("Usage:\n");
	printf("\tsuper-write [option] begin_chunkblock count\n\n");

	printf("Option:\n");
	printf("\t-e, --seed=SEED\n"
		"\t\tspecify rand seed of write data, or use time(NULL) as default seed and print it\n\n");
	printf("\t-H, --head=HEAD\n"
		"\t\tselect head, default is head 2\n\n");
	printf("\t-N, --noprogress\n"
		"\t\tdisable show progress\n\n");
	printf("\t-h, --help\n"
		"\t\tdisplay this help and exit\n\n");

	printf("NOTE:\n\tBlock index is count by chunk\n");
}

#define	PLUN		(dev->config->nlun - 1)	//FIXME: don`t use fixed parity lun
#define	NDATALUN	(dev->config->nlun - 1)

int shannon_super_write(struct shannon_dev *dev, int argc, char **argv)
{
	struct option longopts [] = {
		{"seed", required_argument, NULL, 'e'},
		{"head", required_argument, NULL, 'H'},
		{"noprogress", no_argument, NULL, 'N'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int i, rc = 0;
	int opt;
	int seed, head, seed_way, noprogress;
	int blk, plane, ppa, page;
	int ihead, lun, begin_chunkblock, count;
	struct shannon_request *chunk_head_req, *req, *tmp;
	struct list_head req_head;
	int pre_cent, now_cent;		// used for show progress

	/* analyse argument */
	seed = time(NULL);
	head = INDEP_HEAD;
	seed_way = 0;
	noprogress = 0;

	while ((opt = getopt_long(argc, argv, ":e:H:Nh", longopts, NULL)) != -1) {
		switch (opt) {
		case 'e':
			seed = strtoul(optarg, NULL, 10);
			seed_way = 1;
			break;
		case 'H':
			head = strtoul(optarg, NULL, 10);
			break;
		case 'N':
			noprogress = 1;
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

	begin_chunkblock = strtoul(argv[optind], NULL, 10);
	count = strtoul(argv[optind + 1], NULL, 10);

	if ((begin_chunkblock + count) > dev->flash->nblk/dev->config->nplane) {
		printf("Overflow address\n");
		return OVERFLOW_ADDR;
	}

	if (!seed_way)
		print("seed=%d\n", seed);

	/* manipulate write chunk process */
	if (dev->config->nplane > 1)
		head |= (1 << SH_WRITE_PLANE_SHIFT);

	if (!noprogress) {
		pre_cent = now_cent = 0;
		print("In process...%%%02d", now_cent);
	}

	INIT_LIST_HEAD(&req_head);

	for (blk = begin_chunkblock; blk < begin_chunkblock + count; blk++) {
		ppa = blk * dev->config->nplane * dev->flash->npage;
		page = 0;

		/* raid init before per-block if needed */
		if (dev->config->raid_mode && (head & HEAD_MASK) != INDEP_HEAD) {
			req = alloc_request(dev, sh_raidinit_cmd, PLUN, ppa, head, 0, NDATALUN);
			if (NULL == req)  {
				rc = ALLOCMEM_FAILED;
				goto free_req_out;
			}
			list_add_tail(&req->list, &req_head);
		}

next_block_page:
		/* write chunk */
		for_dev_each_lun(dev, lun) {
			if (dev->config->raid_mode && (head & HEAD_MASK) != INDEP_HEAD && PLUN == lun)		// raid lun
				continue;

			for (plane = 0; plane < dev->config->nplane; plane++) {
				for (i = 0; i < dev->config->page_nsector; i++) {
					req = alloc_request(dev, sh_bufwrite_cmd, lun, ppa + plane * dev->flash->npage + page, head, i + plane * dev->config->page_nsector, 1);
					if (NULL == req) {
						rc = ALLOCMEM_FAILED;
						goto free_req_out;
					}
					list_add_tail(&req->list, &req_head);
#ifdef SUPER_DEBUG 
					pad_same_u32(req->data, dev->config->sector_size, ((ppa + plane * dev->flash->npage + page) * dev->config->page_nsector + i) + ((lun + seed)<<24));
					pad_same_u32(req->metadata, METADATA_SIZE, ((ppa + plane * dev->flash->npage + page) * dev->config->page_nsector + i) + ((lun + seed)<<24));
					printf("lun=%d block=%d page=%d sector=%d plane=%d\n", lun, blk, page, i, plane);
					pr_u32_array_noprefix(req->data, 8, 8);
					pr_u64_array_noprefix(req->metadata, 1, 1);
#else
					srand(((ppa + plane * dev->flash->npage + page) * dev->config->page_nsector + i) + ((lun + seed)<<24));
					pad_rand(req->data, dev->config->sector_size);
					pad_rand(req->metadata, METADATA_SIZE);
#endif
				}
			}
		}

		/* raid write request */
		if (dev->config->raid_mode && (head & HEAD_MASK) != INDEP_HEAD) {
			chunk_head_req = alloc_request(dev, sh_raidwrite_cmd, PLUN, ppa + page, head, 0, 0);
			if (NULL == chunk_head_req) {
				rc = ALLOCMEM_FAILED;
				goto free_req_out;
			}
			list_add_tail(&chunk_head_req->list, &req_head);

			for (plane = 1; plane < dev->config->nplane; plane++) {
				req = alloc_request(dev, sh_raidwrite_cmd, PLUN, ppa + plane * dev->flash->npage + page, head, 0, 0);
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
		for_dev_each_bufhead(dev, ihead)
			update_bufcmdqueue(dev, ihead);

		for_dev_each_lun(dev, lun) {
			if ((rc = poll_cmdqueue(dev, lun)))
				goto free_req_out;
		}
		for_dev_each_bufhead(dev, ihead) {
			if ((rc = poll_bufcmdqueue(dev, ihead)))
				goto free_req_out;
		}

		/* check status */

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

		if (++page < dev->flash->npage)
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
	printf("Usage:\n");
	printf("\tsuper-read [option] begin_chunkblock count\n\n");

	printf("Option:\n");
	printf("\t-e, --seed=SEED\n"
		"\t\tset this option means check data consistence and ecc result, otherwise only check ecc result\n\n");
	printf("\t-N, --noprogress\n"
		"\t\tdisable show progress\n\n");
	printf("\t-h, --help\n"
		"\t\tdisplay this help and exit\n\n");

	printf("NOTE:\n\tBlock index is count by chunk\n");
}

int shannon_super_read(struct shannon_dev *dev, int argc, char **argv)
{
	struct option longopts [] = {
		{"seed", required_argument, NULL, 'e'},
		{"noprogress", no_argument, NULL, 'N'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int i, rc = 0;
	int opt;
	int seed, check_data, head, noprogress;
	int blk, plane, ppa, page;
	int lun, begin_chunkblock, count;
	struct shannon_request *chunk_head_req, *req, *tmp;
	struct list_head req_head;
	int pre_cent, now_cent;		// used for show progress

	/* analyse argument */
	head = 0;			// read just have multi or single plane, no head
	noprogress = 0;
	seed = -1;
	check_data = 0;

	while ((opt = getopt_long(argc, argv, ":e:Nh", longopts, NULL)) != -1) {
		switch (opt) {
		case 'e':
			seed = strtoul(optarg, NULL, 10);
			check_data = 1;
			break;
		case 'N':
			noprogress = 1;
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

	begin_chunkblock = strtoul(argv[optind], NULL, 10);
	count = strtoul(argv[optind + 1], NULL, 10);

	if ((begin_chunkblock + count) > dev->flash->nblk/dev->config->nplane) {
		printf("Overflow address\n");
		return OVERFLOW_ADDR;
	}

	/* manipulate read chunk block and check process */
	if (dev->config->nplane > 1)
		head |= (1 << SH_READ_PLANE_SHIFT);

	if (!noprogress) {
		pre_cent = now_cent = 0;
		print("In process...%%%02d", now_cent);
	}

	INIT_LIST_HEAD(&req_head);

	for (blk = begin_chunkblock; blk < begin_chunkblock + count; blk++) {
		ppa = blk * dev->config->nplane * dev->flash->npage;
		page = 0;

next_block_page:
		/* read chunk */
		for_dev_each_lun(dev, lun) {
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

					req = alloc_request(dev, sh_cacheread_cmd, lun, ppa + plane * dev->flash->npage + page, head, bs, ns);
					if (NULL == req) {
						rc = ALLOCMEM_FAILED;
						goto free_req_out;
					}
					list_add_tail(&req->list, &req_head);

					bs += ns;
					remain_ns -= ns;
				}
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

		/* check status and ecc */
		list_for_each_entry(req, &req_head, list) {
			if (sh_preread_cmd == req->opcode) 
				check_chunk_status(dev, req, sh_preread_cmd);

			if (sh_cacheread_cmd == req->opcode) {
				for (i = 0; i < req->nsector; i++) {
					if (req->ecc[i] < 251)
						continue;

					if (req->ecc[i] == 251)
						printf("Super-read blank %02X. ", req->ecc[i]);
					else if (req->ecc[i] > 251)
						printf("Super-read ecc failed: %02X. ", req->ecc[i]);

						printf("lun=%d block=%d page=%d sector=%d\n",
							req->lun, req->ppa / (dev->flash->npage * dev->config->nplane), req->ppa % dev->flash->npage, req->bsector + i);
				}
			}
		}

		/* check data consistence if needed */
		if (!check_data)
			goto skip_check_data;

		list_for_each_entry(req, &req_head, list) {
			if (sh_cacheread_cmd != req->opcode || PLUN == req->lun)
				continue;

			for (i = 0; i < req->nsector; i++) {
#ifdef SUPER_DEBUG
				printf("lun=%d block=%d page=%d sector=%d plane=%d\n",
						req->lun,
						req->ppa/(dev->flash->npage * dev->config->nplane),
						req->ppa%dev->flash->npage,
						req->bsector + i,
						(req->ppa%(dev->flash->npage * dev->config->nplane))/dev->flash->npage);
				pr_u32_array_noprefix(req->data + i * dev->config->sector_size, 8, 8);
				pr_u64_array_noprefix(req->metadata + i, 1, 1);
#else
				__u8 ch;
				int ibx, jump = 0;

				srand((req->ppa * dev->config->page_nsector + req->bsector + i) + ((req->lun + seed)<<24));

				for (ibx = 0; ibx < dev->config->sector_size; ibx++) {	// data
					if ((ch = rand()) != req->data[i * dev->config->sector_size + ibx]) {
						printf("Data mismatch: lun=%d block=%d page=%d sector=%d\n",
							req->lun, req->ppa/(dev->flash->npage * dev->config->nplane), req->ppa%dev->flash->npage, i);
						jump = 1;
						break;
					}
				}

				if (jump)						// ignore comparing metadata if data mismatch
					continue;

				for (ibx = 0; ibx < METADATA_SIZE; ibx++) {		// metadata
					if ((ch = rand()) != ((__u8 *)req->metadata)[i * METADATA_SIZE + ibx]) {
						printf("Metaata mismatch: lun=%d block=%d page=%d sector=%d\n",
							req->lun, req->ppa/(dev->flash->npage * dev->config->nplane), req->ppa%dev->flash->npage, i);
						break;
					}
				}
#endif
			}
		}

		/* free request */
skip_check_data:
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

		if (++page < dev->flash->npage)
			goto next_block_page;
	}

	if (!noprogress)
		printf("\n");

	rc = 0;
free_req_out:
	list_for_each_entry_safe(req, tmp, &req_head, list) {
		list_del(&req->list);
		free_request(req);
	}

	if (rc)
		printf("Super-read encounter some error\n");
	return rc;
}
/*-----------------------------------------------------------------------------------------------------------*/
