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
#include <ctype.h>
#include <sys/time.h>

#include "tool.h"

#define	MBR_ID	"sh-shannon-pcie-ssd"

#define	MPT_MBR_NBLK		4
#define	MPT_MBR_SIZE		512
#define	MPT_MBR_PAGEOFF		0
#define	MPT_BBT_PAGEOFF		1
#define	MPT_DBBT_PAGEOFF	2
#define MAX_BAD_BLOCK_IN_A_LUN  (dev->flash->nblk/10)

#define	MPT_HELP		" Please contact david <david@mail.shannon-data.com>\n"

static u64* ecc_histogram;

static struct shannon_bbt *used_bbt = NULL;
static struct shannon_bbt *new_bbt = NULL;

/*-----------------------------------------------------------------------------------------------------------*/
static void check_mbr_bbt(struct shannon_dev *dev, struct shannon_bbt *mbr_bbt, char *note)
{
	int lun, blk, n_invalid;

	assert(dev->config->nplane == 1);

	for_dev_each_lun(dev, lun) {
		n_invalid = 0;

		for (blk = 0; blk < MPT_MBR_NBLK; blk++) {
			if (test_bit(lun, mbr_bbt->sb_bbt[blk]))
				n_invalid++;
		}

		if (n_invalid > MPT_MBR_NBLK/2) {
			printf("%s(): lun-%03d phylun-%03d has %d bad MBR blocks, fence this lun\n",
				__func__, lun, log2phy_lun(dev, lun), n_invalid);
			set_bit(lun, dev->lun_bitmap);
			dev->valid_luns--;
		}
	}

	if (!dev->valid_luns)
		exitlog("ERR: all luns are invalid!!!\n");

	if ((dev->present_luns - dev->valid_luns) > dev->check_bad_luns)
		exitlog("%s ERR: Found %d bad luns, threshold is %d\n",
			note, dev->present_luns - dev->valid_luns, dev->check_bad_luns);
}

static void check_all_bbt(struct shannon_dev *dev, struct shannon_bbt *bbt, char *note)
{
	int lun, blk, n_invalid;

	assert(dev->config->nplane == 1);
	assert(bbt->nblock == dev->flash->nblk);

	for_dev_each_lun(dev, lun) {
		n_invalid = 0;

		for (blk = 0; blk < bbt->nblock; blk++) {
			if (test_bit(lun, bbt->sb_bbt[blk]))
				n_invalid++;
		}

		if (n_invalid > MAX_BAD_BLOCK_IN_A_LUN) {
			printf("%s(): lun-%03d phylun-%03d has %d bad blocks, fence this lun\n",
				__func__, lun, log2phy_lun(dev, lun), n_invalid);
			set_bit(lun, dev->lun_bitmap);
			dev->valid_luns--;
		}
	}

	if (!dev->valid_luns)
		exitlog("ERR: all luns are invalid!!!\n");

	if ((dev->present_luns - dev->valid_luns) > dev->check_bad_luns)
		exitlog("%s ERR: Found %d bad luns, threshold is %d\n",
			note, dev->present_luns - dev->valid_luns, dev->check_bad_luns);
}

/*-----------------------------------------------------------------------------------------------------------*/
/*
 * Erase and flagbyte scan MBR blocks
 */
static void mpt_scan_mbr_bbt_basic(struct shannon_dev *dev, struct shannon_bbt *mbr_bbt, int new)
{
	int i;

	/* re-init device first */
	dev->config->sector_size_shift = 12;
	dev->config->sector_ncodeword = 1;
	dev->config->ecc_mode = ECCMODE_DISABLE;
	dev->config->raid_mode = RAIDMODE_DISABLE;
	dev->config->nplane = 1;
	if (re_init_device(dev)) {
		printf("Re-init-device %s() failed."MPT_HELP, __func__);
		exit(EXIT_FAILURE);
	}
	dev->clear_queue(dev);

	/* erase scan mbr-bbt */
	if (!new) {
		print("MBR blocks erase scan...\n");

		memcpy(dev->lun_bitmap_backup, dev->lun_bitmap, sizeof(dev->lun_bitmap_backup));
		memset(dev->lun_bitmap, 0x00, sizeof(dev->lun_bitmap));
		if (erase_scan(dev, mbr_bbt))
			exitlog("erase_scan() failed\n");
		for (i = 0; i < ARRAY_SIZE(dev->lun_bitmap); i++)
			dev->lun_bitmap[i] |= dev->lun_bitmap_backup[i];

		check_mbr_bbt(dev, mbr_bbt, "MBR blocks erase check bad luns");
	}

	/* flagbyte scan mbr-bbt */
	print("MBR blocks flagbyte scan...\n");

	if (flagbyte_scan(dev, mbr_bbt))
		exitlog("flagbyte_scan() failed\n");
	check_mbr_bbt(dev, mbr_bbt, "MBR blocks flagbyte check bad luns");
}

/* DWORD 7 throttle */
#define SH_HALTREAD_SHIFT	16
#define SH_HALTREAD_MASK	0x00ff0000

/* DWORD 0x3d External temperature sensor */
#define SH_TEMP_AUX1_OFFSET		0x3D
#define SH_TEMP_AUX2_OFFSET		0x3E
#define	SH_TEMP_BOARD_OFFSET		0x3F
#define SH_EXTERNAL_TEMP_DIV		32.0

static float get_flash_temp(struct shannon_dev *dev) {
	unsigned int code1 = dev->ioread32(dev, SH_TEMP_AUX1_OFFSET);
	unsigned int code2 = dev->ioread32(dev, SH_TEMP_AUX2_OFFSET);
	float temp;

	if ((code1 & 0x2000) || (code2 & 0x2000)) {
		// printf("negative flash temperature: code1=0x%X code2=0x%X\n", code1, code2);
		return 0.0;
	}

	code1 &= 0x1FFF;
	code2 &= 0x1FFF;
	temp = (code1 > code2 ? code1 : code2) / SH_EXTERNAL_TEMP_DIV;

	if ((temp - dev->max_flash_temp) > 0.01)
		dev->max_flash_temp = temp;

	return temp;
}

static float get_board_temp(struct shannon_dev *dev) {
	unsigned int code = dev->ioread32(dev, SH_TEMP_BOARD_OFFSET);
	float temp;

	if (code & 0x2000) {
		// printf("negative board temperature: code=0x%X\n", code);
		return 0.0;
	}
	temp = (code & 0x1FFF) / SH_EXTERNAL_TEMP_DIV;

	if ((temp - dev->max_board_temp) > 0.01)
		dev->max_board_temp = temp;

	return temp;
}

static float get_controller_temp(struct shannon_dev *dev) {
	unsigned int code = dev->ioread32(dev, 0x26);
	float temp = (code & 0x3FF) * 503975.0 / (1024 * 1000.0) - 273;

	if ((code & 0x3FF) == 0x3FF) {
		printf("negative controller temperature: code1=0x%X\n", code);
		return 0.0;
	}

	if ((temp - dev->max_controller_temp) > 0.01)
		dev->max_controller_temp = temp;

	return temp;
}

static void set_max_ecc(struct shannon_dev *dev, int max_ecc) {
	unsigned int code = dev->ioread32(dev, 0xC7);

	code = (code & ~SH_HALTREAD_MASK) | ((max_ecc << SH_HALTREAD_SHIFT) & SH_HALTREAD_MASK);
	dev->iowrite32(dev, code, 0xC7);
}

static void timespan(time_t b, time_t e, char *stt)
{
	long span = (long)(e - b);
	int h = span / 3600;
	int m = (span % 3600) / 60;
	int s = span % 60;
	int n = 0;

	if (h)
		n += sprintf(stt + n, "%dh", h);
	if (m || h)
		n += sprintf(stt + n, "%dm", m);
	sprintf(stt + n, "%ds", s);
}

/*
 * Write then read and compare to scan MBR blocks
 */
#undef ADVANCED_READ_INFO
// #define	ADVANCED_READ_INFO	1
static void mpt_scan_bbt_advance(struct shannon_dev *dev, struct shannon_bbt *bbt)
{
	int lun, blk, ppa, head;
	struct shannon_request *req, *tmp;
	struct list_head req_head, req_head_ar;
	int i, remain_ns, ns, bs;
	float pre_cent = 0, now_cent = 0;
	int nblock = bbt->nblock;
	float flash_temp, ctrl_temp, board_temp;

	/* re-init device first */
	dev->config->sector_size_shift = dev->config_bakup->sector_size_shift;
	dev->config->sector_ncodeword = dev->config_bakup->sector_ncodeword;
	dev->config->ecc_mode = dev->config_bakup->ecc_mode;
	dev->config->raid_mode = RAIDMODE_DISABLE;
	dev->config->nplane = 1;
	assert(dev->config->sector_size_shift == 12 && dev->config->ecc_mode == ECCMODE_ENABLE);
	if (re_init_device(dev)) {
		printf("Re-init-device %s() failed."MPT_HELP, __func__);
		exit(EXIT_FAILURE);
	}
	dev->clear_queue(dev);

	/* bbt from write, read then compare */
	head = INDEP_HEAD;
	INIT_LIST_HEAD(&req_head);
	INIT_LIST_HEAD(&req_head_ar);

	srand(getseed(0));

	print("block with ecc larger than %d will be marked bad:\n", dev->sorting_ecc_limit);
	ctrl_temp = get_controller_temp(dev);
	flash_temp = get_flash_temp(dev);
	board_temp = get_board_temp(dev);
	print("Sorting 0s%s loops %d/%d, bad blocks %d, controller temp %3.2f, flash temp %3.2f, board temp %3.2f, progress 0.00%%",
		dev->sorting_print_string, dev->loops, dev->scan_loops, dev->bad_blocks, ctrl_temp, flash_temp, board_temp);

	for (blk = 0; blk < nblock; blk++) {
		/* write then do normal read */
		set_max_ecc(dev, 240);

		for (ppa = blk * dev->flash->npage; ppa < (blk + 1) * dev->flash->npage; ppa++) {
			for_dev_each_lun(dev, lun) {
				if (test_bit(lun, bbt->sb_bbt[blk]))
					continue;
				req = alloc_request_no_dma(dev, sh_write_cmd, lun, ppa, head, 0, dev->config->page_nsector, 1);
				if (NULL == req)
					malloc_failed_exit();
				list_add_tail(&req->list, &req_head);
			}
		}

		for (ppa = blk * dev->flash->npage; ppa < (blk + 1) * dev->flash->npage; ppa++) {
			for_dev_each_lun(dev, lun) {
				if (test_bit(lun, bbt->sb_bbt[blk]))
					continue;
				req = alloc_request(dev, sh_preread_cmd, lun, ppa, head, 0, 0);
				if (NULL == req)
					malloc_failed_exit();
				list_add_tail(&req->list, &req_head);

				bs = 0;
				remain_ns = dev->config->page_nsector;
				while (remain_ns) {
					ns = (remain_ns >= 8) ? 8 : remain_ns;

					req = alloc_request_no_dma(dev, sh_cacheread_cmd, lun, ppa, head, bs, ns, 1);
					if (NULL == req)
						malloc_failed_exit();
					list_add_tail(&req->list, &req_head);

					bs += ns;
					remain_ns -= ns;
				}
			}
		}

		submit_polling_loop(dev, &req_head);

		list_for_each_entry(req, &req_head, list) {
			if (sh_write_cmd == req->opcode || sh_preread_cmd == req->opcode) {
				if (check_req_status_silent(req) && !test_bit(req->lun, bbt->sb_bbt[blk])) {
					dev->bad_blocks++;
					dev->bb_count[req->lun]++;
					set_bit(req->lun, bbt->sb_bbt[blk]);
					print("Sorting%s loops %d/%d, bad blocks %d: lun %d(%d) blk %d %s failed\n",
						dev->sorting_print_string, dev->loops, dev->scan_loops, dev->bad_blocks, req->lun, dev->bb_count[req->lun], blk,
						(sh_write_cmd == req->opcode) ? "write" : "pre-read");
					if ((dev->bb_count[req->lun] > MAX_BAD_BLOCK_IN_A_LUN) && !test_bit(req->lun, dev->lun_bitmap)) {
						dev->valid_luns--;
						set_bit(req->lun, dev->lun_bitmap);
						print("lun %d has too many bad blocks, marked as invalid\n", req->lun);
					}
				}
			} else if (sh_cacheread_cmd == req->opcode) {
				for (i = 0; i < req->nsector; i++) {
					if ((req->ecc[i] >= 0xFB) && !test_bit(req->lun, bbt->sb_bbt[blk])) {
						dev->bad_blocks++;
						dev->bb_count[req->lun]++;
						set_bit(req->lun, bbt->sb_bbt[blk]);
						print("Sorting%s loops %d/%d, bad blocks %d: lun %d(%d) blk %d page %d normal read ecc is %d\n",
						      dev->sorting_print_string, dev->loops, dev->scan_loops, dev->bad_blocks, req->lun, dev->bb_count[req->lun], blk, req->page, req->ecc[i]);
						if ((dev->bb_count[req->lun] > MAX_BAD_BLOCK_IN_A_LUN) && !test_bit(req->lun, dev->lun_bitmap)) {
							dev->valid_luns--;
							set_bit(req->lun, dev->lun_bitmap);
							print("lun %d has too many bad blocks, marked as invalid\n", req->lun);
						}
					} else if ((req->ecc[i] > dev->sorting_ecc_limit) && !test_bit(req->lun, bbt->sb_bbt[blk])) {
#ifdef ADVANCED_READ_INFO
						print("Enter Advance Read! Sorting%s loops %d/%d: lun %d blk %d page %d high ecc is %d\n",
							dev->sorting_print_string, dev->loops, dev->scan_loops, req->lun, blk, req->ppa % dev->flash->npage, req->ecc[i]);
#endif
						bs = 0;
						remain_ns = dev->config->page_nsector;
						while (remain_ns) {
							ns = (remain_ns >= 8) ? 8 : remain_ns;
							tmp = alloc_request_no_dma(dev, sh_cacheread_cmd, req->lun, req->ppa, head, bs, ns, 1);
							if (NULL == tmp)
								malloc_failed_exit();
							tmp->advance_read = 1;
							list_add_tail(&tmp->list, &req_head_ar);
							bs += ns;
							remain_ns -= ns;
						}

						break;	/* skip checking othen sector ECC in this page */
					 } else
						ecc_histogram[req->ecc[i]]++;
				}
			} else
				exitlog("Unkonwn command %x\n", req->opcode);
		}

		list_for_each_entry_safe(req, tmp, &req_head, list) {
			list_del(&req->list);
			free_request(req);
		}

		/* do advance read if have then do erase */
		set_max_ecc(dev, dev->sorting_ecc_limit);

		ppa = blk * dev->flash->npage;
		for_dev_each_lun(dev, lun) {
			if (test_bit(lun, bbt->sb_bbt[blk]))
				continue;
			req = alloc_request(dev, sh_erase_cmd, lun, ppa, head, 0, 0);
			if (NULL == req)
				malloc_failed_exit();
			list_add_tail(&req->list, &req_head_ar);
		}

		submit_polling_loop(dev, &req_head_ar);

		list_for_each_entry(req, &req_head_ar, list) {
			if (sh_cacheread_cmd == req->opcode) {
#ifdef ADVANCED_READ_INFO
				print("Sorting%s loops %d/%d: lun %d blk %d page %d advanced read ecc are:",
				      dev->sorting_print_string, dev->loops, dev->scan_loops, req->lun, blk, req->ppa % dev->flash->npage);
				for (i = 0; i < req->nsector; i++)
					print(" %d", req->ecc[i]);
				printf("\n");
#endif
				for (i = 0; i < req->nsector; i++) {
					if (req->ecc[i] <= dev->tmode)
						ecc_histogram[req->ecc[i]]++;

					if ((req->ecc[i] > dev->sorting_ecc_limit) && !test_bit(req->lun, bbt->sb_bbt[blk])) {
						dev->bad_blocks++;
						dev->bb_count[req->lun]++;
						set_bit(req->lun, bbt->sb_bbt[blk]);
						print("Sorting%s loops %d/%d, bad blocks %d: lun %d(%d) blk %d advanced read ecc is %d\n",
							dev->sorting_print_string, dev->loops, dev->scan_loops, dev->bad_blocks, req->lun, dev->bb_count[req->lun], blk, req->ecc[i]);
						if ((dev->bb_count[req->lun] > MAX_BAD_BLOCK_IN_A_LUN) && !test_bit(req->lun, dev->lun_bitmap)) {
							dev->valid_luns--;
							set_bit(req->lun, dev->lun_bitmap);
							print("lun %d has too many bad blocks, marked as invalid\n", req->lun);
						}
					}
				}
			} else if (sh_erase_cmd == req->opcode) {
				if (check_req_status_silent(req) && !test_bit(req->lun, bbt->sb_bbt[blk])) {
					dev->bad_blocks++;
					dev->bb_count[req->lun]++;
					set_bit(req->lun, bbt->sb_bbt[blk]);
					print("Sorting%s loops %d/%d, bad blocks %d: lun %d(%d) blk %d erase failed\n",
					      dev->sorting_print_string, dev->loops, dev->scan_loops, dev->bad_blocks, req->lun, dev->bb_count[req->lun], blk);
					if ((dev->bb_count[req->lun] > MAX_BAD_BLOCK_IN_A_LUN) && !test_bit(req->lun, dev->lun_bitmap)) {
						dev->valid_luns--;
						set_bit(req->lun, dev->lun_bitmap);
						print("lun %d has too many bad blocks, marked as invalid\n", req->lun);
					}
				}
			} else
				exitlog("Unkonwn command %x\n", req->opcode);
		}

		list_for_each_entry_safe(req, tmp, &req_head_ar, list) {
			list_del(&req->list);
			free_request(req);
		}

		/* check MBR blocks */
		if (blk == (MPT_MBR_NBLK - 1))
			check_mbr_bbt(dev, bbt, (bbt->nblock == MPT_MBR_NBLK) ? "MBR-LOOP check MBR blocks bad luns" : "INIT-LOOP check MBR blocks bad luns");

		now_cent = 100.0 * (blk + 1) / nblock;
		if ((now_cent - pre_cent) > 0.05) {
			ctrl_temp = get_controller_temp(dev);
			flash_temp = get_flash_temp(dev);
			board_temp = get_board_temp(dev);
			timespan(dev->mpt_begintime, time(NULL), dev->mpt_timetook);
			printf("\r\033[K");
			print("Sorting %s%s loops %d/%d, bad blocks %d, controller temp %3.2f, flash temp %3.2f, board temp %3.2f, progress %2.2f%%",
				dev->mpt_timetook, dev->sorting_print_string, dev->loops, dev->scan_loops, dev->bad_blocks, ctrl_temp, flash_temp, board_temp, now_cent);
			pre_cent = now_cent;
		}
	}

	if (bbt->nblock == dev->flash->nblk)
		check_all_bbt(dev, bbt, "INIT LOOP check bad luns");

	print("\n");

	/* restore value */
	set_max_ecc(dev, 240);
}

static void mpt_scan_mbr_bbt(struct shannon_dev *dev, struct shannon_bbt *mbr_bbt, int new)
{
	mpt_scan_mbr_bbt_basic(dev, mbr_bbt, new);
}

static void mpt_build_sb_luninfo(struct shannon_dev *dev, struct shannon_bbt *bbt)
{
	int lun, blk;
	int group, group_valid_luns, min_data_luns, valid_groups;

	assert(NULL != dev->sb);
	assert(dev->config->nplane == dev->config_bakup->nplane);
	memset(dev->sb, 0x00, sizeof(*dev->sb) * dev->flash->nblk / dev->config_bakup->nplane);

	for_dev_each_block(dev, blk) {
		dev->sb[blk].sb_luninfo.luns = 0;
		dev->sb[blk].sb_luninfo.ndatalun = 0;

		min_data_luns = 65536;
		valid_groups = 0;

		for (group = 0; group < dev->group_raid_num; group++) {
			group_valid_luns = 0;

			for (lun = group * dev->group_raid_luns; lun < (group + 1) * dev->group_raid_luns; lun++) {
				if (test_bit(lun, bbt->sb_bbt[blk]))
					set_bit(lun, dev->sb[blk].sb_luninfo.sb_bbt);
				else
					group_valid_luns++;
			}

			if (group_valid_luns > 1) {
				dev->sb[blk].sb_luninfo.luns += group_valid_luns;
				if ((group_valid_luns - 1) < min_data_luns)
					min_data_luns = group_valid_luns - 1;
				valid_groups++;
			}
		}
		dev->sb[blk].sb_luninfo.ndatalun = min_data_luns * valid_groups;	// FIXME: same luns pre raid group
#if 0 /* TODO */
		if (dev->sb[blk].sb_luninfo.ndatalun < 8)
			dev->sb[blk].sb_luninfo.luns = dev->sb[blk].sb_luninfo.ndatalun = 0;
#endif
	}
}

/*-----------------------------------------------------------------------------------------------------------*/
static void check_ecc_power(struct shannon_dev *dev, int power)
{
	return;

	if (16 == dev->tmode) {
		if (power != 6) {
			printf("%s() fail: tmode=%d ecc_power=%d\n", __func__, dev->tmode, power);
			exit(EXIT_FAILURE);
		}
	} else if (35 == dev->tmode || 50 == dev->tmode || 58 == dev->tmode) {
		if (power != 12 && power != 8) {
			printf("%s() fail: tmode=%d ecc_power=%d\n", __func__, dev->tmode, power);
			exit(EXIT_FAILURE);
		}
	} else {
		printf("%s(): invalid tmode %d\n", __func__, dev->tmode);
		exit(EXIT_FAILURE);
	}
}

static void mpt_setvalue_mbr(struct shannon_dev *dev, struct shannon_mbr *mbr)
{
	int lun, blk, init_hot_sblk, init_cold_sblk;
	u64 ndatablock, capacity;

	memset(mbr, 0x00, sizeof(*mbr));

	/* look for init_hot_sblk and init_cold_sblk */
	for (blk = MPT_MBR_NBLK/dev->config_bakup->nplane; blk < dev->flash->nblk / dev->config_bakup->nplane; blk++) {
		if (dev->sb[blk].sb_luninfo.ndatalun != 0) {
			init_hot_sblk = blk;
			break;
		}
	}
	assert(init_hot_sblk < dev->flash->nblk / dev->config_bakup->nplane);

	for (blk += 1; blk < dev->flash->nblk / dev->config_bakup->nplane; blk++) {
		if (dev->sb[blk].sb_luninfo.ndatalun != 0) {
			init_cold_sblk = blk;
			break;
		}
	}
	assert(init_cold_sblk < dev->flash->nblk / dev->config_bakup->nplane);

	/* calculate capacity */
	ndatablock = 0;
	for (blk = MPT_MBR_NBLK/dev->config_bakup->nplane; blk < dev->flash->nblk / dev->config_bakup->nplane; blk++)
		ndatablock += dev->sb[blk].sb_luninfo.ndatalun;
	if (ndatablock <= 17 * (dev->valid_luns - dev->group_raid_num)) {
		printf("Capacity too small\n");
		exit(EXIT_FAILURE);
	}
	ndatablock -= 17 * (dev->valid_luns - dev->group_raid_num);
	capacity = (ndatablock * dev->flash->npage * dev->config_bakup->chunk_ndata) >> 9;	// capacity in unit sector
	if (0 == dev->mpt_set_capacity) {
		capacity = capacity * (100 - dev->config_bakup->overprovision_rate) / 100;	// delet overprovision_rate
	} else {
		if (dev->mpt_set_capacity > (capacity << 9) * 95/100) {
			printf("WARNNING: your input capacity is larger than 95%% physical capacity will be ignored, use default\n");
			capacity = capacity * (100 - dev->config_bakup->overprovision_rate) / 100;	// delet overprovision_rate
		} else
			capacity = (dev->mpt_set_capacity + 511) >> 9;
	}
	capacity = (capacity + 0x7FFllu) & (~0x7FFllu);						// align 1MB

	/* set per-member */
	sprintf(mbr->id, MBR_ID);
	mbr->mbr_version = dev->mbr_version;
	mbr->hardware_version = dev->config_bakup->hardware_version;
	mbr->software_version = dev->config_bakup->software_version;
	mbr->nand_manufacture = dev->flash->vendor_id;
	mbr->nand_id = dev->flash->device_id;
	mbr->capacity = capacity;

	mbr->lun_amount = dev->config_bakup->luns;
	mbr->eblocks_in_lun = dev->flash->nblk;
	mbr->pages_in_eblock = dev->flash->npage;
	mbr->nand_page_shift = dev->flash->page_size_shift;
	mbr->oob_size = dev->flash->oob_size;
	mbr->logicb_shift = dev->config_bakup->sector_size_shift;
	if (1 == dev->config_bakup->nplane)
		mbr->plane_order = 0;
	else if (2 == dev->config_bakup->nplane)
		mbr->plane_order = 1;
	else if (4 == dev->config_bakup->nplane)
		mbr->plane_order = 2;
	else
		assert(0);
	mbr->cfg_nchannel = dev->config_bakup->nchannel;
	mbr->cfg_nthread = dev->config_bakup->nthread;
	mbr->cfg_nlun = dev->config_bakup->nlun;

	mbr->init_hot_sblk = init_hot_sblk;
	mbr->init_cold_sblk = init_cold_sblk;

	mbr->interrupt_delay = dev->interrupt_delay ? dev->interrupt_delay : 5;
	mbr->ecc_codewords_in_logicb = dev->config_bakup->sector_ncodeword;
	mbr->ecc_correction_power = dev->hw_config->hw_ecc_power & 0x0F;
	check_ecc_power(dev, mbr->ecc_correction_power);
	mbr->flash_drvmode = dev->config_bakup->drvmode + 1;
	mbr->lun_map_mode = dev->newlunmap;
	mbr->luns_per_ce_mask = dev->config->lun_mask;
	mbr->raid_stripes = dev->group_raid_num;

	/* set bad physical lun bitmap */
	memset(mbr->bad_phy_lun_map, 0xFF, sizeof(mbr->bad_phy_lun_map));
	mbr->user_logicb_shift = dev->config_bakup->user_sector_size_shift;
	mbr->power_budget = dev->power_budget;
	for_dev_each_lun(dev, lun)  // NOTE: the loop of for_dev_each_lun has skipped badlun
		clear_bit(log2phy_lun(dev, lun), (unsigned long *)mbr->bad_phy_lun_map);

	if (dev->atomic_write)
		mbr->feature_flags |= ATOMIC_WRITE;
	if (dev->prioritize_write)
		mbr->feature_flags |= PRIORITIZE_WRITE;

	mbr->dma_max_read_limit = dev->dma_max_read_limit;
	mbr->clk = dev->flash_ifclock;
	mbr->max_outstanding_bios = dev->max_outstanding_bios;
}

static void mpt_print_mbr(struct shannon_dev *dev, struct shannon_mbr *mbr)
{
	int i, fence = 0;

	if (strcmp(mbr->id, MBR_ID)) {
		printf("No MBR\n");
		return;
	}

	printf("id=%s\n", mbr->id);
	printf("mbr_version=0x%04lX\n", mbr->mbr_version);
	printf("hardware_version=0x%08lX\n", mbr->hardware_version);
	printf("software_version=%ld\n", mbr->software_version);
	printf("nand_manufacture=0x%02lX\n", mbr->nand_manufacture);
	printf("nand_id=0x%02lX\n", mbr->nand_id);
	printf("capacity=%ld sector (%ld MB, %ld GiB)\n", mbr->capacity, mbr->capacity >> 11, (mbr->capacity * 512)/1000000000ul);

	printf("lun_amount=%d\n", mbr->lun_amount);
	printf("eblocks_in_lun=%d\n", mbr->eblocks_in_lun);
	printf("pages_in_eblock=%d\n", mbr->pages_in_eblock);
	printf("nand_page_shift=%d\n", mbr->nand_page_shift);
	printf("oob_size=%d\n", mbr->oob_size);
	printf("logicb_shift=%d\n", mbr->logicb_shift);
	printf("plane_order=%d\n", mbr->plane_order);
	printf("cfg_nchannel=%d\n", mbr->cfg_nchannel);
	printf("cfg_nthread=%d\n", mbr->cfg_nthread);
	printf("cfg_nlun=%d\n", mbr->cfg_nlun);

	printf("init_hot_sblk=%d\n", mbr->init_hot_sblk);
	printf("init_cold_sblk=%d\n", mbr->init_cold_sblk);

	printf("interrupt_delay=%d\n", mbr->interrupt_delay);
	// printf("overprovision_rate=%d\n", mbr->overprovision_rate);
	printf("ecc_codewords_in_logicb=%d\n", mbr->ecc_codewords_in_logicb);
	printf("ecc_correction_power=%d\n", mbr->ecc_correction_power);
	printf("flash_drvmode=%d\n", mbr->flash_drvmode - 1);
	printf("lun_map_mode=%d\n", mbr->lun_map_mode);
	printf("luns_per_ce_mask=0x%02X\n", mbr->luns_per_ce_mask);
	printf("group_raid_num=%d\n", mbr->raid_stripes);

	printf("bad_phy_lun_map=");
	for (i = 0; i < 7; i++)
		printf("%016lX_", mbr->bad_phy_lun_map[i]);
	printf("%016lX\n", mbr->bad_phy_lun_map[7]);
	printf("user_logicb_shift=%d\n", mbr->user_logicb_shift);
	printf("power_budget=%d\n", mbr->power_budget);

	printf("atomic_write=%d\n", (mbr->feature_flags & ATOMIC_WRITE) ? 1 : 0);
	printf("prioritize_write=%d\n", (mbr->feature_flags & PRIORITIZE_WRITE) ? 1 : 0);

	printf("dma_max_read_limit=%d\n", mbr->dma_max_read_limit);
	printf("clk=%d\n", mbr->clk);
	printf("max_outstanding_bios=%d\n", mbr->max_outstanding_bios);

	printf("*inherent_mbr_sector_size=%d\n", dev->inherent_mbr_sector_size);

	printf("\033[0;1;31mFenced Bad Log Luns:\033[0m");
	for (i = 0; i < dev->config->luns; i++) {
		if (test_bit(i, dev->lun_bitmap))
			continue;

		if (test_bit(log2phy_lun(dev, i), (unsigned long *)mbr->bad_phy_lun_map)) {
			printf(" %d", i);
			fence++;
		}
	}
	if (fence)
		printf(" total=%d", fence);
	printf("\n");

	fence = 0;
	printf("\033[0;1;31mFenced Bad Phy Luns:\033[0m");
	for (i = 0; i < dev->config->luns; i++) {
		if (test_bit(phy2log_lun(dev, i), dev->lun_bitmap))
			continue;

		if (test_bit(i, (unsigned long *)mbr->bad_phy_lun_map)) {
			printf(" %d", i);
			fence++;
		}
	}
	if (fence)
		printf(" total=%d", fence);
	printf("\n");
}

/*-----------------------------------------------------------------------------------------------------------*/
#define GET_MBR_LINE(key, keyname, base, must)					\
	do {									\
		int k;								\
										\
		for (k = 0; k < fn; k++) {					\
			if (!strcmp(keyname, fctx[k][0])) {			\
				mbr->key = strtoul(fctx[k][1], NULL, base);	\
				break;						\
			}							\
		}								\
										\
		if (must && k >= fn) {						\
			printf("No found %s in mbr file\n", keyname);		\
			exit(EXIT_FAILURE);					\
		}								\
	} while(0)

static void mpt_setvalue_mbr_from_file(struct shannon_dev *dev, struct shannon_mbr *mbr, FILE *fp)
{
	int i, j, fn;
	char line[1024], *fctx[512][2], *stmp, *ep, *np;

	fn = 0;
	while(fgets(line, sizeof(line), fp)) {
		if ('*' == line[0])
			continue;

		if ((ep = strchr(line, '=')) == NULL) {
			printf("Invalid mbr line: %s\n", line);
			exit(EXIT_FAILURE);
		}
		*ep = '\0';

		fctx[fn][0]=strdup(line);
		fctx[fn][1]=strdup(ep+1);

		if (++fn >= 512) {
			printf("Too many lines in MBR file %d\n", fn);
			exit(EXIT_FAILURE);
		}
	}

	// for (i = 0; i < fn; i++)
	//	printf("%s:%s", fctx[i][0], fctx[i][1]);

	if (strncmp(fctx[0][1], MBR_ID, strlen(MBR_ID))) {
		printf("The 1st line of MBR file is invalid: %s=%s", fctx[0][0], fctx[0][1]);
		exit(EXIT_FAILURE);
	}

	sprintf(mbr->id, MBR_ID);

	GET_MBR_LINE(mbr_version, "mbr_version", 16, 1);
	GET_MBR_LINE(hardware_version, "hardware_version", 16, 0);
	GET_MBR_LINE(software_version, "software_version", 10, 0);
	GET_MBR_LINE(nand_manufacture, "nand_manufacture", 16, 0);
	GET_MBR_LINE(nand_id, "nand_id", 16, 0);
	GET_MBR_LINE(capacity, "capacity", 10, 1);

	GET_MBR_LINE(lun_amount, "lun_amount", 10, 1);
	GET_MBR_LINE(eblocks_in_lun, "eblocks_in_lun", 10, 1);
	GET_MBR_LINE(pages_in_eblock, "pages_in_eblock", 10, 1);
	GET_MBR_LINE(nand_page_shift, "nand_page_shift", 10, 1);
	GET_MBR_LINE(oob_size, "oob_size", 10, 1);
	GET_MBR_LINE(logicb_shift, "logicb_shift", 10, 1);
	GET_MBR_LINE(plane_order, "plane_order", 10, 1);
	GET_MBR_LINE(cfg_nchannel, "config_channels", 10, 1);
	GET_MBR_LINE(cfg_nthread, "config_lunset_in_channel", 10, 1);
	GET_MBR_LINE(cfg_nlun, "config_lun_in_lunset", 10, 1);

	GET_MBR_LINE(init_hot_sblk, "init_hot_sblk", 10, 1);
	GET_MBR_LINE(init_cold_sblk, "init_cold_sblk", 10, 1);

	GET_MBR_LINE(ecc_codewords_in_logicb, "ecc_codewords_in_logicb", 10, 1);
	GET_MBR_LINE(ecc_correction_power, "ecc_correction_power", 10, 1);
	GET_MBR_LINE(history_erase_count, "history_erase_count", 10, 0);

	GET_MBR_LINE(power_cycle_count, "power_cycle_count", 10, 0);
	GET_MBR_LINE(power_on_seconds, "power_on_seconds", 10, 0);
	GET_MBR_LINE(host_write_sectors, "host_write_sectors", 10, 0);
	GET_MBR_LINE(total_write_sectors, "total_write_sectors", 10, 0);
	GET_MBR_LINE(host_read_sectors, "host_read_sectors", 10, 0);

	GET_MBR_LINE(flash_drvmode, "flash_drvmode", 10, 0);
	GET_MBR_LINE(luns_per_ce_mask, "luns_per_ce_mask", 16, 1);
	GET_MBR_LINE(lun_map_mode, "lun_map_mode", 10, 0);
	GET_MBR_LINE(raid_stripes, "raid_stripes", 10, 0);

	GET_MBR_LINE(max_pages_in_eblock, "max_pages_in_eblock", 10, 0);
	GET_MBR_LINE(user_logicb_shift, "user_logicb_shift", 10, 0);

	/* get bad_phy_lun_map */
	ep = NULL;
	for (i = 0; i < fn; i++) {
		if (!strcmp(fctx[i][0], "bad_phy_lun_map")) {
			ep = fctx[i][1];
			break;
		}
	}
	if (NULL == ep) {
		printf("No foundd bad_phy_lun_map in mbr file\n");
		exit(EXIT_FAILURE);
	}
	stmp = ep;

	np = ep;
	for (i = 0; i < 8; i++) {
		mbr->bad_phy_lun_map[i] = strtoul(np, &ep, 16);
		if (i != 7 && *ep != '_') {
			printf("Invalid bad_phy_lun_map in mbr %s", stmp);
			exit(EXIT_FAILURE);
		}
		np = ep + 1;

		for (j = 0; j < 64; j++) {
			if (i*64+j >= dev->hw_luns)
				continue;

			if (test_bit(j, (unsigned long *)&mbr->bad_phy_lun_map[i]))
				set_bit(phy2log_lun(dev, i*64+j), dev->lun_bitmap);
		}
	}

	fclose(fp);
	// mpt_print_mbr(dev, mbr);
}

static void mpt_setbbt_from_file(struct shannon_dev *dev, struct shannon_bbt *bbt, FILE *fp)
{
	char line[1024], *p, *ep, *np;
	int i, j, blk;
	unsigned long bmp;

	while (fgets(line, sizeof(line), fp)) {
		if ((p = strchr(line, '[')) == NULL) {
			printf("Invalid bbt line %s", line);
			exit(EXIT_FAILURE);
		}

		blk = strtoul(p+1, NULL, 10);
		if (blk * (1 << dev->mbr->plane_order) < MPT_MBR_NBLK)
			continue;

		// printf("bad_lun[%04d]:", blk);

		if ((p = strchr(line, ':')) == NULL) {
			printf("Invalid bbt line %s", line);
			exit(EXIT_FAILURE);
		}
		np = p + 1;

		for (i = 0; i < 4; i++) {
			bmp = strtoul(np, &ep, 16);
			if (i != 3 && *ep != ' ') {
				printf("Invalid bbt line %s", line);
				exit(EXIT_FAILURE);
			}
			np = ep + 1;

			// printf(" %016lX", bmp);

			for (j = 0; j < 64; j++) {
				if (test_bit(j, &bmp))
					set_bit(i*64+j, bbt->sb_bbt[blk]);
			}
		}
		// printf("\n");
	}

	fclose(fp);
}

/*-----------------------------------------------------------------------------------------------------------*/
static void mpt_write_mbr(struct shannon_dev *dev, struct shannon_mbr *mbr)
{
	int i, lun, blk, ppa, head;
	struct shannon_bbt *mbr_bbt;
	struct shannon_request *req, *tmp;
	struct list_head req_head;
	struct shannon_mbr *le_mbr;

	assert(!strcmp(MBR_ID, mbr->id));
	assert(dev->config->sector_size_shift == (dev->mbr_by_4k ? 12 : 9));
	assert(ECCMODE_ENABLE == dev->config->ecc_mode);

	dev->inherent_mbr_sector_size = 2 << dev->config->sector_size_shift;

	le_mbr = zmalloc(sizeof(*le_mbr));
	assert(NULL != le_mbr);
	memcpy(le_mbr, mbr, sizeof(*le_mbr));

	/* convert MBR info from cpu to LE */
	// char id[32])
	cpu_to_le64s(&le_mbr->mbr_version);
	cpu_to_le64s(&le_mbr->hardware_version);
	cpu_to_le64s(&le_mbr->software_version);
	cpu_to_le64s(&le_mbr->nand_manufacture);
	cpu_to_le64s(&le_mbr->nand_id);
	cpu_to_le64s(&le_mbr->capacity);

	cpu_to_le32s(&le_mbr->lun_amount);
	cpu_to_le32s(&le_mbr->eblocks_in_lun);
	cpu_to_le32s(&le_mbr->pages_in_eblock);
	cpu_to_le32s(&le_mbr->nand_page_shift);
	cpu_to_le32s(&le_mbr->oob_size);
	cpu_to_le32s(&le_mbr->logicb_shift);
	cpu_to_le32s(&le_mbr->plane_order);
	cpu_to_le32s(&le_mbr->cfg_nchannel);
	cpu_to_le32s(&le_mbr->cfg_nthread);
	cpu_to_le32s(&le_mbr->cfg_nlun);

	cpu_to_le32s(&le_mbr->init_hot_sblk);
	cpu_to_le32s(&le_mbr->init_cold_sblk);

	cpu_to_le16s(&le_mbr->interrupt_delay);
	// u8 ecc_codewords_in_logicb
	// u8 ecc_correction_power
	cpu_to_le32s(&le_mbr->history_erase_count);

	cpu_to_le64s(&le_mbr->power_cycle_count);
	cpu_to_le64s(&le_mbr->power_on_seconds);
	cpu_to_le64s(&le_mbr->host_write_sectors);
	cpu_to_le64s(&le_mbr->total_write_sectors);
	cpu_to_le64s(&le_mbr->host_read_sectors);

	cpu_to_le32s(&le_mbr->flash_drvmode);
	// u8 luns_per_ce_mask
	// u8 lun_map_mode
	cpu_to_le16s(&le_mbr->raid_stripes);

	for (i = 0; i <ARRAY_SIZE(mbr->bad_phy_lun_map); i++) cpu_to_le64s(&le_mbr->bad_phy_lun_map[i]);
	cpu_to_le32s(&le_mbr->max_pages_in_eblock);
	cpu_to_le32s(&le_mbr->user_logicb_shift);

	cpu_to_le64s(&le_mbr->feature_flags);
	// u8 power_budget;

	// u8 dma_max_read_limit;
	cpu_to_le16s(&le_mbr->clk);
	cpu_to_le32s(&le_mbr->max_outstanding_bios);
	/* convert END */

	mbr_bbt = dev->private_data;
	assert(NULL != mbr_bbt);

	head = INDEP_HEAD;
	INIT_LIST_HEAD(&req_head);

	for (blk = 0; blk < MPT_MBR_NBLK; blk++) {
		ppa = blk * dev->flash->npage + MPT_MBR_PAGEOFF;

		/* write requests */
		for_dev_each_lun(dev, lun) {
			if (test_bit(lun, mbr_bbt->sb_bbt[blk]))
				continue;

			req = alloc_request(dev, sh_write_cmd, lun, ppa, head, 0, 1);
			if (NULL == req)
				malloc_failed_exit();
			memset(req->data, 0x00, dev->config->sector_size);
			memcpy(req->data, le_mbr, sizeof(*le_mbr));
			req->metadata[0] = MBR_WATERMARK;	
			list_add_tail(&req->list, &req_head);
		}

		/* submit requests and run them */
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

		/* check status */
		list_for_each_entry(req, &req_head, list) {
			if (check_req_status(req))
				exitlog("write MBR check status failed: lun-%03d block-%03d\n", req->lun, req->block);
		}

		list_for_each_entry_safe(req, tmp, &req_head, list) {
			list_del(&req->list);
			free_request(req);
		}
	}

	free(le_mbr);
}

static void mpt_write_bbt(struct shannon_dev *dev, struct shannon_bbt *bbt)
{
	int i, lun, blk, ppa, plane, head, mblk;
	struct shannon_request *lun_head_req, *req, *tmp;
	struct list_head req_head;
	struct shannon_bbt *mbr_bbt;
	__u16 *blkmap, off;

	mbr_bbt = dev->private_data;
	assert(NULL != mbr_bbt);

	head = INDEP_HEAD;
	INIT_LIST_HEAD(&req_head);

	/* all write requests */
	for_dev_each_lun(dev, lun) {
		lun_head_req = NULL;

		if (dev->private_int)
			print("Lun-%02d Phylun-%03d badblock:", lun, log2phy_lun(dev, lun));

		for (mblk = 0; mblk < MPT_MBR_NBLK; mblk++) {
			if (test_bit(lun, mbr_bbt->sb_bbt[mblk]))
				continue;

			ppa = mblk * dev->flash->npage + MPT_BBT_PAGEOFF;

			/* 1st valid mbr block */
			if (NULL == lun_head_req) {
				req = alloc_request(dev, sh_write_cmd, lun, ppa, head, 0, dev->config_bakup->page_nsector);
				if (NULL == req)
					malloc_failed_exit();
				req->bfree = 0;
				if (dev->bm_bbt) {
					memset(req->data, 0x00, dev->config_bakup->ndata);
					*((unsigned long *)req->data) = BITMAP_BBT_MAGIC;
				} else
					memset(req->data, 0xFF, dev->config_bakup->ndata);
				for (i = 0; i < dev->config->page_nsector; i++)
					req->metadata[i] = MBR_WATERMARK;

				off = 0;	// analyse invalid block location of this lun
				blkmap = (__u16 *)req->data;
				for (blk = 0; blk < MPT_MBR_NBLK; blk++) {
					if (test_bit(lun, mbr_bbt->sb_bbt[blk])) {
						if (dev->bm_bbt)
							set_bit(blk, (unsigned long *)req->data + 1);
						else
							blkmap[off++] = cpu_to_le16((__u16)blk);
						dev->invalid_blocks++;

						if (dev->private_int)
							print(" %d", blk);
					}
				}
				for (blk = MPT_MBR_NBLK / dev->config_bakup->nplane; blk < dev->flash->nblk / dev->config_bakup->nplane; blk++) {
					if (test_bit(lun, bbt->sb_bbt[blk])) {
						for (plane = 0; plane < dev->config_bakup->nplane; plane++) {
							if (dev->bm_bbt)
								set_bit(blk * dev->config_bakup->nplane + plane, (unsigned long *)req->data + 1);
							else
								blkmap[off++] = cpu_to_le16((__u16)(blk * dev->config_bakup->nplane + plane));
							dev->invalid_blocks++;

							if (dev->private_int)
								print(" %d", blk * dev->config_bakup->nplane + plane);
						}
					}
				}

				if (dev->bm_bbt) {
					for (i = 0; i < dev->config_bakup->ndata/8; i++)
						cpu_to_le64s((unsigned long *)req->data + i);
				}

				lun_head_req = req;
				list_add_tail(&lun_head_req->list, &req_head);
			} else {
			/* other backup mbr block */
				req = alloc_request(dev, sh_write_cmd, lun, ppa, head, 0, 0);
				if (NULL == req)
					malloc_failed_exit();
				req->bfree = 0;
				req->nsector = dev->config->page_nsector;
				req->data = lun_head_req->data;
				req->metadata = lun_head_req->metadata;
				list_add_tail(&req->list, &req_head);
			}
		}

		if (dev->private_int)
			printf("\n");

		req->bfree = 1;		// last backup mbr block free bbt memory
	}

	/* submit requests and run them */
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

	/* check status */
	list_for_each_entry(req, &req_head, list) {
		if (check_req_status(req))
			exitlog("write MBR check status failed: lun-%03d block-%03d\n", req->lun, req->block);
	}

	list_for_each_entry_safe(req, tmp, &req_head, list) {
		list_del(&req->list);
		free_request(req);
	}
}

/*-----------------------------------------------------------------------------------------------------------*/
/*
 * getonly == 1
 * 	return immediately when found one valid MBR info
 *
 * getonly == 0, check which MBR block lost MBR info.
 * 	if (NULL == (mbr_bbt = dev->private_data))
 * 		skip bad MBR blocks
 *
 * enable_print 1 -- print MBR to stdout; 0 -- disable print MBR
 */

static int __readmbr(struct shannon_dev *dev, struct list_head *req_head, struct list_head *arreq_head)
{
	int i, lun, blk, ppa, head;
	struct shannon_mbr *mbr;
	struct shannon_request *req, *arreq, *tmp;

	head = INDEP_HEAD;
	INIT_LIST_HEAD(req_head);
	INIT_LIST_HEAD(arreq_head);

	/* all read requests */
	for_dev_each_lun(dev, lun) {
		for (blk = 0; blk < MPT_MBR_NBLK; blk++) {
			ppa = blk * dev->flash->npage + MPT_MBR_PAGEOFF;

			req = alloc_request(dev, sh_preread_cmd, lun, ppa, head, 0, 0);
			if (NULL == req)
				malloc_failed_exit();
			list_add_tail(&req->list, req_head);

			req = alloc_request(dev, sh_cacheread_cmd, lun, ppa, head, 0, 1);
			if (NULL == req)
				malloc_failed_exit();
			list_add_tail(&req->list, req_head);

			/* ar read */
			arreq = alloc_request(dev, sh_cacheread_cmd, lun, ppa, head, 0, 1);
			if (NULL == arreq)
				malloc_failed_exit();
			arreq->advance_read = 1;
			req->sibling_arreq = arreq;
			list_add_tail(&arreq->list, arreq_head);
		}
	}

	/* submit requests and run them */
	list_for_each_entry(req, req_head, list) {
		if (dev->submit_request(req))
			submit_failed_exit(req->lun);
	}

	for_dev_each_lun(dev, lun)
		update_cmdqueue(dev, lun);

	for_dev_each_lun(dev, lun) {
		if (poll_cmdqueue(dev, lun))
			poll_failed_exit(lun);
	}

	/* submit ar requests and run them */
	list_for_each_entry(req, arreq_head, list) {
		if (dev->submit_request(req))
			submit_failed_exit(req->lun);
	}

	for_dev_each_lun(dev, lun)
		update_cmdqueue(dev, lun);

	for_dev_each_lun(dev, lun) {
		if (poll_cmdqueue(dev, lun))
			poll_failed_exit(lun);
	}

	/* display MBR if have */
	list_for_each_entry(req, req_head, list) {
		if (sh_cacheread_cmd == req->opcode) {
			mbr = (struct shannon_mbr *)req->data;

			if (req->ecc[0] < 251 && !strcmp(MBR_ID, mbr->id)) {
				/* convert MBR info from le to cpu */
				//char id[32]
				le64_to_cpus(&mbr->mbr_version);
				le64_to_cpus(&mbr->hardware_version);
				le64_to_cpus(&mbr->software_version);
				le64_to_cpus(&mbr->nand_manufacture);
				le64_to_cpus(&mbr->nand_id);
				le64_to_cpus(&mbr->capacity);

				le32_to_cpus(&mbr->lun_amount);
				le32_to_cpus(&mbr->eblocks_in_lun);
				le32_to_cpus(&mbr->pages_in_eblock);
				le32_to_cpus(&mbr->nand_page_shift);
				le32_to_cpus(&mbr->oob_size);
				le32_to_cpus(&mbr->logicb_shift);
				le32_to_cpus(&mbr->plane_order);
				le32_to_cpus(&mbr->cfg_nchannel);
				le32_to_cpus(&mbr->cfg_nthread);
				le32_to_cpus(&mbr->cfg_nlun);

				le32_to_cpus(&mbr->init_hot_sblk);
				le32_to_cpus(&mbr->init_cold_sblk);

				le16_to_cpus(&mbr->interrupt_delay);
				// u8 ecc_codewords_in_logicb
				// u8 ecc_correction_power
				le32_to_cpus(&mbr->history_erase_count);

				le64_to_cpus(&mbr->power_cycle_count);
				le64_to_cpus(&mbr->power_on_seconds);
				le64_to_cpus(&mbr->host_write_sectors);
				le64_to_cpus(&mbr->total_write_sectors);
				le64_to_cpus(&mbr->host_read_sectors);

				le32_to_cpus(&mbr->flash_drvmode);
				// u8 luns_per_ce_mask
				// u8 lun_map_mode
				le16_to_cpus(&mbr->raid_stripes);

				for (i = 0; i < ARRAY_SIZE(mbr->bad_phy_lun_map); i++) le64_to_cpus(&mbr->bad_phy_lun_map[i]);
				le32_to_cpus(&mbr->max_pages_in_eblock);
				le32_to_cpus(&mbr->user_logicb_shift);

				cpu_to_le64s(&mbr->feature_flags);
				// u8 power_budget;

				// u8 dma_max_read_limit;
				cpu_to_le16s(&mbr->clk);
				cpu_to_le32s(&mbr->max_outstanding_bios);
				/* convert END */

				memcpy(dev->mbr, mbr, sizeof(*mbr));
				memcpy(dev->inherent_mbr, mbr, sizeof(*mbr));
				return 0;
			}
		}
	}

	list_for_each_entry_safe(req, tmp, req_head, list) {
		list_del(&req->list);
		free_request(req);
	}

	list_for_each_entry_safe(req, tmp, arreq_head, list) {
		list_del(&req->list);
		free_request(req);
	}
	return 1;
}

static int shannon_mpt_readmbr(struct shannon_dev *dev, int getonly, int enable_print)
{
	struct list_head req_head, arreq_head;
	struct shannon_bbt *mbr_bbt;
	struct shannon_request *req, *tmp;

	mbr_bbt = (struct shannon_bbt *)dev->private_data;

	/* try with 512 */
	if (!dev->mbr_by_4k) {
		dev->config->sector_size_shift = 9;
		dev->config->sector_ncodeword = 1;
		dev->config->ecc_mode = ECCMODE_ENABLE;
		dev->config->raid_mode = RAIDMODE_DISABLE;
		dev->config->nplane = 1;
		if (re_init_device(dev)) {
			printf("Re-init-device (WRITE-MBR) failed."MPT_HELP);
			exit(EXIT_FAILURE);
		}

		if (!__readmbr(dev, &req_head, &arreq_head)) {
			dev->inherent_mbr_sector_size = 512;
			goto found_mbr;
		}
	}

	/* try with 4096 */
	dev->config->sector_size_shift = dev->config_bakup->sector_size_shift;
	dev->config->sector_ncodeword = dev->config_bakup->sector_ncodeword;
	dev->config->ecc_mode = ECCMODE_ENABLE;
	dev->config->raid_mode = RAIDMODE_DISABLE;
	dev->config->nplane = 1;
	if (re_init_device(dev)) {
		printf("Re-init-device (WRITE-MBR) failed."MPT_HELP);
		exit(EXIT_FAILURE);
	}

	if (!__readmbr(dev, &req_head, &arreq_head)) {
		dev->inherent_mbr_sector_size = 4096;
		goto found_mbr;
	}

	printf("No MBR found. Maybe not format!\n");
	return ERR;

found_mbr:
	assert(dev->inherent_mbr_sector_size == 512 || dev->inherent_mbr_sector_size == 4096);
	if (enable_print)
		mpt_print_mbr(dev, dev->mbr);

	/* check which MBR lun-block lost MBR */
	if (!getonly) {
		list_for_each_entry(req, &req_head, list) {
			if (NULL != mbr_bbt) {		// skip check MBR bad block
				if (test_bit(req->lun, mbr_bbt->sb_bbt[req->block]))
					continue;
			}

			if (sh_cacheread_cmd == req->opcode) {
				if ((req->ecc[0] >= 251 || strcmp(MBR_ID, ((struct shannon_mbr *)(req->data))->id)) &&
					(req->sibling_arreq->ecc[0] >= 251 || strcmp(MBR_ID, ((struct shannon_mbr *)(req->sibling_arreq->data))->id))) {
					dev->private_int = 1;
					printf("### NOTE: lun-%03d phylun-%03d block-%02d lost MBR\n",
						req->lun, log2phy_lun(dev, req->lun), req->block);
				}
			}
		}
	}

	list_for_each_entry_safe(req, tmp, &req_head, list) {
		list_del(&req->list);
		free_request(req);
	}

	list_for_each_entry_safe(req, tmp, &arreq_head, list) {
		list_del(&req->list);
		free_request(req);
	}
	return 0;
}

/*
 * check_only == 0, read bbt info
 * 	if (NULL == used_bbt)
 * 		print bbt info to stdout
 * 	else
 * 		save bbt into used_bbt
 *
 * check_only == 1, check which MBR block lost BBT info.
 * 	if (NULL == (mbr_bbt = dev->private_data))
 * 		skip bad MBR blocks
 */
int shannon_mpt_readbbt(struct shannon_dev *dev, int check_only)
{
	int i, lun, blk, ppa, head;
	struct shannon_request *req, *arreq, *tmp;
	struct list_head req_head, arreq_head;
	__u16 *blkmap;
	struct shannon_bbt *mbr_bbt;

	mbr_bbt = (struct shannon_bbt *)dev->private_data;

	/* read MBR first */
	if (shannon_mpt_readmbr(dev, 1, 0))
		return ERR;

	/* read BBT according to configuration in MBR */
	dev->config->sector_size_shift = dev->mbr->logicb_shift;
	dev->config->sector_ncodeword = dev->mbr->ecc_codewords_in_logicb;
	dev->config->ecc_mode = ECCMODE_ENABLE;		// FIXME: add ecc_mode in MBR
	dev->config->raid_mode = RAIDMODE_DISABLE;
	dev->config->nplane = 1;
	if (re_init_device(dev)) {
		printf("Re-init-device (WRITE-MBR) failed."MPT_HELP);
		exit(EXIT_FAILURE);
	}

	head = INDEP_HEAD;
	INIT_LIST_HEAD(&req_head);
	INIT_LIST_HEAD(&arreq_head);

	/* all read requests */
	for_dev_each_lun(dev, lun) {
		for (blk = 0; blk < MPT_MBR_NBLK; blk++) {
			ppa = blk * dev->flash->npage + MPT_BBT_PAGEOFF;

			req = alloc_request(dev, sh_preread_cmd, lun, ppa, head, 0, 0);
			if (NULL == req)
				malloc_failed_exit();
			list_add_tail(&req->list, &req_head);

			req = alloc_request(dev, sh_cacheread_cmd, lun, ppa, head, 0,
					(dev->config->page_nsector) > 8 ? 8 : dev->config->page_nsector);
			if (NULL == req)
				malloc_failed_exit();
			list_add_tail(&req->list, &req_head);

			/* ar read */
			arreq = alloc_request(dev, sh_cacheread_cmd, lun, ppa, head, 0,
					(dev->config->page_nsector) > 8 ? 8 : dev->config->page_nsector);
			if (NULL == arreq)
				malloc_failed_exit();
			arreq->advance_read = 1;
			req->sibling_arreq = arreq;
			list_add_tail(&arreq->list, &arreq_head);
		}
	}

	/* submit requests and run them */
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

	/* submit ar requests and run them */
	list_for_each_entry(req, &arreq_head, list) {
		if (dev->submit_request(req))
			submit_failed_exit(req->lun);
	}

	for_dev_each_lun(dev, lun)
		update_cmdqueue(dev, lun);

	for_dev_each_lun(dev, lun) {
		if (poll_cmdqueue(dev, lun))
			poll_failed_exit(lun);
	}

	if (check_only)
		goto check_only_bbt;

	/* display per-lun BBT if have */
	for_dev_each_lun(dev, lun) {
		if (NULL == used_bbt)
			printf("lun-%03d phylun-%03d bad blocks:", lun, log2phy_lun(dev, lun));

		list_for_each_entry(req, &req_head, list) {
			if (req->lun != lun)
				continue;
			if (sh_preread_cmd != req->opcode)
				continue;
			if (check_req_status_silent(req)) 				// check preread status
				continue;

			tmp = list_entry(req->list.next, struct shannon_request, list);	// check cachread ecc
			if (tmp->metadata[0] != MBR_WATERMARK)
				continue;

			for (i = 0; i < tmp->nsector; i++) {
				if (tmp->ecc[i] >= 251)
					break;
			}
			if (i == tmp->nsector)
				break;

			/* check sibling arreq */
			tmp = tmp->sibling_arreq;
			if (tmp->metadata[0] != MBR_WATERMARK)
				continue;

			for (i = 0; i < tmp->nsector; i++) {
				if (tmp->ecc[i] >= 251)
					break;
			}
			if (i == tmp->nsector)
				break;
		}

		/* travel all cachread req for this lun, but no valid bbt info */
		if (&req->list == &req_head) {
			if (NULL == used_bbt)
				printf(" \033[0;1;31mlost BBT\033[0m\n");
			continue;
		}

		/* found valid bbt info for this lun */
		if (cpu_to_le64(*((unsigned long *)tmp->data)) == BITMAP_BBT_MAGIC)
			goto read_bm_bbt;

		blkmap = (__u16 *)tmp->data;

		if (NULL != used_bbt) {
			for (i = 0; i < MAX_BADBLK_COUNT && le16_to_cpu(blkmap[i]) != 0xFFFF; i++) {
				if (le16_to_cpu(blkmap[i]) >= dev->flash->nblk) {
					printf("FATAL %s(): lun-%03d static badblk %d larger than phyical address!!!\n",
						 __func__, lun, le16_to_cpu(blkmap[i]));
				} else {
					set_bit(lun, used_bbt->sb_bbt[le16_to_cpu(blkmap[i])]);
					set_bit(lun, dev->sb[le16_to_cpu(blkmap[i])/dev->config_bakup->nplane].sb_luninfo.sb_bbt);
				}
			}

			if (i >= MAX_BADBLK_COUNT)
				exitlog("Too many bad blocks - %d, maybe some FATAL\n", i);
		} else {
			for (i = 0; i < MAX_BADBLK_COUNT && le16_to_cpu(blkmap[i]) != 0xFFFF; i++) {
				set_bit(lun, dev->sb[le16_to_cpu(blkmap[i])/dev->config_bakup->nplane].sb_luninfo.sb_bbt);
				printf(" %d", le16_to_cpu(blkmap[i]));
			}

			if (i >= MAX_BADBLK_COUNT)
				printf(" \033[0;1;31mToo many bad blocks - %d, maybe some FATAL\033[0m", i);

			if (i * 100 / dev->flash->nblk >= 5 )
				printf(" [%d/%d \033[0;1;31m%.2f%%\033[0m]\n", i, dev->flash->nblk, i * 100.0 / dev->flash->nblk);
			else
				printf(" [%d/%d \033[0;1;32m%.2f%%\033[0m]\n", i, dev->flash->nblk, i * 100.0 / dev->flash->nblk);
		}
		continue;

read_bm_bbt:
		for (i = 0; i < dev->config_bakup->ndata/8; i++)
			cpu_to_le64s((unsigned long *)tmp->data + i);

		if (NULL != used_bbt) {
			for (blk = 0; blk < dev->flash->nblk; blk++) {
				if (test_bit(blk, (unsigned long *)tmp->data + 1)) {
					set_bit(lun, used_bbt->sb_bbt[blk]);
					set_bit(lun, dev->sb[blk/dev->config_bakup->nplane].sb_luninfo.sb_bbt);
				}
			}
		} else {
			int lunbadblk = 0;

			for (blk = 0; blk < dev->flash->nblk; blk++) {
				if (test_bit(blk, (unsigned long *)tmp->data + 1)) {
					set_bit(lun, dev->sb[blk/dev->config_bakup->nplane].sb_luninfo.sb_bbt);
					lunbadblk++;
					printf(" %d", blk);
				}
			}

			if ((lunbadblk - dev->targetlun[lun].blk_hole_count) * 100 / dev->flash->nblk >= 5 )
				printf(" [%d/%d \033[0;1;31m%.2f%%\033[0m]\n", i, dev->flash->nblk, lunbadblk * 100.0 / dev->flash->nblk);
			else
				printf(" [%d/%d \033[0;1;32m%.2f%%\033[0m]\n", i, dev->flash->nblk, lunbadblk * 100.0 / dev->flash->nblk);
		}
	}
	goto free_req_out;

	/* check which MBR lun-block lost BBT */
check_only_bbt:
	list_for_each_entry(req, &req_head, list) {
		__u16 *pbad = (__u16 *)req->data;

		if (sh_cacheread_cmd != req->opcode)
			continue;

		if (NULL != mbr_bbt) {		// skip ckeck MBR bad block
			if (test_bit(req->lun, mbr_bbt->sb_bbt[req->block]))
				continue;
		}

		i = 0;
		if (cpu_to_le64(*((unsigned long *)req->data)) == BITMAP_BBT_MAGIC || le16_to_cpu(pbad[MAX_BADBLK_COUNT]) == 0xFFFF) {
			for (i = 0; i < req->nsector; i++) {
				if (req->ecc[i] >= 251)
					break;
			}
		}

		/*check ar read if needed */
		if (i != req->nsector) {
			i = 0;
			pbad = (__u16 *)req->sibling_arreq->data;

			if (cpu_to_le64(*((unsigned long *)req->sibling_arreq->data)) == BITMAP_BBT_MAGIC || le16_to_cpu(pbad[MAX_BADBLK_COUNT]) == 0xFFFF) {
				for (i = 0; i < req->sibling_arreq->nsector; i++) {
					if (req->sibling_arreq->ecc[i] >= 251)
						break;
				}
			}

			if (i != req->sibling_arreq->nsector) {
				dev->private_int = 1;
				printf("### NOTE: lun-%03d phylun-%03d block-%02d lost BBT\n",
					req->lun, log2phy_lun(dev, req->lun), req->block);
			}
		}
	}

	/* exit */
free_req_out:
	list_for_each_entry_safe(req, tmp, &req_head, list) {
		list_del(&req->list);
		free_request(req);
	}

	list_for_each_entry_safe(req, tmp, &arreq_head, list) {
		list_del(&req->list);
		free_request(req);
	}
	return 0;
}

/*-----------------------------------------------------------------------------------------------------------*/
#define	HW_REG_SIZE	16384

static struct live_context *alloc_live_context(struct shannon_dev *dev)
{
	int lun;
	struct live_context *context;

	assert(NULL != dev->mbr);

	context = zmalloc(sizeof(*context));
	if (NULL == context)
		goto out;
	context->dev = dev;

	context->reg = malloc(HW_REG_SIZE);
	if (NULL == context->reg)
		goto free_context_out;

	context->cmdq = zmalloc(dev->config->luns * sizeof(*context->cmdq));
	if (NULL == context->cmdq)
		goto free_reg_out;
	for_dev_each_lun(dev, lun) {
		context->cmdq[lun] = malloc(PAGE_SIZE);
		if (NULL == context->cmdq[lun])
			goto free_cmdq_out;
	}

	context->cmpq = zmalloc(dev->config->luns * sizeof(*context->cmpq));
	if (NULL == context->cmdq)
		goto free_cmdq_out;
	for_dev_each_lun(dev, lun) {
		context->cmpq[lun] = malloc(PAGE_SIZE);
		if (NULL == context->cmpq[lun])
			goto free_cmpq_out;
	}
	return context;

free_cmpq_out:
	for_dev_each_lun(dev, lun) {
		if (NULL == context->cmpq[lun])
			break;
		free(context->cmpq[lun]);
	}
	free(context->cmpq);
free_cmdq_out:
	for_dev_each_lun(dev, lun) {
		if (NULL == context->cmdq[lun])
			break;
		free(context->cmdq[lun]);
	}
	free(context->cmdq);
free_reg_out:
	free(context->reg);
free_context_out:
	free(context);
out:
	return NULL;
}

static void free_live_context(struct live_context *context)
{
	int lun;
	struct shannon_dev *dev = context->dev;

	for_dev_each_lun(dev, lun) {
		if (NULL == context->cmpq[lun])
			break;
		free(context->cmpq[lun]);
	}
	free(context->cmpq);

	for_dev_each_lun(dev, lun) {
		if (NULL == context->cmdq[lun])
			break;
		free(context->cmdq[lun]);
	}
	free(context->cmdq);

	free(context->reg);
	free(context);
}

static void record_live_context(struct live_context *context)
{
	int lun;
	struct shannon_dev *dev = context->dev;

	if (lseek(dev->fd, 0, SEEK_SET) < 0)
		perror_exit("%s() lseek failed", __func__);

	if (read(dev->fd, context->reg, HW_REG_SIZE) != HW_REG_SIZE)
		perror_exit("%s() read failed", __func__);

	for_dev_each_lun(dev, lun) {
		dev->read_mem(dev, context->cmdq[lun], dev->lun[lun].thread->cmdmem.kernel_addr, PAGE_SIZE);
		dev->read_mem(dev, context->cmpq[lun], dev->lun[lun].thread->cmdmem.kernel_addr + PAGE_SIZE, PAGE_SIZE);
	}
}

#if 0
static void save_live_context(struct live_context *context, FILE *fp)
{
	int i, lun;
	struct shannon_dev *dev = context->dev;

	fprintf(fp, "=================================== %s ===================================\n", context->id);

	fprintf(fp, "sys registers:");
	for (i = 0; i < sizeof(*dev->hw_sysinfo)/4; i++)
		fprintf(fp, " %08X ", context->reg[dev->sysreg_dwoff + i]);
	fprintf(fp, "\n");

	fprintf(fp, "cfg registers:");
	for (i = 0; i < sizeof(*dev->hw_config)/4; i++)
		fprintf(fp, " %08X", context->reg[dev->cfgreg_dwoff + i]);
	fprintf(fp, "\n");

	for_dev_each_lun(dev, lun) {
		fprintf(fp, "cmd lun-%03d phylun-%03d:", lun, log2phy_lun(dev, lun));

		for (i = 0; i < PAGE_SIZE / 8; i++) {
			if (0 == i % 8)
				fprintf(fp, "\n%04X:", i * 8);
			fprintf(fp, " %016lX", context->cmdq[lun][i]);
		}

		fprintf(fp, "\n");
	}

	for_dev_each_lun(dev, lun) {
		fprintf(fp, "cmp lun-%03d phylun-%03d:", lun, log2phy_lun(dev, lun));

		for (i = 0; i < PAGE_SIZE / 8; i++) {
			if (0 == i % 8)
				fprintf(fp, "\n%04X:", i * 8);
			fprintf(fp, " %016lX", context->cmpq[lun][i]);
		}

		fprintf(fp, "\n");
	}
}
#endif

/*-----------------------------------------------------------------------------------------------------------*/
static int shannon_mpt_read_used_bbt(struct shannon_dev *dev, int print_only, int skip_dynamic)
{
	int lun, blk, page, ppa, badblk;
	struct shannon_request *preread_req = NULL, *cachread_req = NULL;
	int pre_cent = 0, now_cent = 0;

	if (!print_only) {
		used_bbt = zmalloc(sizeof(*used_bbt) + dev->flash->nblk * (MAX_LUN_NBYTE));
		if (NULL == used_bbt) {
			printf("%s() malloc bbt failed\n", __func__);
			exit(EXIT_FAILURE);
		}
	}

	/* read static BBT */
	if (shannon_mpt_readbbt(dev, 0))
		return ERR;

	if (skip_dynamic)
		return 0;

	/* read dynamic BBT */
	lun = 0;
	if (!print_only)
		print("Read BBT...%%%02d", now_cent);

next_lun:
	if (test_bit(lun, dev->lun_bitmap))
		goto prepare_next_lun;

	if (print_only)
		printf("dynamic bad blocks lun-%03d phylun-%03d:", lun, log2phy_lun(dev, lun));

	for (page = MPT_DBBT_PAGEOFF; page < dev->flash->npage; page++) {
		for (blk = 0; blk < MPT_MBR_NBLK; blk++) {
			ppa = blk * dev->flash->npage + page;

			// free pre data structure
			if (NULL != preread_req)
				free_request(preread_req);
			if (NULL != cachread_req)
				free_request(cachread_req);

			// preread
			preread_req = alloc_request(dev, sh_preread_cmd, lun, ppa, INDEP_HEAD, 0, 0);
			if (NULL == preread_req)
				malloc_failed_exit();
			if (dev->submit_request(preread_req))
				submit_failed_exit(preread_req->lun);

			// cachread
			cachread_req = alloc_request(dev, sh_cacheread_cmd, lun, ppa, INDEP_HEAD, 0, 1);
			if (NULL == cachread_req)
				malloc_failed_exit();
			if (dev->submit_request(cachread_req))
				submit_failed_exit(cachread_req->lun);

			// run queue
			update_cmdqueue(dev, lun);
			if (poll_cmdqueue(dev, lun))
				poll_failed_exit(lun);

			// check dynamic bbt
			if (0xFE == preread_req->status) {
				if ((MPT_MBR_NBLK - 1) == blk)	// all MBR blocks timeout, maybe this lun is absent
					goto end_this_lun;
				else
					continue;
			} else {
				if (check_req_status_silent(preread_req) || cachread_req->ecc[0] > 251)
					continue;		// lookup next MBR block for this page
			}

			if (251 == cachread_req->ecc[0])
				goto end_this_lun;		// Blank page means dynamic bbt end

			if (cachread_req->metadata[0] != MBR_WATERMARK)
				continue;

			badblk = le16_to_cpu(((__u16 *)cachread_req->data)[0]);
			if (badblk > dev->flash->nblk) {
				printf("FATAL %s(): lun-%03d block-%d page-%03d dynamic badblk %d larger than phyical address!!!\n",
					 __func__, lun, blk, page, badblk);
				break;
			}
			if (print_only)
				printf(" %d", badblk);
			else
				set_bit(lun, used_bbt->sb_bbt[badblk]);
			break;				// have found valid bbt for this page, break block loops
		}
	}

end_this_lun:
	if (print_only)
		printf("\n");
prepare_next_lun:
	lun++;
	if (!print_only) {
		now_cent = 100 * lun / dev->config->luns;
		if (now_cent > pre_cent) {
			print("\b\b\b%%%02d", now_cent);
			pre_cent = now_cent;
		}
	}
	if (lun < dev->config->luns)
		goto next_lun;

	if (!print_only)
		printf("\n");

	free_request(preread_req);
	free_request(cachread_req);

	return 0;
}

static int shannon_mpt_just_scan(struct shannon_dev *dev)
{
	int lun, blk;
	struct shannon_bbt *bbt;

	bbt = zmalloc(sizeof(*bbt) + dev->flash->nblk * MAX_LUN_NBYTE);
	if (NULL == bbt)
		malloc_failed_exit();
	bbt->nblock = dev->flash->nblk;
	dev->config->sector_size_shift = 12;
	dev->config->sector_ncodeword = 1;
	dev->config->ecc_mode = ECCMODE_DISABLE;
	dev->config->raid_mode = RAIDMODE_DISABLE;
	dev->config->nplane = 1;
	if (re_init_device(dev)) {
		printf("Re-init-device (SCAN) failed."MPT_HELP);
		exit(EXIT_FAILURE);
	}
	dev->clear_queue(dev);

	if (flagbyte_scan(dev, bbt)) {
		printf("%s() Flagbyte scan failed."MPT_HELP, __func__);
		exit(EXIT_FAILURE);
	}

	for_dev_each_lun(dev, lun) {
		printf("lun-%03d phylun-%03d bad blocks:", lun, log2phy_lun(dev, lun));
		for_dev_each_block(dev, blk) {
			if (test_bit(lun, bbt->sb_bbt[blk]))
				printf(" %d", blk);
		}
		printf("\n");
	}

	free(bbt);
	return 0;
}

/*-----------------------------------------------------------------------------------------------------------*/
static void shannon_mpt_usage(void)
{
	printf("Usage:\n");
	printf("\tmpt [option]\n\n");

	printf("Option:\n");
	printf("\t-n, --new\n"
		"\t\tFlash is unused\n");
	printf("\t-u, --used\n"
		"\t\tFlash is used, bbt info in MBR block will be reuse\n");
	printf("\t-f, --force\n"
		"\t\tFlash is used, ignore bbt info in MBR block\n");
	printf("\t-o, --scan-whole=loops\n"
		"\t\tSorting the whole flash blocks\n");
	printf("\t-t, --absent-luns=loglun|phylun:x1-x2,x3,x4-x5...\n"
		"\t\tSpecify absent luns\n");
	printf("\t-T, --present-luns=loglun|phylun:x1-x2,x3,x4-x5...\n"
		"\t\tSpecify present luns\n");
	printf("\t-r, --raid-stripes=raid_group_number\n"
		"\t\tSet raid group number\n");
	printf("\t-F, --logfile=FILE\n"
		"\t\tWrite logfile\n");
	printf("\t-V, --mbr-version\n"
		"\t\tMBR version, the default is 0x%X\n", CURRENT_MBR_VERSION);
	printf("\t-W, --manual-writembr=mbrfile,bbtfile\n"
		"\t\tRestore MBR from blocks driver debugfs files mbr and badlun_bitmap. Check plane and lunmap_mode config before do!!!\n");
	printf("\t-i, --sorting-ecc-limit=N or Np\n"
		"\t\tThe accpetable max ECC value for MBR block when sorting. For example: 80p mean max ECC is tmode*80/100\n");
	printf("\t-c, --capacity=CAPACITY\n"
		"\t\tSet user capacity as a specific size(in TB, GB or MB, e.g. 3200GB)\n");
	// printf("\t-b, --bitmap-bbt\n"
	//	"\t\tUse bitmap static bad block bitmap\n\n");
	printf("\t-k, --mbr-4k\n"
		"\t\tWrite MBR info with 4KB\n");
	printf("\t-s, --scan\n"
		"\t\tOnly scan factory bad blocks with single plane for new flash then print them\n");
	printf("\t-M, --mbr\n"
		"\t\tDisplay MBR info\n");
	printf("\t-B[all], --bbt\n"
		"\t\tDisplay BBT info\n");
	printf("\t-z, --user-sector=n\n"
		"\t\tInput user sector size shift\n");
	printf("\t-D, --debug=[R/E/M/B]\n"
		"\t\tPrint some debug info during format process:\n"
		"\t\t  R, print bbt of MBR blocks\n"
		"\t\t  E, print bbt of entire blocks\n"
		"\t\t  M, print mbr info\n"
		"\t\t  B, print bad block map info\n");
	printf("\t-A, --atomic-write\n"
		"\t\tSet atomic_write flag in MBR\n");
	printf("\t-P, --prioritize-write\n"
		"\t\tSet prioritize_write flag in MBR\n");
	printf("\t-e, --performace-adjust=interrupt_delay,dma_max_read_limit,max_outstanding_bios\n"
		"\t\tAdjust performace parameters in MBR. 0 means default value\n");
	printf("\t-R, --record\n"
		"\t\tRecord misc info\n");
	printf("\t-E, --disable-ecc-histogram\n"
		"\t\tDisplay print ecc histogram when do burn-in test\n");
	printf("\t-X, --check-bad-luns=N\n"
		"\t\tExit fail if bad luns larger than this value\n");
	printf("\t-y, --temperature-threshold=controller,flash,board\n"
		"\t\tSet speed-limiting temperature threshold\n");
	printf("\t-h, --help\n"
		"\t\tDisplay this help and exit\n");

	printf("Examples for option '-t' and '-T':\n");
	printf("\t-t loglun:3-6,7,9\n"
		"\t\tlogical luns 3 4 5 6 7 9 are absent\n");
	printf("\t-t phylun:3,5-7,12\n"
		"\t\tphysical luns 3 5 6 7 12 are absent\n");
	printf("\t-T loglun:2,7,10-12\n"
		"\t\tlogical luns 2 7 10 11 12 are present, i.e. other luns are absent\n");
	printf("\tNote: put -t behind -T if you use them simultaneously\n");
}

#define	logout(x...)	do { printf(x); if (NULL != logfp) fprintf(logfp, x); } while (0)

int shannon_mpt(struct shannon_dev *dev, int argc, char **argv)
{	
	struct option longopts [] = {
		{"new", no_argument, NULL, 'n'},
		{"used", no_argument, NULL, 'u'},
		{"force", no_argument, NULL, 'f'},
		{"absent-luns", required_argument, NULL, 't'},
		{"present-luns", required_argument, NULL, 'T'},
#ifndef __RELEASE__
		{"scan", no_argument, NULL, 's'},
		{"mbr", no_argument, NULL, 'M'},
		{"bbt", optional_argument, NULL, 'B'},
		{"user-sector", required_argument, NULL, 'z'},
		{"debug", required_argument, NULL, 'D'},
#endif
		{"raid-stripes", required_argument, NULL, 'r'},
		{"help", no_argument, NULL, 'h'},
		{"scan-whole", required_argument, NULL, 'o'},
		{"logfile", required_argument, NULL, 'F'},
		{"mbr-version", required_argument, NULL, 'V'},
		{"manual-writembr", required_argument, NULL, 'W'},
		{"sorting-ecc-limit", required_argument, NULL, 'i'},
		{"capacity", required_argument, NULL, 'c'},
		{"bitmap-bbt", no_argument, NULL, 'b'},
		{"mbr-4k", no_argument, NULL, 'k'},
		{"atomic-write", no_argument, NULL, 'A'},
		{"prioritize-write", no_argument, NULL, 'P'},
		{"performace-adjust", required_argument, NULL, 'e'},
		{"record", required_argument, NULL, 'R'},
		{"nothing", no_argument, NULL, 'G'},
		{"disable-ecc-histogram", no_argument, NULL, 'E'},
		{"check-bad-luns", required_argument, NULL, 'X'},
		{"temperature-threshold", required_argument, NULL, 'y'},
		{"sorting-prefix-string", required_argument, NULL, 'S'},
		{0, 0, 0, 0},
	};
	int i, j, opt, blk, lun, bbt_size;
	int new, used, force;
	struct shannon_bbt *bbt, *mbr_bbt;
	struct live_context *mbr_context, *bbt_context;
	int debug_mbrblk_bbt, debug_entireblk_bbt, debug_mbr_info, debug_bbt_info;
	int lun_nbadblk[512];
	int scan_whole = 0;

	FILE *logfp = NULL;

	int wm_flag = 0;
	FILE *wm_mfp = NULL, *wm_bfp = NULL;
	char *p, *endptr;

	unsigned long inherent_lun_bitmap[32];
	u32 sensor12, sensor13, sensor14;

	/* pre condition must be satisfied */
	if (ECCMODE_DISABLE == dev->config->ecc_mode) {
		printf("BUG: ECC should be enable with value %d(with scambler) or %d(bypass scambler). Please re-config!\n",
			ECCMODE_ENABLE, ECCMODE_BYPASS_SCRAMBLER_ONLY);
		return ERR;
	}

	if (sizeof(*dev->mbr) > MPT_MBR_SIZE) {
		printf("BUG: MBR size %ldByte is larger than %dByte, this is invalid!\n", sizeof(*dev->mbr), MPT_MBR_SIZE);
		return ERR;
	}

	if (12 != dev->config->sector_size_shift) {
		printf("ERR: mpt should use sector_size 4096\n");
		return ERR;
	}

	/* analyse arguments */
	new = 0;
	used = 0;
	force = 0;
	dev->group_raid_num = 1;

	debug_mbrblk_bbt = 0;
	debug_entireblk_bbt = 0;
	debug_mbr_info = 0;
	debug_bbt_info = 0;

	dev->mbr_version = CURRENT_MBR_VERSION;
	dev->sorting_ecc_limit = (dev->tmode/2 < 6) ? 6 : dev->tmode/2;
	dev->mpt_set_capacity = 0;
	dev->bm_bbt = 0;
	dev->mbr_by_4k = 0;
	dev->prioritize_write = 0;
	dev->atomic_write = 0;

	dev->recordfp = NULL;
	dev->bad_blocks = 0;
	dev->bb_count = NULL;
	dev->scan_loops = 1;
	dev->print_burnin_ecc_histogram = 1;

	dev->max_controller_temp = 0.0;
	dev->max_flash_temp = 0.0;
	dev->max_board_temp = 0.0;

	dev->ctrl_temp_threshold = 0;	// hw default: 93 100
	dev->flash_temp_threshold = 0;	// hw default: 70 80
	dev->board_temp_threshold = 0;	// hw default: 70 80

	dev->check_bad_luns = 65536;
	memset(dev->sorting_print_string, 0x00, sizeof(dev->sorting_print_string));

	/* parse option -t and -T first */
	for (i = 0; i < argc; i++) {
		if (!strncmp(argv[i], "-t", 2) || !strncmp(argv[i], "--absent-luns", 13))
			present_absent_luns(dev, *(argv[i]+2) ? argv[i]+2 : argv[i+1], 1);

		if (!strncmp(argv[i], "-T", 2) || !strncmp(argv[i], "--present-luns", 14))
			present_absent_luns(dev, *(argv[i]+2) ? argv[i]+2 : argv[i+1], 0);
	}

	memcpy(dev->absent_lun_bitmap, dev->lun_bitmap, sizeof(dev->absent_lun_bitmap));
	for_dev_each_lun(dev, lun)
		dev->valid_luns++;
	dev->present_luns = dev->valid_luns;
	dev->absent_luns = dev->config->luns - dev->valid_luns;

	while ((opt = getopt_long(argc, argv, "nufsMB::z:D:F:ht:T:r:o:V:W:i:c:bkAPe:R:EX:y:GS:", longopts, NULL)) != -1) {
		switch (opt) {
		case 'n':
			new = 1;
			break;

		case 'u':
			used = 1;
			break;

		case 'f':
			force = 1;
			break;
		case 's':
			return shannon_mpt_just_scan(dev);

		case 'M':
			return shannon_mpt_readmbr(dev, 0, 1);

		case 'B':
			return shannon_mpt_read_used_bbt(dev, 1, optarg ? 0 : 1);

		case 'z':
			dev->config->user_sector_size_shift = dev->config_bakup->user_sector_size_shift = strtoul(optarg, NULL, 10);
			if (dev->config->user_sector_size_shift > dev->config->sector_size_shift) {
				printf("ERR: user_sector_size > sector_size\n");
				return ERR;
			}
			break;

		case 'D':
			for (i = 0; i < strlen(optarg); i++) {
				switch (optarg[i]) {
				case 'R':
					debug_mbrblk_bbt = 1;
					break;
				case 'E':
					debug_entireblk_bbt = 1;
					break;
				case 'M':
					debug_mbr_info = 1;
					break;
				case 'B':
					debug_bbt_info = 1;
					break;
				default:
					shannon_mpt_usage();
					break;
				}
			}
			break;
		case 'o':
			scan_whole = 1;
			dev->scan_loops = atoi(optarg);
			break;
		case 'c':
			dev->mpt_set_capacity = strtoul(optarg, &endptr, 10);
			if ('m' == *endptr || 'M' == *endptr) {
				dev->mpt_set_capacity *= 1000000ul;
			} else if ('g' == *endptr || 'G' == *endptr) {
				dev->mpt_set_capacity *= 1000000000ul;
			} else {
				printf("capacity should specify unit, e.g. mb MB gb and GB\n");
				exit(EXIT_FAILURE);
			}
			if (!dev->mpt_set_capacity) {
				printf("ERROR input capacity = 0\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 'h':
			shannon_mpt_usage();
			return 0;

		case 't':
			break;

		case 'T':
			break;

		case 'r':
			dev->group_raid_num = atoi(optarg);
			if (dev->group_raid_num <= 0 ||
				dev->group_raid_num > dev->config_bakup->luns / 2 ||
				dev->config_bakup->luns % dev->group_raid_num) {
				printf("Invalid conjunction of: group_raid_num %d and config-luns %d\n",
					dev->group_raid_num, dev->config_bakup->luns);
				exit(EXIT_FAILURE);
			}
			break;
		case 'F':
			logfp = strncmp(optarg, "+", 1) ? fopen(optarg, "w") : fopen(optarg + 1, "a");
			if (NULL == logfp) {
				perror("mpt open logfile error:");
				exit(EXIT_FAILURE);
			} else {
				time_t tt;
				tt = time(NULL);
				fprintf(logfp, "[MPT LOG BEG %s] --> %s", dev->name, ctime(&tt));
			}
			break;
		case 'V':
			dev->mbr_version = strtoul(optarg, NULL, 0x10);
			break;
		case 'W':
			endptr = strchr(optarg, ',');
			if (NULL == endptr) {
				printf("Input valid manual-writembr para: mbrfile,bbtfile\n");
				exit(EXIT_FAILURE);
			}
			*endptr = '\0';

			if ((wm_mfp = fopen(optarg, "r")) == NULL) {
				printf("Open mbr ile fail: %s\n", optarg);
				exit(EXIT_FAILURE);
			}

			if ((wm_bfp = fopen(endptr+1, "r")) == NULL) {
				printf("Open bbt file fail: %s\n", endptr+1);
				exit(EXIT_FAILURE);
			}

			force = 1;
			wm_flag = 1;
			break;
		case 'i':
			dev->sorting_ecc_limit = strtol(optarg, &endptr, 10);
			if (*endptr == 'p')
				dev->sorting_ecc_limit = dev->tmode * dev->sorting_ecc_limit / 100;
			if (dev->sorting_ecc_limit == 0 || dev->sorting_ecc_limit >= dev->tmode) {
				printf("Option -i, --sorting-ecc-limit should between (0, %d), but input is %d\n",
					dev->tmode, dev->sorting_ecc_limit);
				exit(EXIT_FAILURE);
			}
			break;
		case 'b':
			dev->bm_bbt = 0;	/* FIXME: disable */
			break;
		case 'k':
			dev->mbr_by_4k = 1;
			break;
		case 'A':
			dev->atomic_write = 1;
			break;
		case 'P':
			dev->prioritize_write = 1;
			break;
		case 'e':
			p = optarg;
			dev->interrupt_delay = strtol(p, &endptr, 10);
			if (*endptr != ',') {
				printf("1: --performace-adjust=interrupt_delay,dma_max_read_limit,max_outstanding_bios. But your input is %s\n", optarg);
				exit(EXIT_FAILURE);
			}

			p = endptr + 1;
			dev->dma_max_read_limit = strtol(p, &endptr, 10);
			if (*endptr != ',') {
				printf("2: --performace-adjust=interrupt_delay,dma_max_read_limit,max_outstanding_bios. But your input is %s\n", optarg);
				exit(EXIT_FAILURE);
			}

			p = endptr + 1;
			if (!*p) {
				printf("3: --performace-adjust=interrupt_delay,dma_max_read_limit,max_outstanding_bios. But your input is %s\n", optarg);
				exit(EXIT_FAILURE);
			}
			dev->max_outstanding_bios = strtol(p, &endptr, 10);
			if (*endptr) {
				printf("4: --performace-adjust=interrupt_delay,dma_max_read_limit,max_outstanding_bios. But your input is %s\n", optarg);
				exit(EXIT_FAILURE);
			}

			// printf("interrupt_delay=%d dma_max_read_limit=%d max_outstanding_bios=%d\n", dev->interrupt_delay, dev->dma_max_read_limit, dev->max_outstanding_bios);
			break;
		case 'R':
			dev->recordfp = fopen(optarg, "w");
			if (NULL == dev->recordfp) {
				printf("Create record file fail\n");
				exit(EXIT_FAILURE);
			}
		case 'E':
			dev->print_burnin_ecc_histogram = 0;
			break;
		case 'X':
			dev->check_bad_luns = atoi(optarg);
			break;
		case 'y':
			p = optarg;
			dev->ctrl_temp_threshold = strtol(p, &endptr, 10);
			if (*endptr != ',') {
				printf("1, temperature_limit format: controller_temp,flash_temp,board_temp\n");
				exit(EXIT_FAILURE);
			}
			if (dev->ctrl_temp_threshold && (dev->ctrl_temp_threshold < 70 || dev->ctrl_temp_threshold > 100)) {
				printf("controller temperature threshold should between 70 and 100\n");
				exit(EXIT_FAILURE);
			}

			p = endptr + 1;
			dev->flash_temp_threshold = strtol(p, &endptr, 10);
			if (*endptr != ',') {
				printf("2, temperature_limit format: controller_temp,flash_temp,board_temp\n");
				exit(EXIT_FAILURE);
			}
			if (dev->flash_temp_threshold && (dev->flash_temp_threshold < 60 || dev->flash_temp_threshold > 100)) {
				printf("flash temperature threshold should between 60 and 100\n");
				exit(EXIT_FAILURE);
			}

			p = endptr + 1;
			if (!*p) {
				printf("3, temperature_limit format: controller_temp,flash_temp,board_temp\n");
				exit(EXIT_FAILURE);
			}
			dev->board_temp_threshold = strtol(p, &endptr, 10);
			if (dev->board_temp_threshold && (dev->board_temp_threshold < 60 || dev->board_temp_threshold > 100)) {
				printf("board temperature threshold should between 60 and 100\n");
				exit(EXIT_FAILURE);
			}
			// printf("%d %d %d\n", dev->ctrl_temp_threshold, dev->flash_temp_threshold, dev->board_temp_threshold);
			break;
		case 'S':
			dev->sorting_print_string[0] = ' ';
			strcpy(dev->sorting_print_string+1, optarg);
			break;
		case 'G':	// do nothing just for compatible
			break;
		default:
			shannon_mpt_usage();
			return ERR;		
		}
	}

	sensor12 = dev->ioread32(dev, 0x12);
	sensor13 = dev->ioread32(dev, 0x13);
	sensor14 = dev->ioread32(dev, 0x14);
	if (dev->flash_temp_threshold) {
		sensor12 = (sensor12 & ~0xFFFF) | dev->flash_temp_threshold | (dev->flash_temp_threshold << 8);
		sensor13 = (sensor13 & ~0xFFFF) | (dev->flash_temp_threshold + 10) | ((dev->flash_temp_threshold + 10) << 8);
	}
	if (dev->board_temp_threshold) {
		sensor12 = (sensor12 & ~0xFF0000) | (dev->board_temp_threshold << 16);
		sensor13 = (sensor13 & ~0xFF0000) | ((dev->board_temp_threshold + 10)<< 16);
	}
	if (dev->ctrl_temp_threshold) {
		sensor14 = (((dev->ctrl_temp_threshold + 273) * 1024 * 1000 / 503975) & 0x3FF) | \
			((((dev->ctrl_temp_threshold + 7 + 273) * 1024 * 1000 / 503975) & 0x3FF) << 16);
	}
	dev->iowrite32(dev, sensor12, 0x12);
	dev->iowrite32(dev, sensor13, 0x13);
	dev->iowrite32(dev, sensor14, 0x14);

	dev->group_raid_luns = dev->config_bakup->luns / dev->group_raid_num;

	check_ecc_power(dev, dev->hw_config->hw_ecc_power & 0x0F);

	if (0 == (new + used + force)) {
		printf("ERR: option of [-n, --new], [-u, --used] or [-f, --force] must be selected!\n\n");
		shannon_mpt_usage();
		return ERR;
	} else if (1 != (new + used + force)) {
		printf("ERR: option of [-n, --new], [-u, --used] and [-f ,--force] are mutually exclusive!\n\n");
		shannon_mpt_usage();
		return ERR;
	}

	dev->mpt_begintime = time(NULL);

	/* scan MBR blocks */
	mbr_bbt = zmalloc(sizeof(*mbr_bbt) + MPT_MBR_NBLK * MAX_LUN_NBYTE);
	if (NULL == mbr_bbt)
		malloc_failed_exit();
	mbr_bbt->nblock = MPT_MBR_NBLK;

	if (used) {
		shannon_mpt_read_used_bbt(dev, 0, 0);

		for_dev_each_lun(dev, lun) {
			for (blk = 0; blk < MPT_MBR_NBLK; blk++) {
				if (test_bit(lun, used_bbt->sb_bbt[blk]))
					set_bit(lun, mbr_bbt->sb_bbt[blk]);
			}
		}
		check_mbr_bbt(dev, mbr_bbt, "MBR blocks read-used-info bad luns");
	}

	dev->private_data = mbr_bbt;
	mpt_scan_mbr_bbt(dev, mbr_bbt, new);
	if (debug_mbrblk_bbt) {
		for (blk = 0; blk < MPT_MBR_NBLK; blk++) {
			printf("MBR superblock-%d badblock bitmap:", blk);
			for (i = 0; i < (dev->config_bakup->luns + 8 * sizeof(long) - 1) / (8 * sizeof(long)); i++)
				printf(" 0x%lX", mbr_bbt->sb_bbt[blk][i]);
			printf("\n");
		}
	}
	/* XXX From here for_dev_each_lun() will skip invalid luns XXX*/

	/* for record live context */
	mbr_context = alloc_live_context(dev);
	if (NULL == mbr_context)
		malloc_failed_exit();
	sprintf(mbr_context->id, "MBR-LIVE-CONTEXT");

	bbt_context = alloc_live_context(dev);
	if (NULL == bbt_context)
		malloc_failed_exit();
	sprintf(bbt_context->id, "BBT-LIVE-CONTEXT");

	/* SCAN entire flash bbt info should use 'sector=512/bypass ecc/bypass raid/single plane' mode */
	bbt_size = sizeof(*bbt) + dev->flash->nblk * (MAX_LUN_NBYTE);
	bbt = zmalloc(bbt_size);
	if (NULL == bbt)
		malloc_failed_exit();
	bbt->nblock = dev->flash->nblk;
	dev->config->ecc_mode = ECCMODE_DISABLE;
	dev->config->raid_mode = RAIDMODE_DISABLE;
	dev->config->nplane = 1;
	assert(dev->config->sector_size_shift == 12);
	if (re_init_device(dev)) {
		printf("Re-init-device (SCAN) failed."MPT_HELP);
		exit(EXIT_FAILURE);
	}
	dev->clear_queue(dev);

	if (wm_flag)
		goto manual_writembr;

	if (used || force) {
		if (erase_scan(dev, bbt))
			exitlog("Erase scan failed\n");

		check_all_bbt(dev, bbt, "All blocks erase check bad luns");
	}

	if (flagbyte_scan(dev, bbt))
		exitlog("Flagbyte scan failed\n");
	check_all_bbt(dev, bbt, "All blocks flagbyte check bad luns");

	if (used) {
		for_dev_each_lun(dev, lun) {
			for (blk = MPT_MBR_NBLK; blk < dev->flash->nblk; blk++) {
				if (test_bit(lun, used_bbt->sb_bbt[blk]))
					set_bit(lun, bbt->sb_bbt[blk]);
			}
		}
	}
	check_all_bbt(dev, bbt, "All blocks read-used-info check bad luns");

	// alloc bb_count
	if (dev->bb_count == NULL) {
		dev->bb_count = zmalloc(dev->config->luns * sizeof(unsigned int));

		if (dev->bb_count == NULL) {
			printf("Allocation in %s() failed.", __func__);
			exit(EXIT_FAILURE);
		}
	}
	for_dev_each_lun(dev, lun) {
		for (blk = 0; blk < bbt->nblock; blk++) {
			if (test_bit(lun, bbt->sb_bbt[blk]))
				dev->bb_count[lun]++;
		}
	}

	ecc_histogram = zmalloc(sizeof(u64) * (dev->tmode + 1));
	if (NULL == ecc_histogram)
		malloc_failed_exit();

	if (scan_whole) {
		time_t now = time(NULL);
		for (dev->loops = 1; dev->loops <= dev->scan_loops; dev->loops++) {
			print("ALL blocks sorting loop %d/%d...", dev->loops, dev->scan_loops);
			mpt_scan_bbt_advance(dev, bbt);

			if (dev->print_burnin_ecc_histogram) {
				for (j = 0; j <= dev->tmode; j++)
					printf("%d\t%lu\n", j, ecc_histogram[j]);
			}

#if 0
			if (dev->max_controller_temp - 1.0*dev->ctrl_temp_threshold >= 1.0 ||
				dev->max_flash_temp - 1.0*dev->flash_temp_threshold >= 1.0 ||
				dev->max_board_temp - 1.0*dev->board_temp_threshold >= 1.0) {
				dev->exitcode = TEMPERATURE_LIMIT;
				printf("temperature larger than threshold, exit init loop\n");
				break;
			}
#endif
		}
		printf("Sorting took %ld seconds\n", time(NULL) - now);

		for_dev_each_lun(dev, lun) {
			for (blk = 0; blk < MPT_MBR_NBLK; blk++) {
				if (test_bit(lun, bbt->sb_bbt[blk]))
					set_bit(lun, mbr_bbt->sb_bbt[blk]);
			}
		}
	} else {
		time_t now = time(NULL);
		for (dev->loops = 1; dev->loops <= dev->scan_loops; dev->loops++) {
			print("MBR blocks sorting loop %d...", dev->loops);
			mpt_scan_bbt_advance(dev, mbr_bbt);
		}
		printf("Sorting took %ld seconds\n", time(NULL) - now);
	}

	if (NULL != dev->recordfp) {
		fprintf(dev->recordfp, "dynamic_bad_blocks %d\n", dev->bad_blocks);
		fprintf(dev->recordfp, "max_controller_temp %.0f\n", dev->max_controller_temp);
		fprintf(dev->recordfp, "max_flash_temp %.0f\n", dev->max_flash_temp);
		fprintf(dev->recordfp, "max_board_temp %.0f\n", dev->max_board_temp);
	}
	free(ecc_histogram);

	if (dev->config->nplane != dev->config_bakup->nplane) {
		// need to convert bbt from single plane to configured plane
		bbt_size = sizeof(*bbt) + (dev->flash->nblk / dev->config_bakup->nplane) * (MAX_LUN_NBYTE);
		new_bbt = zmalloc(bbt_size);
		if (NULL == new_bbt)
			malloc_failed_exit();
		new_bbt->nblock = dev->flash->nblk;
		for_dev_each_lun(dev, lun) {
			for (blk = MPT_MBR_NBLK; blk < dev->flash->nblk; blk++) {
				if (test_bit(lun, bbt->sb_bbt[blk]))
					set_bit(lun, new_bbt->sb_bbt[blk / dev->config_bakup->nplane]);
			}
		}
		free(bbt);
		bbt = new_bbt;
		/* re-init device */
		dev->config->sector_size_shift = dev->config_bakup->sector_size_shift;
		dev->config->sector_ncodeword = dev->config_bakup->sector_ncodeword;
		dev->config->ecc_mode = dev->config_bakup->ecc_mode;
		dev->config->raid_mode = dev->config_bakup->raid_mode;
		dev->config->nplane = dev->config_bakup->nplane;
		if (re_init_device(dev)) {
			printf("Re-init-device %s() failed."MPT_HELP, __func__);
			exit(EXIT_FAILURE);
		}
		dev->clear_queue(dev);
	}

	/* delete luns which have too many invalid blocks or MBR blocks are all invalid */
	for (lun = 0; lun < dev->config->luns; lun++)
		lun_nbadblk[lun] = dev->flash->nblk;	//XXX: for_dev_each_lun() will skip bad lun marked in lun_bitmap

	for_dev_each_lun(dev, lun) {
		lun_nbadblk[lun] = 0;

		for_dev_each_block(dev, blk) {
			if (test_bit(lun, bbt->sb_bbt[blk]))
				lun_nbadblk[lun]++;
		}
		lun_nbadblk[lun] *= dev->config_bakup->nplane;

		if (lun_nbadblk[lun] > MAX_BADBLK_COUNT
			|| (lun_nbadblk[lun] - dev->targetlun[lun].blk_hole_count) > MAX_BAD_BLOCK_IN_A_LUN) {
			dev->valid_luns--;
			set_bit(lun, dev->lun_bitmap);
		}
	}

	memset(inherent_lun_bitmap, 0x00, sizeof(inherent_lun_bitmap));
	if (0 != dev->inherent_mbr->lun_amount) {
		if (dev->config->nchannel != dev->inherent_mbr->cfg_nchannel ||
			dev->config->nthread != dev->inherent_mbr->cfg_nthread ||
			dev->config->nlun != dev->inherent_mbr->cfg_nlun) {
			printf("\033[0;1;31m[WARNNIG] Inherent MBR mismatch current config!!! \033[0m");
			printf("Inherent: nchannel=%d nthread=%d nlun=%d; ",
				dev->inherent_mbr->cfg_nchannel, dev->inherent_mbr->cfg_nthread, dev->inherent_mbr->cfg_nlun);
			printf("Curconfig: nchannel=%d nthread=%d nlun=%d\n",
				dev->config->nchannel, dev->config->nthread, dev->config->nlun);
		} else {
			for (lun = 0; lun < dev->config->luns; lun++) {
				if (test_bit(log2phy_lun(dev, lun), (unsigned long *)dev->inherent_mbr->bad_phy_lun_map)) {
					if (!test_bit(lun, dev->lun_bitmap)) {
						set_bit(lun, inherent_lun_bitmap);
						dev->valid_luns--;
					}
					set_bit(lun, dev->lun_bitmap);
				}
			}
		}
	}

	if (dev->valid_luns < (dev->config->luns - dev->absent_luns)) {
		char *p_input, buf_input[32];
		logout("Invalid luns (%d) have been detected: ", (dev->config->luns - dev->absent_luns) - dev->valid_luns);

		for (lun = 0; lun < dev->config->luns; lun++) {
			if (test_bit(lun, dev->lun_bitmap) && !test_bit(lun, dev->absent_lun_bitmap)) {
				printf(" \033[0;1;31m%d\033[0m[%d %.2f%%%s]",
					lun, lun_nbadblk[lun], lun_nbadblk[lun] * 100.0 / dev->flash->nblk,
					test_bit(lun, inherent_lun_bitmap) ? "*" : "");
				if (NULL != logfp)
					fprintf(logfp, " %d[%d %.2f%%%s]",
						lun, lun_nbadblk[lun], lun_nbadblk[lun] * 100.0 / dev->flash->nblk,
						test_bit(lun, inherent_lun_bitmap) ? "*" : "");
			}
		}
		logout(" nblk=%d\n", dev->flash->nblk);
		print("Do you want continue format this device anyway? [y/n]");

		fgets(buf_input, sizeof(buf_input), stdin);
		p_input = buf_input;
		while (isblank(*p_input)) p_input++;
		if ('y' != tolower(p_input[0]))
			exitlog("Input not y to exit\n");
	}

	for (lun = 0; lun < dev->config->luns; lun++) {
		if (test_bit(lun, dev->lun_bitmap)) {
			for_dev_each_block(dev, blk)
				set_bit(lun, bbt->sb_bbt[blk]);
		}
	}

	if (debug_entireblk_bbt) {
		for_dev_each_block(dev, blk) {
			printf("Superblock-%-4d badblock bitmap:", blk);
			for (i = 0; i < (dev->config_bakup->luns + 8 * sizeof(long) - 1) / (8 * sizeof(long)); i++)
				printf(" 0x%lX", bbt->sb_bbt[blk][i]);
			printf("\n");
		}
	}

	mpt_build_sb_luninfo(dev, bbt);

	/* MBR write should use 'sector=512/enable ecc/bypass raid/single plane' mode */
manual_writembr:
	if (!wm_flag) {
		mpt_setvalue_mbr(dev, dev->mbr);
	} else {
		mpt_setvalue_mbr_from_file(dev, dev->mbr, wm_mfp);
		mpt_setbbt_from_file(dev, bbt, wm_bfp);
	}
	if (debug_mbr_info)
		mpt_print_mbr(dev, dev->mbr);

	if (dev->mbr_by_4k) {
		dev->config->sector_size_shift = dev->config_bakup->sector_size_shift;
		dev->config->sector_ncodeword = dev->config_bakup->sector_ncodeword;
	} else {
		dev->config->sector_size_shift = 9;
		dev->config->sector_ncodeword = 1;
	}
	dev->config->ecc_mode = ECCMODE_ENABLE;		//XXX: MBR write should enable ecc and scambler. BBT write use
	dev->config->raid_mode = RAIDMODE_DISABLE;	//     configed value which will enable ecc with or bypass scrambler
	dev->config->nplane = 1;
	if (re_init_device(dev)) {
		printf("Re-init-device (WRITE-MBR) failed."MPT_HELP);
		exit(EXIT_FAILURE);
	}
	dev->clear_queue(dev);
	mpt_write_mbr(dev, dev->mbr);
	record_live_context(mbr_context);

	/* BBT write should use configuration of what is configed but single plane */
	memcpy(dev->config, dev->config_bakup, sizeof(*dev->config));
	dev->config->nplane = 1;
	dev->config->raid_mode = RAIDMODE_DISABLE;
	assert(dev->config->sector_size_shift == 12);
	if (re_init_device(dev)) {
		printf("Re-init-device (WRITE-BBT) failed."MPT_HELP);
		exit(EXIT_FAILURE);
	}
	dev->clear_queue(dev);
	dev->private_int = debug_bbt_info;
	mpt_write_bbt(dev, bbt);
	record_live_context(bbt_context);

	/* statistics invalid blocks + invalid lun */
	for (lun = 0; lun < dev->config->luns; lun++) {
		if (test_bit(lun, dev->lun_bitmap) && !test_bit(lun, dev->absent_lun_bitmap))
			dev->invalid_blocks += dev->flash->nblk;
	}

	/* read and check MBR and BBT */
	dev->private_int = 0;
	dev->private_data = mbr_bbt;
	if (shannon_mpt_readmbr(dev, 0, 0)) {
		printf("BUG: Read MBR failed after mpt."MPT_HELP);
		return ERR;
	}
	if (shannon_mpt_readbbt(dev, 1)) {
		printf("BUG: Read BBT failed after mpt."MPT_HELP);
		return ERR;
	}

	if (dev->private_int)
		exitlog("BUG: Read MBR and BBT is mismatch with writed MBR and BBT.\n");


	logout("mbr-version: 0x%04lX\n", dev->mbr->mbr_version);
#ifndef __RELEASE__
	logout("luns: %d = %d(channel) x %d(thread) x %d(lun)\n",
		dev->mbr->lun_amount, dev->mbr->cfg_nchannel, dev->mbr->cfg_nthread, dev->mbr->cfg_nlun);
#endif
	logout("sector: %dB\n", 1 << dev->mbr->logicb_shift);
#ifndef __RELEASE__
	logout("codeword/sector: %d\n", dev->mbr->ecc_codewords_in_logicb);
#endif
	logout("capacity: %ld MB, %ld GiB\n", dev->mbr->capacity >> 11, (dev->mbr->capacity * 512)/1000000000ul);
#ifndef __RELEASE__
	logout("static-invalid-blocks: %d/%d (%%%.3f)\n", dev->invalid_blocks, (dev->config->luns - dev->absent_luns) * dev->flash->nblk,
		(100.0 * dev->invalid_blocks) / (dev->config->luns * dev->flash->nblk));
#endif
	/* exit */
	free(bbt);
	free_live_context(mbr_context);
	free_live_context(bbt_context);
	free(mbr_bbt);
	if (NULL != used_bbt) free(used_bbt);
	if (NULL != logfp) {
		time_t tt;
		tt = time(NULL);
		fprintf(logfp, "[MPT LOG END %s] <-- %s", dev->name, ctime(&tt));
		fclose(logfp);
	}
	if (NULL != dev->recordfp)
		fclose(dev->recordfp);

#if 0
	if (dev->exitcode == TEMPERATURE_LIMIT)
		exitlog("temperature larger threshold: %3.2f,%3.2f,%3.2f > %d,%d,%d. done-initloops:%d\n",
			dev->max_controller_temp, dev->max_flash_temp, dev->max_board_temp,
			dev->ctrl_temp_threshold, dev->flash_temp_threshold, dev->board_temp_threshold,
			dev->loops);
#endif

	return 0;
}

/*-----------------------------------------------------------------------------------------------------------*/
