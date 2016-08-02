#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>

#include "tool.h"

#define	PARSE_MEMBER(type, member, member_str, name, pv)			\
		do {								\
			int value;						\
										\
			if (pv[0] == '0' && (pv[1] == 'x' || pv[1] == 'X'))	\
				value = strtoul(pv, NULL, 16);			\
			else							\
				value = strtoul(pv, NULL, 10);			\
										\
			if (!strcmp(name, member_str)) {			\
				type->member = value;				\
				return 0;					\
			} 							\
		} while (0)

/*--------------------------------------------------------------------------------------------------------------------------*/
/*
 * get flash name from flash lib file according to flash id
 */
static int get_flashlib_section(union flash_id fid, char *section)
{
	FILE *fp;
	char *p, *endptr, line[256];
	union flash_id lib_fid;
	int i;

	if ((fp = fopen("flash", "r")) == NULL) {
		printf("%s(): open flash lib file failed\n", __func__);
		exit(1);
	}

	while (fgets(line, sizeof(line), fp) != NULL) {
		if ('#' == line[0] || '\r' == line[0] || '\n' == line[0])
			continue;

		p = line;
		while (*p != '\r' && *p != '\n') p++;
		*p = '\0';

		if ('[' != line[0])
			continue;

		strcpy(section, line);

		fgets(line, sizeof(line), fp);
		if (strncmp(line, "id=", 3)) {
			printf("Invalid flash lib format: %s\n", section);
			exit(EXIT_FAILURE);
		}

		p = line;
		while (*p != '=') p++;
		p++;

		for (i = 0; i < 8; i++) {
			lib_fid.byteid[i] = strtoul(p, &endptr, 0x10);
			p = endptr + 1;
		}
		if (lib_fid.longid == fid.longid)
			return 0;
	}

	strcpy(section, "unknown");
	fclose(fp);
	return ERR;
}

static int flashfile_member(struct shannon_dev *dev, struct usr_flash *flash, char *name, char *pv)
{
	PARSE_MEMBER(flash, nblk, "blk_num", name, pv);
	PARSE_MEMBER(flash, npage, "page_num", name, pv);
	PARSE_MEMBER(flash, page_size_shift, "page_size_shift", name, pv);
	PARSE_MEMBER(flash, oob_size, "oob_size", name, pv);
	PARSE_MEMBER(flash, lun_mask, "lun_mask", name, pv);
	PARSE_MEMBER(flash, nplane, "plane_num", name, pv);
	PARSE_MEMBER(flash, plane_mask, "plane_mask", name, pv);

	if (!strcmp(name, "ifmode")) {
		if (!strncmp(pv, "async", 5))
			flash->ifmode = FLASH_IFMODE_ASYNC;
		else if (!strncmp(pv, "sync", 4))
			flash->ifmode = FLASH_IFMODE_SYNC;
		else if (!strncmp(pv, "toggle", 6))
			flash->ifmode = FLASH_IFMODE_TOGGLE;
		else
			goto out;
		return 0;
	}

	if (!strcmp(name, "factory_ivb")) {
		int i;
		char *ph, *pe = pv;

		for (i = 0; i < 8; i++)
			flash->factory_ivb[i].row = -1;

		i = 0;
		while (*pe != '\r' && *pe != '\n') {
			if (*pe != '[')
				goto out;
			pe++;

			/* row */
			ph = pe;
			pe = strchr(ph, ',');
			if (NULL == pe)
				goto out;
			*pe++ = '\0';
			flash->factory_ivb[i].row = strtoul(ph, NULL, 10);

			/* lo_col */
			ph = pe;
			pe = strchr(ph, ',');
			if (NULL == pe)
				goto out;
			*pe++ = '\0';
			flash->factory_ivb[i].lo_col = strtoul(ph, NULL, 10) * (dev->iowidth - dev->valid_8bit);

			/* hi_col */
			ph = pe;
			pe = strchr(ph, ']');
			if (NULL == pe)
				goto out;
			*pe++ = '\0';
			flash->factory_ivb[i].hi_col = (strtoul(ph, NULL, 10) + 1) * (dev->iowidth - dev->valid_8bit) - 1;

			assert(flash->factory_ivb[i].hi_col >= flash->factory_ivb[i].lo_col);

			/* next */
			while (isblank(*pe)) pe++;
			i++;
		}

		/* debug */
		// for (i = 0; flash->factory_ivb[i].row != -1; i++)
		//	printf("row = %d, col = %d %d\n", flash->factory_ivb[i].row,
		//		flash->factory_ivb[i].lo_col, flash->factory_ivb[i].hi_col);

		return 0;
	}

	if (!strcmp(name, "drvmode")) {
		int i;
		char *endptr, *p = pv;

		bzero(flash->drvsetting.data, sizeof(flash->drvsetting.data));

		if (!strncmp(p, "None", 4))
			return 0;

		if (*p != '[')
			goto out;
		p++;

		for (i = 0; i < sizeof(flash->drvsetting.data); i++){
			flash->drvsetting.data[i] = strtoul(p, &endptr, 16);
			flash->drvsetting.datanum++;

			if (*endptr == ',') {
				p = endptr + 1;
				continue;
			} else if (*endptr == ']') {
				break;
			} else {
				goto out;
			}
		}
		assert(flash->drvsetting.datanum > 2);

		/* debug */
		/* printf("%s drvsetting [%d]:", flash->name, flash->drvsetting.datanum);
		for (i = 0; i < flash->drvsetting.datanum; i++)
			printf(" 0x%02X", flash->drvsetting.data[i]);
		printf("\n"); */

		return 0;
	}

	if (!strcmp(name, "id")) {
		int i;
		char *p, *endptr;

		p = pv;
		for (i = 0; i < 8; i++) {
			flash->id.byteid[i] = strtoul(p, &endptr, 0x10);
			p = endptr + 1;
		}

		flash->vendor_id = flash->id.byteid[0];
		flash->device_id = flash->id.byteid[1];
		flash->feature_id = flash->id.byteid[2];

		return 0;
	}

out:
	printf("can`t parse flash config: %s\n", name);
	return ERR;
}

/*
 * parse flash details member according to flash section name recorded in flash->name
 */
static int details_flash(struct shannon_dev *dev, struct usr_flash *flash)
{
	int rc = ERR;
	FILE *fp;
	char line[256], *p, *pm, *pv;

	if ((fp = fopen("flash", "r")) == NULL) {
		printf("%s(): open flash lib file failed\n", __func__);
		exit(rc);
	}

	while (fgets(line, sizeof(line), fp) != NULL) {
		if ('#' == line[0] || '\r' == line[0] || '\n' == line[0])
			continue;

		p = line;
		while (*p != '\r' && *p != '\n') p++;
		*p = '\0';

		if (strcmp(flash->name, line))
			continue;

		/* found section */
		while (fgets(line, sizeof(line), fp) != NULL) {
			if ('#' == line[0] || '\r' == line[0] || '\n' == line[0])
				continue;

			if ('[' == line[0]) {
				rc = 0;
				goto end;
			}

			pm = pv = line;
			while (*pv != '=') pv++;
			*pv='\0';
			pv++;

			if (flashfile_member(dev, flash, pm, pv)) {
				rc = ERR;
				goto end;
			}
		}
		rc = 0;		/* end file will reach here*/
	}

end:
	flash->page_size_shift += (dev->iowidth - 1 - dev->valid_8bit);
	flash->oob_size *= (dev->iowidth - dev->valid_8bit);

	flash->page_size =  1 << flash->page_size_shift;
	flash->entire_page_size = flash->page_size + flash->oob_size;

	/* whether user input blocks number or not */
	if (dev->fblocks) {
		if (dev->fblocks > flash->nblk) {
			printf("User input flash block number larger than physical blocks\n");
			fclose(fp);
			return ERR;
		}
		flash->nblk = dev->fblocks;
	}

	fclose(fp);
	return rc;
}

int parse_flash(struct shannon_dev *dev)
{
	int lun, phythread, off, timeout;
	__u64 status;
	union flash_id fid[256];
	struct sh_reset sh_reset;
	struct sh_readid sh_readid;
	int found_id = 0;

	assert(0 != dev->fd);
	assert(NULL != dev->phythread_mem);

	memset(fid, 0x5A, sizeof(fid));

	for (lun = 0; lun < dev->config->luns; lun++) {	// logical lun based on all luns are exist
		phythread = get_phychannel(dev, lun) * dev->hw_nthread + get_phythread(dev, lun);
		off = 16 * get_phylun(dev, lun);

		dev->iowrite32(dev, U64_LOW_32(dev->phythread_mem[phythread].dma_addr),
				dev->lunreg_dwoff + phythread * dev->lunreg_dwsize + HW_cmdq_pte_lo);
		dev->iowrite32(dev, U64_HIGH_32(dev->phythread_mem[phythread].dma_addr),
				dev->lunreg_dwoff + phythread * dev->lunreg_dwsize + HW_cmdq_pte_hi);

		dev->iowrite32(dev, U64_LOW_32(dev->phythread_mem[phythread].dma_addr + PAGE_SIZE),
				dev->lunreg_dwoff + phythread * dev->lunreg_dwsize + HW_cmpq_pte_lo);
		dev->iowrite32(dev, U64_HIGH_32(dev->phythread_mem[phythread].dma_addr + PAGE_SIZE),
				dev->lunreg_dwoff + phythread * dev->lunreg_dwsize + HW_cmpq_pte_hi);
		/* reset */
		memset(&sh_reset, 0x00, sizeof(sh_reset));
		sh_reset.opcode = sh_writereg_cmd;
		sh_reset.rsv[5] = 0xFF;
		sh_reset.lun = log2phy_lun(dev, lun);

		dev->write_mem(dev, dev->phythread_mem[phythread].kernel_addr + off, &sh_reset, 8);
		dev->iowrite32(dev, off + 8, dev->lunreg_dwoff + phythread * dev->lunreg_dwsize + HW_cmdq_head);

		timeout = 0;
		while (dev->ioread32(dev, dev->lunreg_dwoff + phythread * dev->lunreg_dwsize + HW_cmpq_head) != off + 8) {
			usleep(100);
			if (timeout++ > RUNCMDQ_US_TIMEOUT) {
				memset(&fid[lun], 0xAA, 8);
				break;
			}
		}
		if (timeout > RUNCMDQ_US_TIMEOUT)
			continue;

		dev->read_mem(dev, &status, dev->phythread_mem[phythread].kernel_addr + PAGE_SIZE + off, sizeof(status));
		le64_to_cpus(&status);
		if (!check_status(dev, sh_reset_cmd, status)) {
			memset(&fid[lun], 0xBB, 7);
			fid[lun].byteid[7] = (__u8)status;
			continue;
		}
		if (found_id)
			continue;

		/* read id */
		memset(&sh_readid, 0x00, sizeof(sh_readid));
		sh_readid.opcode = sh_readid_cmd;
		sh_readid.addr = 0x00;
		sh_readid.cmd = 0x90;
		sh_readid.nbyte = 8;
		sh_readid.lun = log2phy_lun(dev, lun);

		dev->write_mem(dev, dev->phythread_mem[phythread].kernel_addr + off + 8, &sh_readid, 8);
		dev->iowrite32(dev, off + 16, dev->lunreg_dwoff + phythread * dev->lunreg_dwsize + HW_cmdq_head);

		timeout = 0;
		while (dev->ioread32(dev, dev->lunreg_dwoff + phythread * dev->lunreg_dwsize + HW_cmpq_head) != off + 16) {
			usleep(1);
			if (timeout++ > RUNCMDQ_US_TIMEOUT) {
				memset(&fid[lun], 0xCC, 8);
				break;
			}
		}
		if (timeout > RUNCMDQ_US_TIMEOUT)
			continue;

		/* read completion queue including flash id */
		dev->read_mem(dev, &fid[lun], dev->phythread_mem[phythread].kernel_addr + PAGE_SIZE + off + 8, 8);

		/* do match using id in flash lib file */
		if (!get_flashlib_section(fid[lun], dev->flash->name))
			found_id = 1;
	}

	if (!found_id) {
		printf("ERR: all lun invalid! (AA, reset timeout; BB, reset status failed recorded in last ID; CC, readid timeout; 5A, software bug)\n");
		for (lun = 0; lun < dev->config->luns; lun++) {
			printf("phylun=%03d hwchannel-%02d hwthread-%02d hwlun-%02d: ",
				log2phy_lun(dev, lun),
				get_phychannel(dev, lun),
				get_phythread(dev, lun),
				get_phylun(dev, lun));
				pr_u8_array_noprefix(&fid[lun], 8, 8);
		}
		return ERR;
	}

	/* if found matched flash, parse details flash member */
	if (details_flash(dev, dev->flash))
		return ERR;

	return 0;
}

int flash_info(struct shannon_dev *dev, union flash_id fid, struct usr_flash *flash)
{
	get_flashlib_section(fid, flash->name);

	if (!strcmp(flash->name, "unknown"))
		return 1;

	if (details_flash(dev, flash))
		return 1;

	return 0;
}

/*--------------------------------------------------------------------------------------------------------------------------*/
static int ncw_m13_8b[][2] = {
	{512, 1}, {512, 2},
	{1024, 2},
	{2048, 3}, {2048, 4},
	{4096, 5}, {4096, 8}, {4096, 15},
	{4096, 1}, /* just used for ECC disable */
};

static int ncw_m13_16b[][2] = {
	{512, 1},
	{4096, 5}, {4096, 8},
	{4096, 1}, /* just used for ECC disable */
};

static int ncw_m14_16b[][2] = {
	{512, 1},
	{4096, 3}, {4096, 4}, {4096, 5}, {4096, 8},
	{4096, 1}, /* just used for ECC disable */
};

static int get_codeword_size_fromhw(struct shannon_dev *dev, int sector_size, int sector_ncodeword)
{
	dev->iowrite32(dev, (dev->hw_config->hw_full_sector_nbyte << 16) | (sector_ncodeword << 8) | (sector_size / 512), 0xC1);
	return dev->ioread32(dev, 0xC4) >> 16;
}

static int optimal_ncodeword(struct shannon_dev *dev, int sector_size)
{
	int i, codeword_size, page_nsector;
	int (*ncw)[2], ncw_cnt;

	if (dev->iowidth == 1 && dev->mmode == 13) {
		ncw = ncw_m13_8b;
		ncw_cnt = sizeof(ncw_m13_8b) / (2 * sizeof(int));
	} else if (dev->iowidth == 2 && dev->mmode == 13) {
		ncw = ncw_m13_16b;
		ncw_cnt = sizeof(ncw_m13_16b) / (2 * sizeof(int));
	} else if (dev->iowidth == 2 && dev->mmode == 14) {
		ncw = ncw_m14_16b;
		ncw_cnt = sizeof(ncw_m14_16b) / (2 * sizeof(int));
	} else {
		printf("%s(): Invalid iowidth %d and mmode %d\n", __func__, dev->iowidth, dev->mmode);
		exit(EXIT_FAILURE);
	}

	/* search max page_nsector */
	dev->config->optimal_page_nsector = 0;

	for (i = 0; i < ncw_cnt; i++) {
		if (sector_size != ncw[i][0])
			continue;

		codeword_size = get_codeword_size_fromhw(dev, sector_size, ncw[i][1]);
		page_nsector = dev->flash->entire_page_size / (codeword_size * ncw[i][1]);

		if (page_nsector >= dev->flash->page_size / sector_size) {
			dev->config->optimal_page_nsector = dev->flash->page_size / sector_size;
			break;
		}

		if (page_nsector > dev->config->optimal_page_nsector)
			dev->config->optimal_page_nsector = page_nsector;
	}

	/* search max sector_ncodeword based on optimal_page_nsector */
	dev->config->optimal_ncodeword = 0;

	for (i = 0; i < ncw_cnt; i++) {
		if (sector_size != ncw[i][0])
			continue;

		codeword_size = get_codeword_size_fromhw(dev, sector_size, ncw[i][1]);
		if (dev->config->optimal_page_nsector * codeword_size * ncw[i][1] > dev->flash->entire_page_size)
			continue;

		if (ncw[i][1] > dev->config->optimal_ncodeword) {
			dev->config->optimal_ncodeword = ncw[i][1];
			dev->config->optimal_codeword_size = codeword_size;
		}
	}

	if (0 == dev->config->optimal_page_nsector || 0 == dev->config->optimal_ncodeword || 0 == dev->config->optimal_codeword_size) {
		printf("FATAL: %s() optimal_page_nsector=%d optimal_ncodeword=%d\n",
			__func__, dev->config->optimal_page_nsector, dev->config->optimal_ncodeword);
		exit(EXIT_FAILURE);
	}

	// printf("flash: page_size=%d oob_size=%d sector_size=%d optimal_ncodeword=%d optimal_codeword_size=%d page_nsector=%d\n",
	//	dev->flash->page_size, dev->flash->oob_size, sector_size,
	//	dev->config->optimal_ncodeword, dev->config->optimal_codeword_size, dev->config->optimal_page_nsector);

	return dev->config->optimal_ncodeword;
}

#define	GET_MEMBER(type, member)				\
		printf(""#member" = %d\n", type->member);	\

#define	SET_MEMBER(type, member, def, append, fp)			\
	do {								\
		char *p, buf[256];					\
		printf("Input "#member"%s [default %d]: ", append, def);\
		fflush(stdout);						\
		fgets(buf, sizeof(buf), stdin);				\
		p = buf;						\
		while (isblank(*p)) p++;				\
		if (*p == '\n' || *p == '\r')				\
			type->member = def;				\
		else							\
			type->member = strtoul(p, NULL, 10);		\
									\
		if (NULL != fp)						\
			fprintf(fp, ""#member"=%d\n", type->member);	\
	} while (0)

#define	NONE	""

/*
 * calculate indirect config member
 */
int calculate_indirect_config(struct shannon_dev *dev)
{
	int i;
	int (*ncw)[2], ncw_cnt;

	if (dev->config->nchannel > dev->hw_nchannel ||
		dev->config->nthread > dev->hw_nthread ||
		dev->config->nlun > dev->hw_nlun) {
		printf("ERR: your config channel/thread/lun large than hardware supported!\n");
		return ERR;
	}

	dev->config->threads = dev->config->nchannel * dev->config->nthread;
	dev->config->luns = dev->config->threads * dev->config->nlun;
	dev->config->sector_size =  1 << dev->config->sector_size_shift;

	dev->config->drvmode = dev->flash->drvsetting.data[2];

	/* get codeword_size from hw */
	if (dev->iowidth == 1 && dev->mmode == 13) {
		ncw = ncw_m13_8b;
		ncw_cnt = sizeof(ncw_m13_8b) / (2 * sizeof(int));
	} else if (dev->iowidth == 2 && dev->mmode == 13) {
		ncw = ncw_m13_16b;
		ncw_cnt = sizeof(ncw_m13_16b) / (2 * sizeof(int));
	} else if (dev->iowidth == 2 && dev->mmode == 14) {
		ncw = ncw_m14_16b;
		ncw_cnt = sizeof(ncw_m14_16b) / (2 * sizeof(int));
	} else {
		printf("%s(): Invalid iowidth %d and mmode %d\n", __func__, dev->iowidth, dev->mmode);
		exit(EXIT_FAILURE);
	}

	for (i = 0; i < ncw_cnt; i++) {
		if (dev->config->sector_size == ncw[i][0] && dev->config->sector_ncodeword == ncw[i][1])
			break;
	}
	if (i >= ncw_cnt) {
		printf("### Invalid sector_size and sector_ncodeword ###\n");
		return 1;
	}

	if (dev->config->sector_size == 4096 && dev->config->sector_ncodeword == 1)
		assert(dev->config->ecc_mode == ECCMODE_DISABLE);

	dev->iowrite32(dev, (dev->hw_config->hw_full_sector_nbyte << 16) |
				(dev->config->sector_ncodeword << 8) |
				(dev->config->sector_size / 512), 0xC1);
	dev->config->codeword_size = dev->ioread32(dev, 0xC4) >> 16;
	// printf("codeword_size=%d\n", dev->config->codeword_size);

	if (ECCMODE_DISABLE == dev->config->ecc_mode) {			   /* bypass ecc/crc/scrambler */
		dev->config->codeword_size = dev->config->sector_size + 8; /* metadata size */
		dev->config->sector_ncodeword = 1;
	}

	/* calculate page_nsector and chunk_nsector according flash */
	dev->config->full_sector_size = dev->config->sector_ncodeword * dev->config->codeword_size;

	dev->config->page_nsector = dev->flash->entire_page_size / (dev->config->sector_ncodeword * dev->config->codeword_size);
	dev->config->chunk_nsector = dev->config->page_nsector * dev->config->nplane;

	dev->config->ndata = dev->config->page_nsector * dev->config->sector_size;
	dev->config->nmeta = dev->config->page_nsector * METADATA_SIZE;
	dev->config->chunk_ndata = dev->config->nplane * dev->config->ndata;
	dev->config->chunk_nmeta = dev->config->nplane * dev->config->nmeta;

	return 0;
}

#ifndef __RELEASE__
static void interactive_config(struct shannon_dev *dev, FILE *fp)
{
	int def_ifmode;

	switch (dev->flash->ifmode) {
	case FLASH_IFMODE_ASYNC:
		def_ifmode = IFMODE_ASYNC;
		break;
	case FLASH_IFMODE_SYNC:
		def_ifmode = IFMODE_SYNC;
		break;
	case FLASH_IFMODE_TOGGLE:
		def_ifmode = IFMODE_TOGGLE;
		break;
	default:
		printf("BUG %s(): %d\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}

	if (dev->unsafe_cfgable) {
		SET_MEMBER(dev->config, nchannel, dev->hw_nchannel, NONE, fp);
		SET_MEMBER(dev->config, nthread, dev->hw_nthread, NONE, fp);
		SET_MEMBER(dev->config, nlun, dev->hw_nlun, NONE, fp);
	}
	SET_MEMBER(dev->config, sector_size_shift, 12, " (9 512B, 12 4096B)", fp);
	SET_MEMBER(dev->config, sector_ncodeword, optimal_ncodeword(dev, 1 << dev->config->sector_size_shift), NONE, fp);
	SET_MEMBER(dev->config, user_sector_size_shift, 9, NONE, fp);
	SET_MEMBER(dev->config, nplane, (dev->flash->nplane <= dev->maxplanes) ? dev->flash->nplane : dev->maxplanes, NONE, fp);
	SET_MEMBER(dev->config, ifmode, def_ifmode, " (IO interface. 0 async, 1 toggle, 3 sync)", fp);
	SET_MEMBER(dev->config, async_cycles, 2, " (async mode timing. 2 3 or 4)", fp);
	SET_MEMBER(dev->config, ecc_mode, 0, " (0 enable, 1 disable, 2 only bypass scrambler)", fp);
	SET_MEMBER(dev->config, raid_mode, 1, " (0 disable, 1 enable)", fp);
	SET_MEMBER(dev->config, overprovision_rate, 20, NONE, fp);
}

static void silent_config(struct shannon_dev *dev)
{
	int def_ifmode;

	switch (dev->flash->ifmode) {
	case FLASH_IFMODE_ASYNC:
		def_ifmode = IFMODE_ASYNC;
		break;
	case FLASH_IFMODE_SYNC:
		def_ifmode = IFMODE_SYNC;
		break;
	case FLASH_IFMODE_TOGGLE:
		def_ifmode = IFMODE_TOGGLE;
		break;
	default:
		printf("BUG %s(): %d\n", __func__, __LINE__);
		exit(EXIT_FAILURE);
	}

	dev->config->nchannel			= dev->hw_nchannel;
	dev->config->nthread			= dev->hw_nthread;
	dev->config->nlun			= dev->hw_nlun;
	dev->config->sector_size_shift		= 12;
	dev->config->sector_ncodeword		= optimal_ncodeword(dev, 1 << dev->config->sector_size_shift);
	dev->config->user_sector_size_shift	= 9;
	dev->config->nplane			= (dev->flash->nplane <= dev->maxplanes) ? dev->flash->nplane : dev->maxplanes;
	if (dev->manual_nplane && dev->manual_nplane <= dev->config->nplane)
		dev->config->nplane = dev->manual_nplane;
	dev->config->ifmode			= def_ifmode;
	dev->config->async_cycles		= 2;
	dev->config->ecc_mode			= 0;
	if (dev->disable_ecc)
		dev->config->ecc_mode		= 1;
	dev->config->raid_mode			= 1;
	dev->config->overprovision_rate		= 20;

	calculate_indirect_config(dev);
}

static void print_config(struct shannon_dev *dev)
{
	GET_MEMBER(dev->config, nchannel);
	GET_MEMBER(dev->config, nthread);
	GET_MEMBER(dev->config, nlun);
	GET_MEMBER(dev->config, sector_size_shift);
	GET_MEMBER(dev->config, sector_ncodeword);
	GET_MEMBER(dev->config, user_sector_size_shift);
	GET_MEMBER(dev->config, nplane);
	GET_MEMBER(dev->config, ifmode);
	GET_MEMBER(dev->config, async_cycles);
	GET_MEMBER(dev->config, ecc_mode);
	GET_MEMBER(dev->config, raid_mode);
	GET_MEMBER(dev->config, overprovision_rate);
}

static int configfile_member(struct shannon_dev *dev, char *name, char *pv)
{
	if (dev->unsafe_cfgable) {
		PARSE_MEMBER(dev->config, nchannel, "nchannel", name, pv);
		PARSE_MEMBER(dev->config, nthread, "nthread", name, pv);
		PARSE_MEMBER(dev->config, nlun, "nlun", name, pv);
	}
	PARSE_MEMBER(dev->config, sector_size_shift, "sector_size_shift", name, pv);
	PARSE_MEMBER(dev->config, sector_ncodeword, "sector_ncodeword", name, pv);
	PARSE_MEMBER(dev->config, user_sector_size_shift, "user_sector_size_shift", name, pv);
	PARSE_MEMBER(dev->config, nplane, "nplane", name, pv);
	PARSE_MEMBER(dev->config, ifmode, "ifmode", name, pv);
	PARSE_MEMBER(dev->config, async_cycles, "async_cycles", name, pv);
	PARSE_MEMBER(dev->config, ecc_mode, "ecc_mode", name, pv);
	PARSE_MEMBER(dev->config, raid_mode, "raid_mode", name, pv);
	PARSE_MEMBER(dev->config, overprovision_rate, "overprovision_rate", name, pv);

	printf("can`t parse dev->config config: %s\n", name);
	return ERR;
}

/*
 * If file 'config' exist, parse it rather than parse interactive
 */
static int parse_config_from_file(struct shannon_dev *dev)
{
	FILE *fp;
	char line[256], *pm, *pv;

	if (NULL == (fp = fopen("config", "r")))
		return 1;

	while(NULL != fgets(line, sizeof(line), fp)) {
		if ('#' == line[0] || '\r' == line[0] || '\n' == line[0])
			continue;

		pm = pv = line;
		while (*pv != '=') pv++;
		*pv='\0';
		pv++;

		if (configfile_member(dev, pm, pv)) {
			fclose(fp);
			return 1;
		}
	}

	fclose(fp);
	if (dev->config->user_sector_size_shift > dev->config->sector_size_shift) {
		printf("ERR: user_sector_size > sector_size\n");
		return ERR;
	}
	return calculate_indirect_config(dev);
}

int parse_config(struct shannon_dev *dev)
{
	FILE *fp = NULL;
	char line[256];

	dev->config->hardware_version = dev->hw_sysinfo->firmware_tag;
	dev->config->software_version = 2;

	if (dev->silent_config) {
		silent_config(dev);
		goto end;
	}

	if (!dev->unsafe_cfgable) {
		dev->config->nchannel = dev->hw_nchannel;
		dev->config->nthread = dev->hw_nthread;
		dev->config->nlun = dev->hw_nlun;
	}

	/* parse config file*/
	if (!parse_config_from_file(dev))
		goto end;

	/* parse interactive */
	while (1) {
		fp = fopen("config", "w");
		if (NULL == fp) {
			printf("Create file 'config' failed\n");
			return ERR;
		}

		fprintf(fp, "#\n");
		fprintf(fp, "# Automatically generated file; DO NOT EDIT.\n");
		fprintf(fp, "# If you want to redo config, it is easy: delete this file.\n");
		fprintf(fp, "#\n");

		interactive_config(dev, fp);

		printf("\nYour input config is:\n");
		printf("=================================\n");
		print_config(dev);

		printf("=================================\n");
		printf("Is correct? [y/n/quit]: ");
		fgets(line, sizeof(line), stdin);

		if (line[0] == '\n' || line[0]== '\r' || tolower(line[0]) == 'y') {	/* Press ENTER or 'Y' means 'yes' */
			if (dev->config->user_sector_size_shift > dev->config->sector_size_shift) {
				printf("ERR: user_sector_size > sector_size\n");
				continue;
			}

			if (calculate_indirect_config(dev)) {
				printf("\nYou input invalid config, please check and config again...\n");
				fclose(fp);
				fp = NULL;
				continue;
			} else {
				break;	/* success return */
			};
		} else if (tolower(line[0]) == 'q') {
			fclose(fp);
			remove("config");
			return 1;
		} else {
			fclose(fp);
			fp = NULL;
			continue;
		}
	}

	fclose(fp);
end:
	optimal_ncodeword(dev, 1 << dev->config->sector_size_shift);
	if (dev->config->ecc_mode != ECCMODE_DISABLE && dev->config->sector_ncodeword != dev->config->optimal_ncodeword) {
		print("Flash: page_size=%d oob_size=%d entire_page_size=%d.\n"
			"Optimal codewords per sector is %d (codeword_size=%d page_nsector=%d).\n"
			"But your setting is %d (codeword_size=%d page_nsector=%d).\n"
			"Are you sure continue: ",
			dev->flash->page_size, dev->flash->oob_size, dev->flash->entire_page_size,
			dev->config->optimal_ncodeword, dev->config->optimal_codeword_size, dev->config->optimal_page_nsector,
			dev->config->sector_ncodeword, dev->config->codeword_size, dev->config->page_nsector);

		fgets(line, sizeof(line), stdin);
		if (strncmp(line, "force", 5))
			exit(EXIT_FAILURE);
	}

	if (dev->config->ecc_mode != ECCMODE_DISABLE && dev->config->page_nsector != dev->flash->page_size / dev->config->sector_size) {
		print("You will lose lots of capacity. page_size=%d oob_size=%d sector_size=%d sector_ncodeword=%d codeword_size=%d page_nsector=%d. "
			"Are you sure continue [yes|no]: ", dev->flash->page_size, dev->flash->oob_size,
			dev->config->sector_size, dev->config->sector_ncodeword, dev->config->codeword_size, dev->config->page_nsector);

		fgets(line, sizeof(line), stdin);
		if (strncmp(line, "yes", 3))
			exit(EXIT_FAILURE);
	}

	if (0 == dev->config->nchannel * dev->config->nthread * dev->config->nlun) {
		printf("Invalid HW paras: nchannel=%d nthread=%d nlun=%d. Remove config file then try again.\n",
			dev->config->nchannel, dev->config->nthread, dev->config->nlun);
		exit(EXIT_FAILURE);
	}

	if (0xFF == dev->config->nchannel) {
		printf("nchannel=0xFF, maybe hw encounter error\n");
		exit(EXIT_FAILURE);
	}

	/* check plane num config */
	if (dev->config->nplane > dev->flash->nplane) {
		printf("ERR: config plane number %d is larger than flash plane number %d\n", dev->config->nplane, dev->flash->nplane);
		exit(EXIT_FAILURE);
	}

	if (dev->config->nplane > dev->maxplanes) {
		printf("ERR: config plane number %d is larger than FPGA supported plane number %d\n", dev->config->nplane, dev->maxplanes);
		exit(EXIT_FAILURE);
	}

	if (!dev->silent_config && dev->config->nplane < dev->flash->nplane) {
		char line[256];

		print("Config nplane=%d will get optimal performance. But now nplane=%d. Use it anyway [yes|no]: ",
			dev->flash->nplane, dev->config->nplane);
		fgets(line, sizeof(line), stdin);
		if (strncmp(line, "yes", 3))
			exit(EXIT_FAILURE);
	}

	return 0;
}
#else
int parse_config(struct shannon_dev *dev)
{
	dev->config->nchannel = dev->hw_nchannel;
	dev->config->nthread = dev->hw_nthread;
	dev->config->nlun = dev->hw_nlun;
	dev->config->sector_size_shift = 12;
	dev->config->sector_ncodeword = optimal_ncodeword(dev, 1 << dev->config->sector_size_shift);
	dev->config->user_sector_size_shift = 9;
	dev->config->nplane = (dev->flash->nplane <= dev->maxplanes) ? dev->flash->nplane : dev->maxplanes;
	dev->config->ifmode = (FLASH_IFMODE_TOGGLE == dev->flash->ifmode) ? IFMODE_TOGGLE : IFMODE_SYNC;
	dev->config->async_cycles = 2;
	dev->config->ecc_mode = 0;
	dev->config->raid_mode = 1;
	dev->config->overprovision_rate = 20;
	dev->config->drvmode = dev->flash->drvsetting.data[2];
	dev->config->hardware_version = dev->hw_sysinfo->firmware_tag;
	dev->config->software_version = 2;

	if (calculate_indirect_config(dev)) {
		printf("config failed!\n");
		return ERR;
	}

	return 0;
}
#endif
